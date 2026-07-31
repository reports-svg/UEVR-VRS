#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "../mods/vr/d3d12/CommandContext.hpp"

namespace render {
// Injects D3D12 Tier-2 Variable Rate Shading (image-based foveation) into the
// game's own command lists. This is the "injected" VRS path: UEVR generates a
// small R8_UINT shading-rate image (one byte per hardware tile, values are the
// D3D12_SHADING_RATE palette) and binds it via RSSetShadingRateImage whenever
// the game binds render targets that look like the stereo scene target.
//
// Upscaler awareness: when the game renders its scene passes at a reduced
// internal resolution (DLSS/FSR2/TSR/r.ScreenPercentage), the expensive passes
// no longer match the display-resolution scene target. Recording threads that
// observe an aspect-preserving sub-resolution of the scene target request a
// variant; the present thread then builds a correctly-scaled shading-rate
// image for that resolution. While sub-resolution scene activity is live,
// display-resolution binds are suppressed so post-upscale passes are never
// coarse-shaded.
//
// Threading model:
//  - update() runs on the present thread (from FoveatedRendering::on_frame).
//  - on_om_set_render_targets() runs on any of the game's recording threads.
//  - register_rtv() runs on whatever thread the game creates RTVs on.
class VRSInjector {
public:
    static VRSInjector& get();

    struct Caps {
        bool queried{false};
        bool tier1{false};
        bool tier2{false};
        bool additional_rates{false}; // 2x4/4x2/4x4 supported
        uint32_t tile_size{16};
    };

    struct FoveationDesc {
        bool enabled{false};
        bool require_depth{true};   // only apply when a DSV is bound (geometry passes)
        bool double_wide{true};     // scene target holds both eyes side-by-side
        bool allow_4x4{true};
        bool allow_subres{true};    // build variants for upscaler render resolutions
        uint32_t scene_width{};
        uint32_t scene_height{};
        uintptr_t ui_target{};      // native resource to always exclude
        // Ring cutoffs, normalized against the per-eye half-diagonal (UE convention).
        float full_rate_cutoff{0.5f};
        float half_rate_cutoff{0.75f};
        // Foveation centers in per-eye UV space.
        float center_u[2]{0.5f, 0.5f};
        float center_v[2]{0.5f, 0.5f};
        // Lens mask: tiles outside an ellipse anchored at the (gaze-independent)
        // optical center are forced to the coarsest ring rate, so pixels the lens
        // cannot resolve stay cheap even when the gaze rings wander toward them.
        bool lens_mask{false};
        float lens_u[2]{0.5f, 0.5f}; // per-eye ellipse centers in per-eye UV space
        float lens_v[2]{0.5f, 0.5f};
        float lens_rx{1.05f};        // semi-axes as fractions of the per-eye half-extent
        float lens_ry{1.05f};
    };

    struct Stats {
        uint32_t rtv_binds{};
        uint32_t vrs_binds{};
        uint32_t subres_binds{};
        uint32_t sri_updates{};
        uint32_t sri_width{};
        uint32_t sri_height{};
        uint32_t subres_width{};
        uint32_t subres_height{};
        bool suppressing_fullres{};
        bool active{};
    };

    // Present thread. Applies the new desc, (re)builds the shading-rate images
    // if anything changed, and publishes them for the recording threads.
    void update(const FoveationDesc& desc);

    // Called by the D3D12Hook OMSetRenderTargets detour (after the original).
    // Positive-only: binds the SRI when a registered scene-target RTV is bound,
    // never clears (viewport matching is the primary trigger and owns clearing).
    void on_om_set_render_targets(
        ID3D12GraphicsCommandList* command_list,
        UINT num_render_targets,
        const D3D12_CPU_DESCRIPTOR_HANDLE* render_targets,
        BOOL single_handle_to_range,
        const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil);

    // Called by the D3D12Hook RSSetViewports detour (after the original). This
    // is the primary VRS trigger: it keys off viewport dimensions instead of
    // descriptor handles, so it is immune to descriptor-heap indirection and to
    // scene RTVs that were created before UEVR's D3D12 hook engaged.
    void on_rs_set_viewports(
        ID3D12GraphicsCommandList* command_list,
        UINT num_viewports,
        const D3D12_VIEWPORT* viewports);

    // Called by the D3D12Hook CreateRenderTargetView detour.
    void register_rtv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle);

    void on_device_reset();

    Caps get_caps() const {
        std::scoped_lock _{m_update_mtx};
        return m_caps;
    }

    Stats get_stats() const;

    bool is_active() const {
        return m_any_active.load(std::memory_order_relaxed);
    }

private:
    // Variant 0 serves the display-resolution scene target; the remaining slots
    // serve upscaler render resolutions discovered at runtime.
    static constexpr size_t MAX_VARIANTS = 3;
    static constexpr size_t SRI_RING_SIZE = 2;
    static constexpr size_t MAX_RTV_ENTRIES = 16384;
    // A sub-resolution variant with no binds for this many updates is unpublished.
    static constexpr uint32_t VARIANT_IDLE_UPDATES = 300;
    // Display-resolution suppression lingers this many updates past the last
    // sub-resolution bind, riding out per-frame pass ordering jitter. Kept short
    // so a misdetection self-heals quickly.
    static constexpr uint32_t SUPPRESS_LINGER_UPDATES = 30;

    struct RtvInfo {
        ID3D12Resource* resource{};
        uint32_t width{};
        uint32_t height{};
    };

