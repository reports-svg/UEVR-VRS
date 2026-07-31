#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "../../Mod.hpp"

namespace sdk {
struct IConsoleVariable;
}

// Foveated rendering via Variable Rate Shading (VRS).
//
// Two paths:
//  - Engine Native: drives UE's own VRS system through cvars
//    (r.VRS.Enable + xr.VRS.FoveationLevel, with fallbacks for the older
//    vr.VRS.HMDFixedFoveation* names). DX12 + Tier 2 GPU + a UE build that
//    shipped with VRS shaders required. The engine generates and applies its
//    own shading-rate image to the base pass, translucency, Nanite, decals,
//    SSR and light functions.
//  - Injected (D3D12): UEVR generates its own shading-rate image (with true
//    per-eye optical centers and optional eye-tracked gaze) and binds it on
//    the game's command lists whenever the stereo scene target is bound.
//    Works even when the game's engine VRS is unreachable. See VRSInjector.
class FoveatedRendering final : public ModComponent {
public:
    enum Mode : int32_t {
        MODE_DISABLED = 0,
        MODE_ENGINE_NATIVE = 1,
        MODE_INJECTED = 2,
    };

    enum Level : int32_t {
        LEVEL_LOW = 0,
        LEVEL_MEDIUM = 1,
        LEVEL_HIGH = 2,
        LEVEL_HIGH_TOP = 3,
        LEVEL_CUSTOM = 4,
    };

    enum LayoutOverride : int32_t {
        LAYOUT_AUTO = 0,
        LAYOUT_DOUBLE_WIDE = 1,
        LAYOUT_MONO = 2,
    };

    FoveatedRendering();
    virtual ~FoveatedRendering() = default;

    std::string_view get_name() const override { return "FoveatedRendering"; }

    void on_frame() override;                                                   // present thread
    void on_pre_engine_tick(sdk::UGameEngine* engine, float delta) override;    // game thread
    void on_draw_ui() override;
    void on_device_reset() override;

    Mode get_mode() const { return (Mode)m_mode->value(); }

private:
    struct EngineCVars {
        sdk::IConsoleVariable* enable{nullptr};   // r.VRS.Enable
        sdk::IConsoleVariable* level{nullptr};    // xr.VRS.FoveationLevel / vr.VRS.HMDFixedFoveationLevel
        sdk::IConsoleVariable* dynamic{nullptr};  // xr.VRS.DynamicFoveation / vr.VRS.HMDFixedFoveationDynamic
        sdk::IConsoleVariable* gaze{nullptr};     // xr.VRS.GazeTrackedFoveation
        sdk::IConsoleVariable* preview{nullptr};  // r.VRS.Preview
        std::string level_cvar_name{};
        bool resolved_any{false};
        float retry_timer{0.0f};
        int applied_level{-1};
        int applied_dynamic{-1};
        int applied_gaze{-1};
        int applied_preview{-1};
        int applied_enable{-1};
        // Snapshot of the game's own VRS cvars taken before we first overwrite
        // them, so leaving engine mode restores them instead of forcing 0.
        bool captured_originals{false};
        int orig_enable{0};
        int orig_level{0};
        int orig_dynamic{0};
        int orig_gaze{0};
        int orig_preview{0};
        bool needs_disable{false};
        float reapply_timer{0.0f};
    };

    struct DynamicState {
        double sum_ms{0.0};
        int num_samples{0};
        float amount{1.0f}; // 0 = no foveation, 1 = full requested foveation
        int64_t last_qpc{0};
    };

    void resolve_engine_cvars();
    void update_engine_path(float delta);
    void update_injected_path();
    float update_dynamic_amount();
    void get_ring_cutoffs(float& full_cutoff, float& half_cutoff, float& center_v) const;
    // Per-eye optical centers (gaze-independent) in per-eye UV space, derived
    // from the HMD's asymmetric projection when enabled.
    void compute_optical_centers(float (&center_u)[2], float (&center_v)[2]) const;
    // Returns per-eye foveation centers in per-eye UV space.
    void compute_eye_centers(float (&center_u)[2], float (&center_v)[2], float preset_center_v);

    // Debug preview: draws a color-coded map of the shading-rate image the
    // injector is actually generating (green=1x1, yellow=2x2, red=4x4), the
    // injected-path analog of the engine's r.VRS.Preview overlay.
    void draw_debug_preview();
    // Rate class (0=1x1, 1=2x2, 2=4x4) at a full-frame normalized position,
    // using the last pattern published to the injector.
    int preview_rate_at(float u, float v) const;

