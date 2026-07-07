#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

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
        uint32_t scene_width{};
        uint32_t scene_height{};
        uintptr_t ui_target{};      // native resource to always exclude
        // Ring cutoffs, normalized against the per-eye half-diagonal (UE convention).
        float full_rate_cutoff{0.5f};
        float half_rate_cutoff{0.75f};
        // Foveation centers in per-eye UV space.
        float center_u[2]{0.5f, 0.5f};
        float center_v[2]{0.5f, 0.5f};
    };

    struct Stats {
        uint32_t rtv_binds{};
        uint32_t vrs_binds{};
        uint32_t sri_updates{};
        uint32_t sri_width{};
        uint32_t sri_height{};
        bool active{};
    };

    // Present thread. Applies the new desc, (re)builds the shading-rate image
    // if anything changed, and publishes it for the recording threads.
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
        return m_active_sri.load(std::memory_order_relaxed) != nullptr;
    }

private:
    static constexpr size_t SRI_RING_SIZE = 4;
    static constexpr size_t MAX_RTV_ENTRIES = 16384;

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

    bool query_caps(ID3D12Device* device);
    bool generate_and_upload(ID3D12Device* device, const FoveationDesc& desc, uint32_t sri_w, uint32_t sri_h);
    void deactivate();

    // Resolve the QI-once ID3D12GraphicsCommandList5 support; returns nullptr
    // if unsupported. Also handles the is_inside_present() guard.
    ID3D12GraphicsCommandList5* resolve_cl5(ID3D12GraphicsCommandList* command_list);

    void bind_sri(ID3D12GraphicsCommandList5* cl5, ID3D12Resource* sri);
    void unbind_sri(ID3D12GraphicsCommandList5* cl5);

    // True if this RTV bind matches a registered scene-target resource.
    bool matches_scene_target(
        UINT num_render_targets,
        const D3D12_CPU_DESCRIPTOR_HANDLE* render_targets,
        const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil) const;

    // True if a viewport of these dims corresponds to the scene render (full
    // double-wide, or a single eye's half). Descriptor-independent.
    bool viewport_matches_scene(const D3D12_VIEWPORT& vp) const;

    // Hot-path published state (recording threads only read these).
    std::atomic<ID3D12Resource*> m_active_sri{nullptr};
    std::atomic<uint32_t> m_match_width{0};
    std::atomic<uint32_t> m_match_height{0};
    std::atomic<uintptr_t> m_excluded_resource{0};
    std::atomic<bool> m_require_depth{true};
    std::atomic<bool> m_double_wide{true};

    // In-flight guard: number of recording threads currently between loading
    // m_active_sri and finishing with it. on_device_reset() drains this to zero
    // before releasing the shading-rate textures so a recording thread can never
    // hand a freed resource to RSSetShadingRateImage.
    std::atomic<int32_t> m_recording_refs{0};

    // -1 = unsupported, 0 = unknown, 1 = supported
    std::atomic<int> m_cl5_support{0};

    // Stats (relaxed atomics, reset each update()).
    mutable std::atomic<uint32_t> m_stat_rtv_binds{0};
    mutable std::atomic<uint32_t> m_stat_vrs_binds{0};
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
    std::array<SriSlot, SRI_RING_SIZE> m_sri_ring{};
    size_t m_sri_index{0};
    uint32_t m_sri_updates{0};
    uint32_t m_update_log_counter{0};
    // One upload context per ring slot. Sharing a single context would force the
    // present thread to wait on the immediately-previous upload every time the
    // image is regenerated (e.g. every frame under eye-tracked gaze); a per-slot
    // context has virtually always gone idle by the time the ring cycles back.
    std::array<d3d12::CommandContext, SRI_RING_SIZE> m_upload_ctx{};
    std::array<bool, SRI_RING_SIZE> m_upload_ctx_ready{};
};
} // namespace render
