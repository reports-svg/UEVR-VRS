#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>
#include <utility/ScopeGuard.hpp>

#include "../Framework.hpp"
#include "../hooks/D3D12Hook.hpp"

#include "VRSInjector.hpp"

namespace render {
VRSInjector& VRSInjector::get() {
    static VRSInjector instance{};
    return instance;
}

void VRSInjector::register_rtv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (resource == nullptr || handle.ptr == 0) {
        return;
    }

    const auto desc = resource->GetDesc();

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return;
    }

    RtvInfo info{};
    info.resource = resource;
    info.width = (uint32_t)desc.Width;
    info.height = (uint32_t)desc.Height;

    std::unique_lock _{m_rtv_mtx};
    m_rtv_map[handle.ptr] = info;

    // Keep the map bounded. A blind clear would also evict the scene target's own
    // RTV (often created once, before the hook engaged, and never again),
    // silently breaking descriptor-based matching for the rest of the session.
    // Drop only entries that don't look like the current scene target first, and
    // fall back to a full reset (re-inserting the live entry) only if that still
    // isn't enough.
    if (m_rtv_map.size() > MAX_RTV_ENTRIES) {
        const auto match_w = m_match_width.load(std::memory_order_relaxed);
        const auto match_h = m_match_height.load(std::memory_order_relaxed);

        if (match_w != 0 && match_h != 0) {
            for (auto it = m_rtv_map.begin(); it != m_rtv_map.end();) {
                if (it->first != handle.ptr && (it->second.width != match_w || it->second.height != match_h)) {
                    it = m_rtv_map.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (m_rtv_map.size() > MAX_RTV_ENTRIES) {
            m_rtv_map.clear();
            m_rtv_map[handle.ptr] = info;
        }
    }
}

int VRSInjector::find_variant_for_target(uint32_t w, uint32_t h) const {
    for (size_t i = 0; i < MAX_VARIANTS; ++i) {
        const auto packed = m_variants[i].pub_dims.load(std::memory_order_relaxed);
        const auto vw = (uint32_t)(packed >> 32);
        const auto vh = (uint32_t)packed;

        if (vw == 0 || vh == 0) {
            continue;
        }

        const uint32_t w_tol = 2 + vw / 200;
        const uint32_t h_tol = 2 + vh / 200;

        if (w + w_tol >= vw && w <= vw + w_tol && h + h_tol >= vh && h <= vh + h_tol) {
            return (int)i;
        }
    }

    return -1;
}

bool VRSInjector::is_subres_scene_candidate(uint32_t w, uint32_t h) const {
    const auto pw = m_match_width.load(std::memory_order_relaxed);
    const auto ph = m_match_height.load(std::memory_order_relaxed);

    if (pw == 0 || ph == 0 || w == 0 || h == 0 || h >= ph) {
        return false;
    }

    const float sy = (float)h / (float)ph;

    // Floor of 0.51 keeps the half-resolution bloom/SSR chains (0.5x) out while
    // accepting DLSS/FSR Quality (0.667) and Balanced (0.58); ceiling of 0.97
    // keeps a clean separation from the display-resolution variant's tolerance.
    if (sy < 0.51f || sy > 0.97f) {
        return false;
    }

    const float sx_full = (float)w / (float)pw;

    return std::fabs(sx_full - sy) <= 0.015f;
}

void VRSInjector::request_subres_variant(uint32_t w, uint32_t h) {
    m_subres_request.store(((uint64_t)w << 32) | (uint64_t)h, std::memory_order_relaxed);
}

ID3D12Resource* VRSInjector::match_rtv_bind(
    UINT num_render_targets,
    const D3D12_CPU_DESCRIPTOR_HANDLE* render_targets,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil)
{
    if (num_render_targets == 0 || render_targets == nullptr || render_targets[0].ptr == 0) {
        return nullptr;
    }

    if (m_require_depth.load(std::memory_order_relaxed)) {
        if (depth_stencil == nullptr || depth_stencil->ptr == 0) {
            return nullptr;
        }
    }

    RtvInfo info{};

    {
        std::shared_lock _{m_rtv_mtx};
        const auto it = m_rtv_map.find(render_targets[0].ptr);

        if (it == m_rtv_map.end()) {
            return nullptr;
        }

        info = it->second;
    }

    if ((uintptr_t)info.resource == m_excluded_resource.load(std::memory_order_relaxed)) {
        return nullptr;
    }

    const bool has_depth = depth_stencil != nullptr && depth_stencil->ptr != 0;
    const auto vi = find_variant_for_target(info.width, info.height);

    if (vi >= 0) {
        auto& variant = m_variants[vi];
        const auto matched_dims = variant.pub_dims.load(std::memory_order_relaxed);

        if (vi == 0) {
            // The RTV path knows exact texture extents; require exact equality
            // for the display-resolution variant so a slightly-larger pooled
            // target can never be paired with a smaller tile grid.
            if (info.width != (uint32_t)(matched_dims >> 32) || info.height != (uint32_t)matched_dims) {
                return nullptr;
            }

            // While upscaler render-resolution activity is live, display-res
            // binds are post-upscale passes - coarse-shading those puts
            // blockiness straight into the final image.
            if (m_suppress_fullres.load(std::memory_order_relaxed)) {
                return nullptr;
            }
        }

        const auto sri = variant.sri.load(std::memory_order_acquire);

        // Reject a cross-retarget mismatch: if the dims changed while we loaded
        // the image, this sri may be scaled for a different resolution.
        if (sri == nullptr || variant.pub_dims.load(std::memory_order_relaxed) != matched_dims) {
            return nullptr;
        }

        variant.binds.fetch_add(1, std::memory_order_relaxed);

        if (vi == 0 && has_depth) {
            m_v0_depth_binds.fetch_add(1, std::memory_order_relaxed);
        }

        return sri;
    }

    // Variant discovery: only from here (true texture extents) and only with a
    // depth target bound - evidence of a geometry pass at that resolution.
    if (has_depth && m_allow_subres.load(std::memory_order_relaxed) && is_subres_scene_candidate(info.width, info.height)) {
        request_subres_variant(info.width, info.height);
    }

    return nullptr;
}

ID3D12Resource* VRSInjector::match_viewport_bind(const D3D12_VIEWPORT& vp) {
    const uint32_t vw = (uint32_t)(vp.Width + 0.5f);
    const uint32_t vh = (uint32_t)(vp.Height + 0.5f);

    if (vw == 0 || vh == 0) {
        return nullptr;
    }

    const bool double_wide = m_double_wide.load(std::memory_order_relaxed);
    const bool suppress_fullres = m_suppress_fullres.load(std::memory_order_relaxed);

    for (size_t i = 0; i < MAX_VARIANTS; ++i) {
        auto& variant = m_variants[i];
        const auto packed = variant.pub_dims.load(std::memory_order_relaxed);
        const auto tw = (uint32_t)(packed >> 32);
        const auto th = (uint32_t)packed;

        if (tw == 0 || th == 0) {
            continue;
        }

        // Height must line up with the target; small offscreen passes (shadow
        // maps, reflection captures, downsampled post) won't.
        const uint32_t h_tol = 2 + th / 200; // ~0.5% tolerance
        if (vh + h_tol < th || vh > th + h_tol) {
            continue;
        }

        const uint32_t w_tol = 2 + tw / 200;

        // Full-frame draw (both eyes in a double-wide target, or a mono target),
        // or a single-eye viewport into a double-wide target. The SRI is
        // full-width and maps 1:1 to the bound RT regardless of the viewport.
        bool matched = (vw + w_tol >= tw && vw <= tw + w_tol);

        if (!matched && double_wide) {
            const uint32_t half = tw / 2;
            matched = (vw + w_tol >= half && vw <= half + w_tol);
        }

        if (!matched) {
            continue;
        }

        if (i == 0 && suppress_fullres) {
            return nullptr;
        }

        const auto sri = variant.sri.load(std::memory_order_acquire);

        // Reject a cross-retarget mismatch (see match_rtv_bind).
        if (sri == nullptr || variant.pub_dims.load(std::memory_order_relaxed) != packed) {
            return nullptr;
        }

        variant.binds.fetch_add(1, std::memory_order_relaxed);
        return sri;
    }

    // No variant discovery from viewports: dims alone can't distinguish an
    // upscaler render resolution from a game's own fixed reduced-res pass
    // (separate translucency, water, scene captures). The RTV+depth path owns
    // discovery; this path only serves already-published variants.
    return nullptr;
}

ID3D12GraphicsCommandList5* VRSInjector::resolve_cl5(ID3D12GraphicsCommandList* command_list) {
    if (command_list == nullptr) {
        return nullptr;
    }

    // Never touch UEVR's own compositor command lists.
    const auto& hook = g_framework->get_d3d12_hook();

    if (hook != nullptr && hook->is_inside_present()) {
        return nullptr;
    }

    // All direct command lists on a given runtime either support
    // ID3D12GraphicsCommandList5 or none do, so probe once with QI and use a
    // static_cast afterwards (D3D12 implements every list interface on one
    // object with a single vtable).
    auto support = m_cl5_support.load(std::memory_order_relaxed);

    if (support == 0) {
        ID3D12GraphicsCommandList5* probe{nullptr};

        if (SUCCEEDED(command_list->QueryInterface(IID_PPV_ARGS(&probe))) && probe != nullptr) {
            probe->Release();
            support = 1;
        } else {
            support = -1;
            spdlog::error("[VRS] ID3D12GraphicsCommandList5 unsupported, injected VRS disabled");
        }

        m_cl5_support.store(support, std::memory_order_relaxed);
    }

    if (support < 0) {
        return nullptr;
    }

    return static_cast<ID3D12GraphicsCommandList5*>(command_list);
}

void VRSInjector::bind_sri(ID3D12GraphicsCommandList5* cl5, ID3D12Resource* sri) {
    // Pipeline/per-primitive rate stays 1x1, the screen-space image wins.
    static const D3D12_SHADING_RATE_COMBINER combiners[2]{
        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
        D3D12_SHADING_RATE_COMBINER_OVERRIDE,
    };

    cl5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
    cl5->RSSetShadingRateImage(sri);
    m_stat_vrs_binds.fetch_add(1, std::memory_order_relaxed);
}

void VRSInjector::unbind_sri(ID3D12GraphicsCommandList5* cl5) {
    cl5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
    cl5->RSSetShadingRateImage(nullptr);
}

namespace {
// A D3D12 command list is only ever recorded by one thread at a time, so this
// per-thread record of "which list (if any) we currently have the shading-rate
// image bound on" needs no synchronization. It lets the viewport path clear the
// image only on lists we actually set it on - never clobbering a game's own
// D3D12 VRS state - and lets us take the image back off if the injector
// deactivates mid-recording.
thread_local ID3D12GraphicsCommandList* g_tl_sri_bound_cl = nullptr;
} // namespace

void VRSInjector::on_om_set_render_targets(
    ID3D12GraphicsCommandList* command_list,
    UINT num_render_targets,
    const D3D12_CPU_DESCRIPTOR_HANDLE* render_targets,
    BOOL single_handle_to_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil)
{
    (void)single_handle_to_range;

    if (command_list == nullptr) {
        return;
    }

    // Cheap relaxed peek so a disabled injector costs the detour only one load.
    // This path only ever binds, so there is nothing to do when inactive.
    if (!m_any_active.load(std::memory_order_relaxed)) {
        return;
    }

    // Hold a recording ref across the load+use of the variant SRIs so
    // on_device_reset cannot free a texture out from under us. The seq-cst here
    // pairs with the seq-cst store + drain in deactivate()/on_device_reset().
    m_recording_refs.fetch_add(1, std::memory_order_seq_cst);
    utility::ScopeGuard rec_guard{[this]() {
        m_recording_refs.fetch_sub(1, std::memory_order_release);
    }};

    // Positive-only: only bind on a confirmed scene-target RTV match. Clearing
    // is owned by the viewport path, which is called after OMSetRenderTargets
    // and before the draws for every UE pass.
    const auto sri = match_rtv_bind(num_render_targets, render_targets, depth_stencil);

    if (sri == nullptr) {
        return;
    }

    auto* cl5 = resolve_cl5(command_list);

    if (cl5 == nullptr) {
        return;
    }

    bind_sri(cl5, sri);
    g_tl_sri_bound_cl = command_list;
}

void VRSInjector::on_rs_set_viewports(
    ID3D12GraphicsCommandList* command_list,
    UINT num_viewports,
    const D3D12_VIEWPORT* viewports)
{
    if (command_list == nullptr || num_viewports == 0 || viewports == nullptr) {
        return;
    }

    const bool holds_state = (g_tl_sri_bound_cl == command_list);

    // Cheap relaxed peek: nothing published and no image of ours left on this
    // list means there is nothing to bind and nothing to clear.
    if (!m_any_active.load(std::memory_order_relaxed) && !holds_state) {
        return;
    }

    m_recording_refs.fetch_add(1, std::memory_order_seq_cst);
    utility::ScopeGuard rec_guard{[this]() {
        m_recording_refs.fetch_sub(1, std::memory_order_release);
    }};

    auto* cl5 = resolve_cl5(command_list);

    if (cl5 == nullptr) {
        return;
    }

    m_stat_rtv_binds.fetch_add(1, std::memory_order_relaxed);

    const auto sri = match_viewport_bind(viewports[0]);

    if (sri != nullptr) {
        bind_sri(cl5, sri);
        g_tl_sri_bound_cl = command_list;
    } else if (g_tl_sri_bound_cl == command_list) {
        // Not a scene viewport (or the injector just deactivated), and we
        // previously bound the image on this list: take it back off so later
        // passes aren't coarse-shaded. Lists we never touched keep whatever
        // shading-rate state the game set.
        unbind_sri(cl5);
        g_tl_sri_bound_cl = nullptr;
    }
}

bool VRSInjector::query_caps(ID3D12Device* device) {
    if (device == m_caps_device && m_caps.queried) {
        return m_caps.tier2;
    }

    m_caps = Caps{};
    m_caps_device = device;

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};

    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6)))) {
        m_caps.queried = true;
        m_caps.tier1 = options6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
        m_caps.tier2 = options6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_2;
        m_caps.additional_rates = options6.AdditionalShadingRatesSupported != FALSE;

        if (options6.ShadingRateImageTileSize >= 8 && options6.ShadingRateImageTileSize <= 64) {
            m_caps.tile_size = options6.ShadingRateImageTileSize;
        }

        spdlog::info("[VRS] Caps: tier1={} tier2={} additional_rates={} tile_size={}",
            m_caps.tier1, m_caps.tier2, m_caps.additional_rates, m_caps.tile_size);
    } else {
        m_caps.queried = true;
        spdlog::warn("[VRS] CheckFeatureSupport(D3D12_OPTIONS6) failed, VRS unavailable");
    }

    return m_caps.tier2;
}