    struct SriSlot {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture{};
        Microsoft::WRL::ComPtr<ID3D12Resource> upload{};
        uint32_t width{};
        uint32_t height{};
        bool in_shading_rate_state{false};
    };

    struct Variant {
        // Hot-path published state (recording threads only read these).
        // Dimensions are packed ((w << 32) | h) so readers can never see a torn
        // width/height pair; matchers re-load them after loading sri to reject
        // a cross-retarget (dims, sri) mismatch.
        std::atomic<ID3D12Resource*> sri{nullptr};
        std::atomic<uint64_t> pub_dims{0};
        mutable std::atomic<uint32_t> binds{0};

        // Update-thread state.
        uint32_t target_width{0};  // 0 = slot free
        uint32_t target_height{0};
        uint32_t idle_updates{0};
        uint64_t pattern_serial{0}; // serial of the desc this variant's SRI encodes
        std::array<SriSlot, SRI_RING_SIZE> ring{};
        size_t ring_index{0};
        std::array<d3d12::CommandContext, SRI_RING_SIZE> upload_ctx{};
        std::array<bool, SRI_RING_SIZE> upload_ctx_ready{};
    };

    bool query_caps(ID3D12Device* device);
    bool generate_and_upload(ID3D12Device* device, const FoveationDesc& desc, Variant& variant);
    void unpublish_variant(Variant& variant);
    void deactivate();
    void retire(Microsoft::WRL::ComPtr<ID3D12Resource> resource);

    // Resolve the QI-once ID3D12GraphicsCommandList5 support; returns nullptr
    // if unsupported. Also handles the is_inside_present() guard.
    ID3D12GraphicsCommandList5* resolve_cl5(ID3D12GraphicsCommandList* command_list);

    void bind_sri(ID3D12GraphicsCommandList5* cl5, ID3D12Resource* sri);
    void unbind_sri(ID3D12GraphicsCommandList5* cl5);

    // Find a published variant serving a target of these dimensions (within
    // tolerance). Returns the variant index or -1.
    int find_variant_for_target(uint32_t w, uint32_t h) const;

    // True if full-target dims (w, h) look like an aspect-preserving upscaler
    // render resolution of the scene target. Only the RTV path (which sees true
    // texture extents, with depth bound) may request variants from this - the
    // viewport path can't distinguish upscaler scene passes from a game's own
    // fixed reduced-resolution passes reliably enough.
    bool is_subres_scene_candidate(uint32_t w, uint32_t h) const;

    // Publish a request for a sub-resolution variant; the present thread
    // consumes it on the next update(). Last writer wins.
    void request_subres_variant(uint32_t w, uint32_t h);

    // True if this RTV bind matches a scene-class target; fills the SRI to bind.
    ID3D12Resource* match_rtv_bind(
        UINT num_render_targets,
        const D3D12_CPU_DESCRIPTOR_HANDLE* render_targets,
        const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil);

    // Viewport-dimension matching; fills the SRI to bind (nullptr = no match).
    ID3D12Resource* match_viewport_bind(const D3D12_VIEWPORT& vp);

    // Hot-path published state (recording threads only read these).
    std::array<Variant, MAX_VARIANTS> m_variants{};
    std::atomic<bool> m_any_active{false};
    std::atomic<bool> m_suppress_fullres{false};
    std::atomic<bool> m_allow_subres{true};
    std::atomic<uint32_t> m_match_width{0};
    std::atomic<uint32_t> m_match_height{0};
    std::atomic<uintptr_t> m_excluded_resource{0};
    std::atomic<bool> m_require_depth{true};
    std::atomic<bool> m_double_wide{true};
    // Packed (w << 32 | h) sub-resolution variant request, 0 = none.
    std::atomic<uint64_t> m_subres_request{0};

    // In-flight guard: number of recording threads currently between loading
    // a variant's sri and finishing with it. on_device_reset() drains this to
    // zero before releasing the shading-rate textures so a recording thread can
    // never hand a freed resource to RSSetShadingRateImage.
    std::atomic<int32_t> m_recording_refs{0};

    // -1 = unsupported, 0 = unknown, 1 = supported
    std::atomic<int> m_cl5_support{0};

    // Stats (relaxed atomics, reset each update()).
    mutable std::atomic<uint32_t> m_stat_rtv_binds{0};
    mutable std::atomic<uint32_t> m_stat_vrs_binds{0};
    // Display-resolution geometry evidence: successful variant-0 binds that had
    // a depth target bound. If these are present, sub-resolution activity is a
    // game's own reduced-res pass (translucency/reflections), NOT an upscaler -
    // so display-res suppression must not arm.
    mutable std::atomic<uint32_t> m_v0_depth_binds{0};
    Stats m_last_stats{};

    // RTV descriptor -> resource info map.
    mutable std::shared_mutex m_rtv_mtx{};
    std::unordered_map<uintptr_t, RtvInfo> m_rtv_map{};

    // Update-thread state.
    mutable std::recursive_mutex m_update_mtx{};
    Caps m_caps{};
    ID3D12Device* m_caps_device{nullptr};
    FoveationDesc m_last_desc{};
    bool m_has_last_desc{false};
    uint64_t m_pattern_serial{1}; // bumped whenever the desc pattern changes
    uint32_t m_suppress_countdown{0};
    uint32_t m_sri_updates{0};
    uint32_t m_update_log_counter{0};
    // Textures pulled out of a variant ring while command lists recorded against
    // them may still be in flight; hold them for a few updates before release.
    std::vector<std::pair<uint32_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> m_retired{};
};
} // namespace render