    const ModCombo::Ptr m_mode{ ModCombo::create(generate_name("Mode"),
        {"Disabled", "Engine Native (CVars)", "Injected (D3D12)"}, MODE_DISABLED) };
    const ModCombo::Ptr m_level{ ModCombo::create(generate_name("Level"),
        {"Low", "Medium", "High", "High (Top-Shifted)", "Custom"}, LEVEL_MEDIUM) };
    // Normalized radii (0..1 of the per-eye half-diagonal). 0 = the ring starts
    // at the very center (maximally aggressive); >=1 = the ring never triggers.
    // Min is 0 so the whole aggressiveness range is reachable from the UI.
    const ModSlider::Ptr m_full_rate_cutoff{ ModSlider::create(generate_name("FullRateCutoff"), 0.0f, 1.0f, 0.5f) };
    const ModSlider::Ptr m_half_rate_cutoff{ ModSlider::create(generate_name("HalfRateCutoff"), 0.0f, 1.5f, 0.75f) };
    const ModToggle::Ptr m_allow_4x4{ ModToggle::create(generate_name("Allow4x4"), true) };
    const ModToggle::Ptr m_dynamic{ ModToggle::create(generate_name("Dynamic"), false) };
    const ModSlider::Ptr m_dynamic_target_ms{ ModSlider::create(generate_name("DynamicTargetMs"), 5.0f, 30.0f, 12.5f) };
    const ModToggle::Ptr m_gaze_tracking{ ModToggle::create(generate_name("GazeTracking"), false) };
    // EMA smoothing adds roughly s/(1-s) frames of gaze latency (~17ms at 0.6 and
    // 90Hz); research puts the total gaze-to-photon budget at 50-70ms, so the
    // default stays low and heavy smoothing is a deliberate user choice.
    const ModSlider::Ptr m_gaze_smoothing{ ModSlider::create(generate_name("GazeSmoothing"), 0.0f, 0.95f, 0.4f, true) };
    const ModToggle::Ptr m_engine_preview{ ModToggle::create(generate_name("EnginePreview"), false) };
    const ModToggle::Ptr m_debug_preview{ ModToggle::create(generate_name("DebugPreview"), false) };
    const ModToggle::Ptr m_require_depth{ ModToggle::create(generate_name("InjectedRequireDepth"), true, true) };
    const ModToggle::Ptr m_use_optical_centers{ ModToggle::create(generate_name("UseOpticalCenters"), true, true) };
    // Upscaler awareness: allow the injector to build shading-rate images for
    // DLSS/FSR/TSR render resolutions and suppress display-res binds meanwhile.
    const ModToggle::Ptr m_upscaler_compat{ ModToggle::create(generate_name("UpscalerCompat"), true, true) };
    // Lens mask: coarsen tiles outside the lens-visible ellipse (anchored at the
    // optical centers) even when the gaze rings wander toward an edge.
    const ModToggle::Ptr m_lens_mask{ ModToggle::create(generate_name("LensMask"), true, true) };
    const ModSlider::Ptr m_lens_scale_x{ ModSlider::create(generate_name("LensMaskScaleX"), 0.70f, 1.50f, 1.05f, true) };
    const ModSlider::Ptr m_lens_scale_y{ ModSlider::create(generate_name("LensMaskScaleY"), 0.70f, 1.50f, 1.05f, true) };
    const ModSlider::Ptr m_center_offset_x{ ModSlider::create(generate_name("CenterOffsetX"), -0.4f, 0.4f, 0.0f, true) };
    const ModSlider::Ptr m_center_offset_y{ ModSlider::create(generate_name("CenterOffsetY"), -0.4f, 0.4f, 0.0f, true) };
    const ModCombo::Ptr m_layout_override{ ModCombo::create(generate_name("LayoutOverride"),
        {"Auto", "Double-Wide", "Mono"}, LAYOUT_AUTO, true) };

    EngineCVars m_engine_cvars{};
    DynamicState m_dynamic_state{};

    // Smoothed gaze center (per-eye UV), present thread only.
    float m_smoothed_gaze_u[2]{0.5f, 0.5f};
    float m_smoothed_gaze_v[2]{0.5f, 0.5f};
    bool m_gaze_was_valid{false};

    // Snapshot of the last pattern published to the injector, so the debug
    // preview shows exactly what is being generated (updated in on_frame).
    struct PreviewState {
        bool valid{false};
        bool double_wide{true};
        bool allow_4x4{true};
        float full_cutoff_sq{0.25f};
        float half_cutoff_sq{0.5625f};
        float eye_aspect{1.0f}; // per-eye width/height, for correct ring shape
        float center_u[2]{0.5f, 0.5f};
        float center_v[2]{0.5f, 0.5f};
        bool lens_mask{false};
        float lens_u[2]{0.5f, 0.5f};
        float lens_v[2]{0.5f, 0.5f};
        float lens_rx{1.05f};
        float lens_ry{1.05f};
    } m_preview{};
};