namespace {
// Pattern-relevant fields only: target dimensions are per-variant and tracked
// separately, so they are deliberately absent here.
bool pattern_equivalent(const VRSInjector::FoveationDesc& a, const VRSInjector::FoveationDesc& b) {
    constexpr float EPS = 0.0025f;

    return a.double_wide == b.double_wide &&
        a.allow_4x4 == b.allow_4x4 &&
        a.lens_mask == b.lens_mask &&
        std::fabs(a.full_rate_cutoff - b.full_rate_cutoff) < EPS &&
        std::fabs(a.half_rate_cutoff - b.half_rate_cutoff) < EPS &&
        std::fabs(a.center_u[0] - b.center_u[0]) < EPS &&
        std::fabs(a.center_v[0] - b.center_v[0]) < EPS &&
        std::fabs(a.center_u[1] - b.center_u[1]) < EPS &&
        std::fabs(a.center_v[1] - b.center_v[1]) < EPS &&
        std::fabs(a.lens_u[0] - b.lens_u[0]) < EPS &&
        std::fabs(a.lens_v[0] - b.lens_v[0]) < EPS &&
        std::fabs(a.lens_u[1] - b.lens_u[1]) < EPS &&
        std::fabs(a.lens_v[1] - b.lens_v[1]) < EPS &&
        std::fabs(a.lens_rx - b.lens_rx) < EPS &&
        std::fabs(a.lens_ry - b.lens_ry) < EPS;
}
} // namespace

