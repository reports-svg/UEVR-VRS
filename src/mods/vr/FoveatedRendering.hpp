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
    // Returns per-eye foveation centers in per-eye UV space.
    void compute_eye_centers(float (&center_u)[2], float (&center_v)[2], float preset_center_v);

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
    const ModSlider::Ptr m_gaze_smoothing{ ModSlider::create(generate_name("GazeSmoothing"), 0.0f, 0.95f, 0.6f, true) };
    const ModToggle::Ptr m_engine_preview{ ModToggle::create(generate_name("EnginePreview"), false) };
    const ModToggle::Ptr m_require_depth{ ModToggle::create(generate_name("InjectedRequireDepth"), true, true) };
    const ModToggle::Ptr m_use_optical_centers{ ModToggle::create(generate_name("UseOpticalCenters"), true, true) };
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
};
