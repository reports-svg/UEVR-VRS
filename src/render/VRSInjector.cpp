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

bool VRSInjector::matches_scene_target(
    UINT num_render_targets,
    const D3D12_CPU_DESCRIPTOR_HANDLE* render_targets,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil) const
{
    if (num_render_targets == 0 || render_targets == nullptr || render_targets[0].ptr == 0) {
        return false;
    }

    if (m_require_depth.load(std::memory_order_relaxed)) {
        if (depth_stencil == nullptr || depth_stencil->ptr == 0) {
            return false;
        }
    }

    const auto match_w = m_match_width.load(std::memory_order_relaxed);
    const auto match_h = m_match_height.load(std::memory_order_relaxed);

    if (match_w == 0 || match_h == 0) {
        return false;
    }

    RtvInfo info{};

    {
        std::shared_lock _{m_rtv_mtx};
        const auto it = m_rtv_map.find(render_targets[0].ptr);

        if (it == m_rtv_map.end()) {
            return false;
        }

        info = it->second;
    }

    if (info.width != match_w || info.height != match_h) {
        return false;
    }

    if ((uintptr_t)info.resource == m_excluded_resource.load(std::memory_order_relaxed)) {
        return false;
    }

    return true;
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
    if (m_active_sri.load(std::memory_order_relaxed) == nullptr) {
        return;
    }

    // Hold a recording ref across the load+use of m_active_sri so on_device_reset
    // cannot free the texture out from under us. The seq-cst here pairs with the
    // seq-cst store + drain in deactivate()/on_device_reset().
    m_recording_refs.fetch_add(1, std::memory_order_seq_cst);
    utility::ScopeGuard rec_guard{[this]() {
        m_recording_refs.fetch_sub(1, std::memory_order_release);
    }};

    const auto sri = m_active_sri.load(std::memory_order_seq_cst);

    if (sri == nullptr) {
        return; // raced with deactivation
    }

    // Positive-only: only bind on a confirmed scene-target RTV match. Clearing
    // is owned by the viewport path, which is called after OMSetRenderTargets
    // and before the draws for every UE pass.
    if (!matches_scene_target(num_render_targets, render_targets, depth_stencil)) {
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
    if (m_active_sri.load(std::memory_order_relaxed) == nullptr && !holds_state) {
        return;
    }

    m_recording_refs.fetch_add(1, std::memory_order_seq_cst);
    utility::ScopeGuard rec_guard{[this]() {
        m_recording_refs.fetch_sub(1, std::memory_order_release);
    }};

    const auto sri = m_active_sri.load(std::memory_order_seq_cst);

    auto* cl5 = resolve_cl5(command_list);

    if (cl5 == nullptr) {
        return;
    }

    m_stat_rtv_binds.fetch_add(1, std::memory_order_relaxed);

    if (sri != nullptr && viewport_matches_scene(viewports[0])) {
        bind_sri(cl5, sri);
        g_tl_sri_bound_cl = command_list;
    } else if (g_tl_sri_bound_cl == command_list) {
        // Not the scene viewport (or the injector just deactivated), and we
        // previously bound the image on this list: take it back off so later
        // passes aren't coarse-shaded. Lists we never touched keep whatever
        // shading-rate state the game set.
        unbind_sri(cl5);
        g_tl_sri_bound_cl = nullptr;
    }
}

bool VRSInjector::viewport_matches_scene(const D3D12_VIEWPORT& vp) const {
    const auto scene_w = m_match_width.load(std::memory_order_relaxed);
    const auto scene_h = m_match_height.load(std::memory_order_relaxed);

    if (scene_w == 0 || scene_h == 0) {
        return false;
    }

    const uint32_t vw = (uint32_t)(vp.Width + 0.5f);
    const uint32_t vh = (uint32_t)(vp.Height + 0.5f);

    // Height must line up with the scene target; small offscreen passes (shadow
    // maps, reflection captures, downsampled post) won't.
    const uint32_t h_tol = 2 + scene_h / 200; // ~0.5% tolerance
    if (vh + h_tol < scene_h || vh > scene_h + h_tol) {
        return false;
    }

    const uint32_t w_tol = 2 + scene_w / 200;

    // Full-frame draw (both eyes in a double-wide target, or a mono target).
    if (vw + w_tol >= scene_w && vw <= scene_w + w_tol) {
        return true;
    }

    // Single-eye viewport into a double-wide target (per-eye pass). The SRI is
    // full-width and maps 1:1 to the bound RT regardless of the viewport.
    if (m_double_wide.load(std::memory_order_relaxed)) {
        const uint32_t half = scene_w / 2;
        if (vw + w_tol >= half && vw <= half + w_tol) {
            return true;
        }
    }

    return false;
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
bool foveation_desc_equivalent(const VRSInjector::FoveationDesc& a, const VRSInjector::FoveationDesc& b) {
    constexpr float EPS = 0.0025f;

    return a.double_wide == b.double_wide &&
        a.allow_4x4 == b.allow_4x4 &&
        a.scene_width == b.scene_width &&
        a.scene_height == b.scene_height &&
        std::fabs(a.full_rate_cutoff - b.full_rate_cutoff) < EPS &&
        std::fabs(a.half_rate_cutoff - b.half_rate_cutoff) < EPS &&
        std::fabs(a.center_u[0] - b.center_u[0]) < EPS &&
        std::fabs(a.center_v[0] - b.center_v[0]) < EPS &&
        std::fabs(a.center_u[1] - b.center_u[1]) < EPS &&
        std::fabs(a.center_v[1] - b.center_v[1]) < EPS;
}
} // namespace

void VRSInjector::deactivate() {
    // seq-cst so it pairs with the seq-cst increment in the recording paths:
    // on_device_reset() can then drain m_recording_refs and know that any thread
    // still holding a non-null image has been counted.
    m_active_sri.store(nullptr, std::memory_order_seq_cst);
    m_match_width.store(0, std::memory_order_relaxed);
    m_match_height.store(0, std::memory_order_relaxed);
}

void VRSInjector::update(const FoveationDesc& desc) {
    std::scoped_lock _{m_update_mtx};

    m_last_stats.rtv_binds = m_stat_rtv_binds.exchange(0, std::memory_order_relaxed);
    m_last_stats.vrs_binds = m_stat_vrs_binds.exchange(0, std::memory_order_relaxed);
    m_last_stats.sri_updates = m_sri_updates;
    m_last_stats.active = false;

    // Periodic bind-rate log so activity is visible without the ImGui panel.
    if (++m_update_log_counter >= 120) {
        m_update_log_counter = 0;
        spdlog::info("[VRS] binds/window: candidates={} applied={} sri_updates={} scene={}x{} double_wide={}",
            m_last_stats.rtv_binds, m_last_stats.vrs_binds, m_sri_updates,
            desc.scene_width, desc.scene_height, desc.double_wide);
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

    const auto tile = m_caps.tile_size;
    const uint32_t sri_w = (desc.scene_width + tile - 1) / tile;
    const uint32_t sri_h = (desc.scene_height + tile - 1) / tile;

    m_last_stats.sri_width = sri_w;
    m_last_stats.sri_height = sri_h;

    // Publish the cheap hot-path parameters every frame.
    m_require_depth.store(desc.require_depth, std::memory_order_relaxed);
    m_excluded_resource.store(desc.ui_target, std::memory_order_relaxed);
    m_double_wide.store(desc.double_wide, std::memory_order_relaxed);

    const bool needs_regen = !m_has_last_desc || !foveation_desc_equivalent(desc, m_last_desc) ||
        m_active_sri.load(std::memory_order_relaxed) == nullptr;

    if (needs_regen) {
        if (!generate_and_upload(device, desc, sri_w, sri_h)) {
            deactivate();
            return;
        }

        m_last_desc = desc;
        m_has_last_desc = true;
    }

    m_match_width.store(desc.scene_width, std::memory_order_relaxed);
    m_match_height.store(desc.scene_height, std::memory_order_relaxed);
    m_last_stats.active = m_active_sri.load(std::memory_order_relaxed) != nullptr;
}

bool VRSInjector::generate_and_upload(ID3D12Device* device, const FoveationDesc& desc, uint32_t sri_w, uint32_t sri_h) {
    const size_t slot_index = m_sri_index;
    m_sri_index = (m_sri_index + 1) % SRI_RING_SIZE;

    auto& slot = m_sri_ring[slot_index];
    auto& ctx = m_upload_ctx[slot_index];

    if (!m_upload_ctx_ready[slot_index]) {
        if (!ctx.setup(L"VRSInjector upload context")) {
            spdlog::error("[VRS] Failed to set up upload command context");
            return false;
        }

        m_upload_ctx_ready[slot_index] = true;
    }

    // This slot's own previous upload is SRI_RING_SIZE regenerations old, so the
    // GPU has virtually always finished and this returns immediately - unlike a
    // single shared context, which would block the present thread on the frame it
    // submitted last (a per-frame CPU/GPU serialization under eye-tracked gaze).
    ctx.wait(2000);

    const uint32_t row_pitch = (sri_w + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    if (slot.texture == nullptr || slot.width != sri_w || slot.height != sri_h) {
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
    // per-eye half-diagonal.
    {
        uint8_t* mapped{nullptr};
        const D3D12_RANGE no_read{0, 0};

        if (FAILED(slot.upload->Map(0, &no_read, (void**)&mapped)) || mapped == nullptr) {
            spdlog::error("[VRS] Failed to map shading-rate upload buffer");
            return false;
        }

        const uint32_t num_eyes = desc.double_wide ? 2 : 1;
        const float tile = (float)m_caps.tile_size;
        const float eye_w_px = (float)desc.scene_width / (float)num_eyes;
        const float h_px = (float)desc.scene_height;
        const float half_diag_sq = (eye_w_px * 0.5f) * (eye_w_px * 0.5f) + (h_px * 0.5f) * (h_px * 0.5f);
        const float full_cutoff_sq = desc.full_rate_cutoff * desc.full_rate_cutoff;
        const float half_cutoff_sq = desc.half_rate_cutoff * desc.half_rate_cutoff;
        const uint8_t outer_rate = (desc.allow_4x4 && m_caps.additional_rates)
            ? (uint8_t)D3D12_SHADING_RATE_4X4 : (uint8_t)D3D12_SHADING_RATE_2X2;

        for (uint32_t ty = 0; ty < sri_h; ++ty) {
            uint8_t* row = mapped + (size_t)ty * row_pitch;
            const float py = ((float)ty + 0.5f) * tile;

            for (uint32_t tx = 0; tx < sri_w; ++tx) {
                const float px = ((float)tx + 0.5f) * tile;
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

    // Publish. Command lists recorded from now on use the new image; ones in
    // flight keep referencing an older ring slot, which stays alive.
    m_active_sri.store(slot.texture.Get(), std::memory_order_release);

    return true;
}

VRSInjector::Stats VRSInjector::get_stats() const {
    std::scoped_lock _{m_update_mtx};
    return m_last_stats;
}

void VRSInjector::on_device_reset() {
    std::scoped_lock _{m_update_mtx};

    deactivate();

    // deactivate() published m_active_sri = nullptr (seq-cst). Wait for any
    // recording thread that already loaded the old pointer to finish using it
    // before releasing the textures, so RSSetShadingRateImage can never run on a
    // freed resource. The seq-cst handshake guarantees a thread still holding a
    // non-null image is observed here as a live ref.
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

    for (auto& slot : m_sri_ring) {
        slot.texture.Reset();
        slot.upload.Reset();
        slot.width = 0;
        slot.height = 0;
        slot.in_shading_rate_state = false;
    }

    for (size_t i = 0; i < SRI_RING_SIZE; ++i) {
        m_upload_ctx[i].reset();
        m_upload_ctx_ready[i] = false;
    }

    m_caps = Caps{};
    m_caps_device = nullptr;
    m_has_last_desc = false;
    m_cl5_support.store(0, std::memory_order_relaxed);
}
} // namespace render