void VRSInjector::unpublish_variant(Variant& variant) {
    variant.sri.store(nullptr, std::memory_order_seq_cst);
    variant.pub_dims.store(0, std::memory_order_relaxed);
}

void VRSInjector::deactivate() {
    // seq-cst so it pairs with the seq-cst increment in the recording paths:
    // on_device_reset() can then drain m_recording_refs and know that any thread
    // still holding a non-null image has been counted.
    for (auto& variant : m_variants) {
        unpublish_variant(variant);
    }

    m_any_active.store(false, std::memory_order_seq_cst);
    m_suppress_fullres.store(false, std::memory_order_relaxed);
    m_suppress_countdown = 0;
    m_match_width.store(0, std::memory_order_relaxed);
    m_match_height.store(0, std::memory_order_relaxed);
}

void VRSInjector::retire(Microsoft::WRL::ComPtr<ID3D12Resource> resource) {
    if (resource != nullptr) {
        // Held for a generous number of present-thread updates so any command
        // list recorded against the old image has long been submitted and
        // retired by the GPU (updates only advance while frames are flowing, so
        // a stalled game cannot age this out prematurely).
        m_retired.emplace_back(16u, std::move(resource));
    }
}

void VRSInjector::update(const FoveationDesc& desc) {
    std::scoped_lock _{m_update_mtx};

    for (auto it = m_retired.begin(); it != m_retired.end();) {
        if (it->first == 0 || --it->first == 0) {
            it = m_retired.erase(it);
        } else {
            ++it;
        }
    }

    m_last_stats.rtv_binds = m_stat_rtv_binds.exchange(0, std::memory_order_relaxed);
    m_last_stats.vrs_binds = m_stat_vrs_binds.exchange(0, std::memory_order_relaxed);
    m_last_stats.sri_updates = m_sri_updates;
    m_last_stats.subres_binds = 0;
    m_last_stats.subres_width = 0;
    m_last_stats.subres_height = 0;
    m_last_stats.suppressing_fullres = false;
    m_last_stats.active = false;

    // Periodic bind-rate log so activity is visible without the ImGui panel.
    if (++m_update_log_counter >= 120) {
        m_update_log_counter = 0;
        spdlog::info("[VRS] binds/window: candidates={} applied={} sri_updates={} scene={}x{} double_wide={} suppress_fullres={}",
            m_last_stats.rtv_binds, m_last_stats.vrs_binds, m_sri_updates,
            desc.scene_width, desc.scene_height, desc.double_wide,
            m_suppress_countdown > 0);
    }

    const auto& hook = g_framework->get_d3d12_hook();

    if (!desc.enabled || hook == nullptr || desc.scene_width == 0 || desc.scene_height == 0) {
        deactivate();
        return;
    }

    auto device = hook->get_device();

    if (device == nullptr || !query_caps(device)) {
        deactivate();
        return;
    }

    // Publish the cheap hot-path parameters every frame.
    m_require_depth.store(desc.require_depth, std::memory_order_relaxed);
    m_excluded_resource.store(desc.ui_target, std::memory_order_relaxed);
    m_double_wide.store(desc.double_wide, std::memory_order_relaxed);
    m_allow_subres.store(desc.allow_subres, std::memory_order_relaxed);
    m_match_width.store(desc.scene_width, std::memory_order_relaxed);
    m_match_height.store(desc.scene_height, std::memory_order_relaxed);

    if (!m_has_last_desc || !pattern_equivalent(desc, m_last_desc)) {
        ++m_pattern_serial;
    }

    m_last_desc = desc;
    m_has_last_desc = true;

    // Variant 0 always serves the display-resolution scene target.
    m_variants[0].target_width = desc.scene_width;
    m_variants[0].target_height = desc.scene_height;

    // Adopt a pending upscaler render-resolution request.
    if (const auto req = m_subres_request.exchange(0, std::memory_order_relaxed); req != 0 && desc.allow_subres) {
        const uint32_t req_w = (uint32_t)(req >> 32);
        const uint32_t req_h = (uint32_t)(req & 0xffffffffull);

        if (find_variant_for_target(req_w, req_h) < 0 && is_subres_scene_candidate(req_w, req_h)) {
            Variant* slot = nullptr;

            // Prefer re-targeting a nearby variant (dynamic-resolution drift),
            // then a free slot, then the longest-idle one.
            for (size_t i = 1; i < MAX_VARIANTS; ++i) {
                auto& v = m_variants[i];

                if (v.target_width != 0 &&
                    std::fabs((float)v.target_width - (float)req_w) <= (float)req_w * 0.04f &&
                    std::fabs((float)v.target_height - (float)req_h) <= (float)req_h * 0.04f) {
                    slot = &v;
                    break;
                }
            }

            if (slot == nullptr) {
                for (size_t i = 1; i < MAX_VARIANTS; ++i) {
                    if (m_variants[i].target_width == 0) {
                        slot = &m_variants[i];
                        break;
                    }
                }
            }

            if (slot == nullptr) {
                slot = &m_variants[1];

                for (size_t i = 2; i < MAX_VARIANTS; ++i) {
                    if (m_variants[i].idle_updates > slot->idle_updates) {
                        slot = &m_variants[i];
                    }
                }

                unpublish_variant(*slot);
            }

            if (slot->target_width != req_w || slot->target_height != req_h) {
                spdlog::info("[VRS] Upscaler render resolution detected: {}x{} (display {}x{})",
                    req_w, req_h, desc.scene_width, desc.scene_height);
            }

            slot->target_width = req_w;
            slot->target_height = req_h;
            slot->idle_updates = 0;
        }
    }

    bool any_active = false;
    uint32_t subres_binds = 0;

    for (size_t i = 0; i < MAX_VARIANTS; ++i) {
        auto& variant = m_variants[i];
        const bool is_subres = i != 0;

        if (variant.target_width == 0 || variant.target_height == 0) {
            continue;
        }

        const auto binds_last = variant.binds.exchange(0, std::memory_order_relaxed);

        if (is_subres) {
            subres_binds += binds_last;

            if (binds_last == 0) {
                if (++variant.idle_updates > VARIANT_IDLE_UPDATES) {
                    unpublish_variant(variant);
                    variant.target_width = 0;
                    variant.target_height = 0;
                    variant.pattern_serial = 0;
                    continue;
                }
            } else {
                variant.idle_updates = 0;
            }
        }

        const auto want_dims = ((uint64_t)variant.target_width << 32) | (uint64_t)variant.target_height;
        const bool published = variant.sri.load(std::memory_order_relaxed) != nullptr &&
            variant.pub_dims.load(std::memory_order_relaxed) == want_dims;

        if (!published || variant.pattern_serial != m_pattern_serial) {
            if (generate_and_upload(device, desc, variant)) {
                variant.pattern_serial = m_pattern_serial;
            } else if (i == 0) {
                deactivate();
                return;
            } else {
                unpublish_variant(variant);
                variant.target_width = 0;
                variant.target_height = 0;
                continue;
            }
        }

        if (variant.sri.load(std::memory_order_relaxed) != nullptr) {
            any_active = true;

            if (is_subres && m_last_stats.subres_width == 0) {
                m_last_stats.subres_width = variant.target_width;
                m_last_stats.subres_height = variant.target_height;
            }
        }
    }

    // Suppress display-resolution binds while upscaler render-resolution scene
    // activity is live (those binds are post-upscale passes), with a linger so
    // per-frame pass ordering jitter doesn't flap the state. Depth-bound binds
    // at the display resolution are proof the game still renders real geometry
    // full-res (i.e. the sub-res activity is the game's own reduced-res pass,
    // like separate translucency - NOT an upscaler), so they veto suppression.
    const auto v0_depth_binds = m_v0_depth_binds.exchange(0, std::memory_order_relaxed);

    if (subres_binds > 0 && v0_depth_binds == 0) {
        m_suppress_countdown = SUPPRESS_LINGER_UPDATES;
    } else if (m_suppress_countdown > 0) {
        --m_suppress_countdown;
    }

    m_suppress_fullres.store(m_suppress_countdown > 0, std::memory_order_relaxed);

    m_any_active.store(any_active, std::memory_order_release);

    const auto& v0_slot = m_variants[0].ring[(m_variants[0].ring_index + SRI_RING_SIZE - 1) % SRI_RING_SIZE];
    m_last_stats.sri_width = v0_slot.width;
    m_last_stats.sri_height = v0_slot.height;
    m_last_stats.subres_binds = subres_binds;
    m_last_stats.suppressing_fullres = m_suppress_countdown > 0;
    m_last_stats.active = any_active;
}

bool VRSInjector::generate_and_upload(ID3D12Device* device, const FoveationDesc& desc, Variant& variant) {
    const uint32_t target_w = variant.target_width;
    const uint32_t target_h = variant.target_height;
    const bool is_subres = &variant != &m_variants[0];

    const auto tile = m_caps.tile_size;
    // The viewport path can bind this SRI to a render target that is slightly
    // larger than the matched viewport (pooled/padded targets), so pad the tile
    // grid to cover the full matching tolerance - an SRI larger than the target
    // is fine, one smaller than its tile grid is undefined behavior. Tiles past
    // the real edge simply never apply.
    (void)is_subres;
    const uint32_t w_margin_px = 2 + target_w / 200;
    const uint32_t h_margin_px = 2 + target_h / 200;
    const uint32_t sri_w = (target_w + w_margin_px + tile - 1) / tile + 1;
    const uint32_t sri_h = (target_h + h_margin_px + tile - 1) / tile + 1;

    const size_t slot_index = variant.ring_index;
    variant.ring_index = (variant.ring_index + 1) % SRI_RING_SIZE;

    auto& slot = variant.ring[slot_index];
    auto& ctx = variant.upload_ctx[slot_index];

    if (!variant.upload_ctx_ready[slot_index]) {
        if (!ctx.setup(L"VRSInjector upload context")) {
            spdlog::error("[VRS] Failed to set up upload command context");
            return false;
        }

        variant.upload_ctx_ready[slot_index] = true;
    }

    // This slot's own previous upload is SRI_RING_SIZE regenerations old, so the
    // GPU has virtually always finished and this returns immediately - unlike a
    // single shared context, which would block the present thread on the frame it
    // submitted last (a per-frame CPU/GPU serialization under eye-tracked gaze).
    ctx.wait(2000);

    const uint32_t row_pitch = (sri_w + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    if (slot.texture == nullptr || slot.width != sri_w || slot.height != sri_h) {
        // A command list recorded a few frames ago may still reference the old
        // image; retire it instead of releasing it out from under the GPU.
        retire(std::move(slot.texture));
        retire(std::move(slot.upload));
        slot.texture.Reset();
        slot.upload.Reset();
        slot.in_shading_rate_state = false;

        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC tex_desc{};
        tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex_desc.Width = sri_w;
        tex_desc.Height = sri_h;
        tex_desc.DepthOrArraySize = 1;
        tex_desc.MipLevels = 1;
        tex_desc.Format = DXGI_FORMAT_R8_UINT;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &tex_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.texture)))) {
            spdlog::error("[VRS] Failed to create shading-rate image ({}x{})", sri_w, sri_h);
            return false;
        }

        slot.texture->SetName(L"UEVR VRS shading-rate image");

        D3D12_HEAP_PROPERTIES upload_props{};
        upload_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC upload_desc{};
        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Width = (uint64_t)row_pitch * sri_h;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.Format = DXGI_FORMAT_UNKNOWN;
        upload_desc.SampleDesc.Count = 1;
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE, &upload_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&slot.upload)))) {
            spdlog::error("[VRS] Failed to create shading-rate upload buffer");
            slot.texture.Reset();
            return false;
        }

        slot.upload->SetName(L"UEVR VRS upload buffer");
        slot.width = sri_w;
        slot.height = sri_h;
    }

    // Fill the upload buffer with the foveation pattern. Rates are literal
    // D3D12_SHADING_RATE palette values, matching UE's VRSShadingRateFoveated.usf:
    // concentric rings measured in squared distance normalized by the squared
    // per-eye half-diagonal. The pattern is generated in this variant's target
    // pixel space, so upscaler render resolutions get correctly-scaled rings.
    {
        uint8_t* mapped{nullptr};
        const D3D12_RANGE no_read{0, 0};

        if (FAILED(slot.upload->Map(0, &no_read, (void**)&mapped)) || mapped == nullptr) {
            spdlog::error("[VRS] Failed to map shading-rate upload buffer");
            return false;
        }

        const uint32_t num_eyes = desc.double_wide ? 2 : 1;
        const float tile_f = (float)tile;
        const float eye_w_px = (float)target_w / (float)num_eyes;
        const float h_px = (float)target_h;
        const float half_diag_sq = (eye_w_px * 0.5f) * (eye_w_px * 0.5f) + (h_px * 0.5f) * (h_px * 0.5f);
        const float full_cutoff_sq = desc.full_rate_cutoff * desc.full_rate_cutoff;
        const float half_cutoff_sq = desc.half_rate_cutoff * desc.half_rate_cutoff;
        const uint8_t outer_rate = (desc.allow_4x4 && m_caps.additional_rates)
            ? (uint8_t)D3D12_SHADING_RATE_4X4 : (uint8_t)D3D12_SHADING_RATE_2X2;

        const bool lens = desc.lens_mask && desc.lens_rx > 0.01f && desc.lens_ry > 0.01f;

        for (uint32_t ty = 0; ty < sri_h; ++ty) {
            uint8_t* row = mapped + (size_t)ty * row_pitch;
            const float py = ((float)ty + 0.5f) * tile_f;

            for (uint32_t tx = 0; tx < sri_w; ++tx) {
                const float px = ((float)tx + 0.5f) * tile_f;
                const uint32_t eye = (num_eyes == 2 && px >= eye_w_px) ? 1 : 0;
                const float cx = (float)eye * eye_w_px + desc.center_u[eye] * eye_w_px;
                const float cy = desc.center_v[eye] * h_px;
                const float dx = px - cx;
                const float dy = py - cy;
                const float d2 = (dx * dx + dy * dy) / half_diag_sq;

                uint8_t rate = (uint8_t)D3D12_SHADING_RATE_1X1;

                if (d2 > half_cutoff_sq) {
                    rate = outer_rate;
                } else if (d2 > full_cutoff_sq) {
                    rate = (uint8_t)D3D12_SHADING_RATE_2X2;
                }

                // Lens mask: outside the lens-visible ellipse (anchored at the
                // optical center, not the gaze), drop straight to the coarsest
                // ring rate - the lens cannot resolve these pixels anyway.
                if (lens && rate != outer_rate) {
                    const float lcx = (float)eye * eye_w_px + desc.lens_u[eye] * eye_w_px;
                    const float lcy = desc.lens_v[eye] * h_px;
                    const float ldx = (px - lcx) / (desc.lens_rx * eye_w_px * 0.5f);
                    const float ldy = (py - lcy) / (desc.lens_ry * h_px * 0.5f);

                    if (ldx * ldx + ldy * ldy > 1.0f) {
                        rate = outer_rate;
                    }
                }

                row[tx] = rate;
            }
        }

        slot.upload->Unmap(0, nullptr);
    }

    // Record: (SRI -> COPY_DEST if needed), copy, SRI -> SHADING_RATE_SOURCE.
    {
        std::scoped_lock _{ctx.mtx};
        auto cmd_list = ctx.cmd_list.Get();

        if (cmd_list == nullptr) {
            return false;
        }

        if (slot.in_shading_rate_state) {
            D3D12_RESOURCE_BARRIER to_copy{};
            to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_copy.Transition.pResource = slot.texture.Get();
            to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
            to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            cmd_list->ResourceBarrier(1, &to_copy);
        }

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = slot.texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = slot.upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UINT;
        src.PlacedFootprint.Footprint.Width = sri_w;
        src.PlacedFootprint.Footprint.Height = sri_h;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = row_pitch;

        cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER to_sri{};
        to_sri.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_sri.Transition.pResource = slot.texture.Get();
        to_sri.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_sri.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_sri.Transition.StateAfter = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
        cmd_list->ResourceBarrier(1, &to_sri);

        ctx.has_commands = true;
    }

    ctx.execute();
    slot.in_shading_rate_state = true;
    ++m_sri_updates;

    // Publish, dims first then the image (matchers load dims, then sri, then
    // re-check dims - so a torn pairing is always rejected). Command lists
    // recorded from now on use the new image; ones in flight keep referencing
    // an older ring slot, which stays alive.
    variant.pub_dims.store(((uint64_t)target_w << 32) | (uint64_t)target_h, std::memory_order_relaxed);
    variant.sri.store(slot.texture.Get(), std::memory_order_release);

    return true;
}

VRSInjector::Stats VRSInjector::get_stats() const {
    std::scoped_lock _{m_update_mtx};
    return m_last_stats;
}

void VRSInjector::on_device_reset() {
    std::scoped_lock _{m_update_mtx};

    deactivate();

    // deactivate() published null SRIs (seq-cst). Wait for any recording thread
    // that already loaded an old pointer to finish using it before releasing the
    // textures, so RSSetShadingRateImage can never run on a freed resource. The
    // seq-cst handshake guarantees a thread still holding a non-null image is
    // observed here as a live ref.
    for (uint32_t spins = 0; m_recording_refs.load(std::memory_order_seq_cst) != 0; ++spins) {
        if (spins >= 2000000u) {
            spdlog::warn("[VRS] on_device_reset: recording refs did not drain, proceeding");
            break;
        }

        std::this_thread::yield();
    }

    {
        std::unique_lock rtv_lock{m_rtv_mtx};
        m_rtv_map.clear();
    }

    for (auto& variant : m_variants) {
        for (auto& slot : variant.ring) {
            slot.texture.Reset();
            slot.upload.Reset();
            slot.width = 0;
            slot.height = 0;
            slot.in_shading_rate_state = false;
        }

        for (size_t i = 0; i < SRI_RING_SIZE; ++i) {
            variant.upload_ctx[i].reset();
            variant.upload_ctx_ready[i] = false;
        }

        variant.target_width = 0;
        variant.target_height = 0;
        variant.idle_updates = 0;
        variant.pattern_serial = 0;
        variant.ring_index = 0;
        variant.binds.store(0, std::memory_order_relaxed);
    }

    m_retired.clear();
    m_subres_request.store(0, std::memory_order_relaxed);
    m_v0_depth_binds.store(0, std::memory_order_relaxed);
    m_caps = Caps{};
    m_caps_device = nullptr;
    m_has_last_desc = false;
    m_cl5_support.store(0, std::memory_order_relaxed);
}
} // namespace render
