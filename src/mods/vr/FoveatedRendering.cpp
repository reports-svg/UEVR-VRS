#include <algorithm>
#include <cmath>

#include <wrl/client.h>

#include <spdlog/spdlog.h>
#include <utility/ScopeGuard.hpp>

#include <sdk/CVar.hpp>
#include <sdk/ConsoleManager.hpp>
#include <sdk/StereoStuff.hpp>

#include "Framework.hpp"

#include "../VR.hpp"

#include "../../render/VRSInjector.hpp"

#include "FoveatedRendering.hpp"

// UE's foveation level tables (FoveatedImageGenerator.cpp), indexed 0..4 where
// 0 is "off". Cutoffs are normalized against the squared per-eye half-diagonal.
namespace {
constexpr float FULL_RATE_CUTOFFS[5]{1.0f, 0.7f, 0.5f, 0.35f, 0.35f};
constexpr float HALF_RATE_CUTOFFS[5]{1.0f, 0.9f, 0.75f, 0.55f, 0.55f};
constexpr float PRESET_CENTER_V[5]{0.5f, 0.5f, 0.5f, 0.5f, 0.42f};

int level_combo_to_engine_level(int32_t combo_value) {
    switch (combo_value) {
    case FoveatedRendering::LEVEL_LOW:
        return 1;
    case FoveatedRendering::LEVEL_MEDIUM:
        return 2;
    case FoveatedRendering::LEVEL_HIGH:
        return 3;
    case FoveatedRendering::LEVEL_HIGH_TOP:
        return 4;
    case FoveatedRendering::LEVEL_CUSTOM:
    default:
        return 3; // engine path has no custom cutoffs, use High
    }
}

bool try_set_cvar(sdk::IConsoleVariable* cvar, int value) {
    if (cvar == nullptr) {
        return false;
    }

    try {
        cvar->Set(std::to_wstring(value).c_str());
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<int> try_get_cvar(sdk::IConsoleVariable* cvar) {
    if (cvar == nullptr) {
        return std::nullopt;
    }

    try {
        return cvar->GetInt();
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace

FoveatedRendering::FoveatedRendering() {
    m_options = {
        *m_mode,
        *m_level,
        *m_full_rate_cutoff,
        *m_half_rate_cutoff,
        *m_allow_4x4,
        *m_gradient,
        *m_dynamic,
        *m_dynamic_target_ms,
        *m_gaze_tracking,
        *m_gaze_smoothing,
        *m_engine_preview,
        *m_debug_preview,
        *m_require_depth,
        *m_use_optical_centers,
        *m_upscaler_compat,
        *m_lens_mask,
        *m_lens_scale_x,
        *m_lens_scale_y,
        *m_center_offset_x,
        *m_center_offset_y,
        *m_layout_override,
    };
}

void FoveatedRendering::resolve_engine_cvars() {
    const auto console_manager = sdk::FConsoleManager::get();

    if (console_manager == nullptr) {
        return;
    }

    auto find_var = [&](const wchar_t* name) -> sdk::IConsoleVariable* {
        try {
            return (sdk::IConsoleVariable*)console_manager->find(name);
        } catch (...) {
            return nullptr;
        }
    };

    if (m_engine_cvars.enable == nullptr) {
        m_engine_cvars.enable = find_var(L"r.VRS.Enable");
    }

    if (m_engine_cvars.level == nullptr) {
        // UE 5.3+ name first, then the older HMD fixed foveation name.
        if (auto* cvar = find_var(L"xr.VRS.FoveationLevel"); cvar != nullptr) {
            m_engine_cvars.level = cvar;
            m_engine_cvars.level_cvar_name = "xr.VRS.FoveationLevel";
        } else if (auto* legacy = find_var(L"vr.VRS.HMDFixedFoveationLevel"); legacy != nullptr) {
            m_engine_cvars.level = legacy;
            m_engine_cvars.level_cvar_name = "vr.VRS.HMDFixedFoveationLevel";
        }
    }

    if (m_engine_cvars.dynamic == nullptr) {
        if (auto* cvar = find_var(L"xr.VRS.DynamicFoveation"); cvar != nullptr) {
            m_engine_cvars.dynamic = cvar;
        } else {
            m_engine_cvars.dynamic = find_var(L"vr.VRS.HMDFixedFoveationDynamic");
        }
    }

    if (m_engine_cvars.gaze == nullptr) {
        m_engine_cvars.gaze = find_var(L"xr.VRS.GazeTrackedFoveation");
    }

    if (m_engine_cvars.preview == nullptr) {
        m_engine_cvars.preview = find_var(L"r.VRS.Preview");
    }

    m_engine_cvars.resolved_any = m_engine_cvars.enable != nullptr && m_engine_cvars.level != nullptr;

    if (m_engine_cvars.resolved_any) {
        spdlog::info("[VRS] Engine cvars resolved (level cvar: {})", m_engine_cvars.level_cvar_name);
    }
}

void FoveatedRendering::update_engine_path(float delta) {
    auto& ec = m_engine_cvars;

    if (get_mode() != MODE_ENGINE_NATIVE) {
        // Turn the engine feature back off once when leaving engine mode,
        // restoring the values the game had before we took over rather than
        // forcing everything to 0 (which would disable a game's own VRS).
        if (ec.needs_disable && ec.resolved_any) {
            if (ec.captured_originals) {
                try_set_cvar(ec.enable, ec.orig_enable);
                try_set_cvar(ec.level, ec.orig_level);
                try_set_cvar(ec.dynamic, ec.orig_dynamic);
                try_set_cvar(ec.gaze, ec.orig_gaze);
                try_set_cvar(ec.preview, ec.orig_preview);
            } else {
                try_set_cvar(ec.enable, 0);
                try_set_cvar(ec.level, 0);
                try_set_cvar(ec.dynamic, 0);
                try_set_cvar(ec.gaze, 0);
                try_set_cvar(ec.preview, 0);
            }

            ec.applied_enable = -1;
            ec.applied_level = -1;
            ec.applied_dynamic = -1;
            ec.applied_gaze = -1;
            ec.applied_preview = -1;
            ec.captured_originals = false;
            ec.needs_disable = false;
            spdlog::info("[VRS] Engine VRS disabled (restored game defaults)");
        }

        return;
    }

    if (!ec.resolved_any) {
        ec.retry_timer -= delta;

        if (ec.retry_timer <= 0.0f) {
            ec.retry_timer = 3.0f;
            resolve_engine_cvars();
        }

        if (!ec.resolved_any) {
            return;
        }
    }

    const int wanted_enable = 1;
    const int wanted_level = level_combo_to_engine_level(m_level->value());
    const int wanted_dynamic = m_dynamic->value() ? 1 : 0;
    const int wanted_gaze = m_gaze_tracking->value() ? 1 : 0;
    const int wanted_preview = m_engine_preview->value() ? 1 : 0;

    ec.reapply_timer -= delta;
    const bool periodic = ec.reapply_timer <= 0.0f;

    if (periodic) {
        // Games and their config systems occasionally stomp these, re-assert.
        ec.reapply_timer = 2.0f;
    }

    if (periodic || ec.applied_enable != wanted_enable) {
        if (!ec.captured_originals) {
            // Snapshot the game's own VRS cvars before we overwrite any of them,
            // so leaving engine mode can put them back exactly.
            ec.orig_enable = try_get_cvar(ec.enable).value_or(0);
            ec.orig_level = try_get_cvar(ec.level).value_or(0);
            ec.orig_dynamic = try_get_cvar(ec.dynamic).value_or(0);
            ec.orig_gaze = try_get_cvar(ec.gaze).value_or(0);
            ec.orig_preview = try_get_cvar(ec.preview).value_or(0);
            ec.captured_originals = true;
        }

        if (try_set_cvar(ec.enable, wanted_enable)) {
            ec.applied_enable = wanted_enable;
            ec.needs_disable = true;
        }
    }

    if (periodic || ec.applied_level != wanted_level) {
        if (try_set_cvar(ec.level, wanted_level)) {
            ec.applied_level = wanted_level;
        }
    }

    // Games and their scalability systems stomp these too, so re-assert them on
    // the same periodic cadence as enable/level, not only when the user changes
    // them.
    if (periodic || ec.applied_dynamic != wanted_dynamic) {
        if (try_set_cvar(ec.dynamic, wanted_dynamic)) {
            ec.applied_dynamic = wanted_dynamic;
        }
    }

    if (periodic || ec.applied_gaze != wanted_gaze) {
        if (try_set_cvar(ec.gaze, wanted_gaze)) {
            ec.applied_gaze = wanted_gaze;
        }
    }

    if (periodic || ec.applied_preview != wanted_preview) {
        if (try_set_cvar(ec.preview, wanted_preview)) {
            ec.applied_preview = wanted_preview;
        }
    }
}

void FoveatedRendering::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    (void)engine;

    update_engine_path(delta);
}

float FoveatedRendering::update_dynamic_amount() {
    if (!m_dynamic->value()) {
        m_dynamic_state = DynamicState{};
        m_dynamic_state.amount = 1.0f;
        return 1.0f;
    }

    auto& ds = m_dynamic_state;

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);

    if (ds.last_qpc == 0) {
        ds.last_qpc = qpc.QuadPart;
        return ds.amount;
    }

    static const double qpf_ms = []() {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return 1000.0 / (double)freq.QuadPart;
    }();

    const double frame_ms = (double)(qpc.QuadPart - ds.last_qpc) * qpf_ms;
    ds.last_qpc = qpc.QuadPart;

    if (frame_ms <= 0.0 || frame_ms > 250.0) {
        return ds.amount; // hitch/alt-tab, ignore
    }

    // Same controller as UE's UpdateDynamicVRSAmount: 10-frame rolling average
    // stepped by 0.1 with a 0.5ms deadband around the target.
    constexpr int NUM_FRAMES_TO_AVERAGE = 10;
    constexpr double MARGIN_MS = 0.5;
    constexpr float INCREMENT = 0.1f;

    ds.sum_ms += frame_ms;
    ++ds.num_samples;

    if (ds.num_samples >= NUM_FRAMES_TO_AVERAGE) {
        const double average_ms = ds.sum_ms / (double)ds.num_samples;
        const double target_ms = (double)m_dynamic_target_ms->value();

        if (average_ms > target_ms + MARGIN_MS && ds.amount < 1.0f) {
            ds.amount = std::clamp(ds.amount + INCREMENT, 0.0f, 1.0f);
        } else if (average_ms < target_ms - MARGIN_MS && ds.amount > 0.0f) {
            ds.amount = std::clamp(ds.amount - INCREMENT, 0.0f, 1.0f);
        }

        ds.sum_ms = 0.0;
        ds.num_samples = 0;
    }

    return ds.amount;
}

void FoveatedRendering::get_ring_cutoffs(float& full_cutoff, float& half_cutoff, float& center_v) const {
    const auto level = m_level->value();

    if (level == LEVEL_CUSTOM) {
        full_cutoff = m_full_rate_cutoff->value();
        half_cutoff = std::max(m_half_rate_cutoff->value(), full_cutoff);
        center_v = 0.5f;
        return;
    }

    const int engine_level = level_combo_to_engine_level(level);
    full_cutoff = FULL_RATE_CUTOFFS[engine_level];
    half_cutoff = HALF_RATE_CUTOFFS[engine_level];
    center_v = PRESET_CENTER_V[engine_level];
}

void FoveatedRendering::compute_optical_centers(float (&center_u)[2], float (&center_v)[2]) const {
    // Fixed defaults: middle of each eye.
    center_u[0] = center_u[1] = 0.5f;
    center_v[0] = center_v[1] = 0.5f;

    auto& vr = VR::get();
    const auto runtime = vr->get_runtime();

    if (m_use_optical_centers->value() && runtime != nullptr && runtime->loaded) {
        for (int eye = 0; eye < 2; ++eye) {
            // raw_projections holds the per-eye frustum tangents as
            // [left, right, top, bottom]; the sign conventions differ between
            // runtimes, so use magnitudes. The forward axis crosses the
            // viewport |left|/(|left|+|right|) of the way from the left edge.
            const auto& raw = runtime->raw_projections[eye];
            const float l = std::fabs(raw[0]);
            const float r = std::fabs(raw[1]);
            const float t = std::fabs(raw[2]);
            const float b = std::fabs(raw[3]);

            if (l + r > 0.01f && t + b > 0.01f) {
                center_u[eye] = l / (l + r);
                center_v[eye] = t / (t + b);
            }
        }
    }
}

void FoveatedRendering::compute_eye_centers(float (&center_u)[2], float (&center_v)[2], float preset_center_v) {
    auto& vr = VR::get();

    // Optical centers (or eye-rect middles), plus the preset's vertical shift
    // and the user's manual offsets.
    float base_u[2]{0.5f, 0.5f};
    float base_v[2]{0.5f, 0.5f};
    compute_optical_centers(base_u, base_v);

    const auto runtime = vr->get_runtime();

    bool gaze_applied = false;

    if (m_gaze_tracking->value() && runtime != nullptr && runtime->is_openxr()) {
        const auto openxr = vr->get_openxr_runtime();

        if (openxr != nullptr) {
            if (const auto gaze = openxr->get_eye_gaze_direction(); gaze.has_value()) {
                // Head-space gaze direction, OpenXR convention: X right, Y up,
                // -Z forward. Project onto each eye's tangent-space frustum.
                const auto& dir = *gaze;

                if (dir.z < -0.05f) {
                    const float tan_x = dir.x / -dir.z;
                    const float tan_y = dir.y / -dir.z;
                    const float smoothing = std::clamp(m_gaze_smoothing->value(), 0.0f, 0.95f);

                    for (int eye = 0; eye < 2; ++eye) {
                        const auto& raw = runtime->raw_projections[eye];
                        const float l = -std::fabs(raw[0]);
                        const float r = std::fabs(raw[1]);
                        const float t = std::fabs(raw[2]);
                        const float b = -std::fabs(raw[3]);

                        if (r - l > 0.01f && t - b > 0.01f) {
                            const float u = (tan_x - l) / (r - l);
                            const float v = (t - tan_y) / (t - b);

                            if (!m_gaze_was_valid) {
                                m_smoothed_gaze_u[eye] = u;
                                m_smoothed_gaze_v[eye] = v;
                            } else {
                                m_smoothed_gaze_u[eye] = smoothing * m_smoothed_gaze_u[eye] + (1.0f - smoothing) * u;
                                m_smoothed_gaze_v[eye] = smoothing * m_smoothed_gaze_v[eye] + (1.0f - smoothing) * v;
                            }

                            base_u[eye] = m_smoothed_gaze_u[eye];
                            base_v[eye] = m_smoothed_gaze_v[eye];
                        }
                    }

                    gaze_applied = true;
                }
            }
        }
    }

    m_gaze_was_valid = gaze_applied;

    for (int eye = 0; eye < 2; ++eye) {
        float v = base_v[eye] + m_center_offset_y->value();

        if (!gaze_applied) {
            v += preset_center_v - 0.5f;
        }

        center_u[eye] = std::clamp(base_u[eye] + m_center_offset_x->value(), 0.05f, 0.95f);
        center_v[eye] = std::clamp(v, 0.05f, 0.95f);
    }
}

void FoveatedRendering::update_injected_path() {
    render::VRSInjector::FoveationDesc desc{};
    desc.enabled = false;

    m_preview.valid = false;

    utility::ScopeGuard push_desc{[&]() {
        render::VRSInjector::get().update(desc);
    }};

    if (get_mode() != MODE_INJECTED || !g_framework->is_dx12()) {
        return;
    }

    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return;
    }

    const auto& fake_stereo_hook = vr->get_fake_stereo_hook();

    if (fake_stereo_hook == nullptr) {
        return;
    }

    const auto rtm = fake_stereo_hook->get_render_target_manager();

    if (rtm == nullptr) {
        return;
    }

    const auto scene_texture = rtm->get_render_target();

    if (scene_texture == nullptr) {
        return;
    }

    const auto native_raw = (ID3D12Resource*)scene_texture->get_native_resource();

    if (native_raw == nullptr) {
        return;
    }

    // Hold a reference across GetDesc(): the RHI thread can recycle the pooled
    // scene target underneath us (resolution change, level load), and GetDesc()
    // is a virtual call that would then run on a freed object.
    Microsoft::WRL::ComPtr<ID3D12Resource> native{native_raw};
    const auto scene_desc = native->GetDesc();

    if (scene_desc.Width == 0 || scene_desc.Height == 0) {
        return;
    }

    desc.scene_width = (uint32_t)scene_desc.Width;
    desc.scene_height = (uint32_t)scene_desc.Height;

    if (const auto ui_texture = rtm->get_ui_target(); ui_texture != nullptr) {
        desc.ui_target = (uintptr_t)ui_texture->get_native_resource();
    }

    switch (m_layout_override->value()) {
    case LAYOUT_DOUBLE_WIDE:
        desc.double_wide = true;
        break;
    case LAYOUT_MONO:
        desc.double_wide = false;
        break;
    case LAYOUT_AUTO:
    default:
        // Native stereo renders both eyes side-by-side into one wide target;
        // AFR-style modes render one eye per frame at full size.
        desc.double_wide = !vr->is_using_afr();
        break;
    }

    float full_cutoff{}, half_cutoff{}, preset_center_v{};
    get_ring_cutoffs(full_cutoff, half_cutoff, preset_center_v);

    // Dynamic foveation scales the rings between "none" (cutoffs at 1.0) and
    // the requested level, like UE's dynamic mode.
    const float amount = update_dynamic_amount();
    full_cutoff = std::lerp(1.0f, full_cutoff, amount);
    half_cutoff = std::lerp(1.0f, half_cutoff, amount);

    desc.full_rate_cutoff = full_cutoff;
    desc.half_rate_cutoff = half_cutoff;
    desc.allow_4x4 = m_allow_4x4->value();
    desc.gradient = m_gradient->value();
    desc.require_depth = m_require_depth->value();
    desc.allow_subres = m_upscaler_compat->value();

    float center_u[2]{}, center_v[2]{};
    compute_eye_centers(center_u, center_v, preset_center_v);

    // Lens mask anchors at the pre-gaze optical centers, so it stays put while
    // the foveation rings follow the eyes.
    float lens_u[2]{}, lens_v[2]{};
    compute_optical_centers(lens_u, lens_v);
    desc.lens_mask = m_lens_mask->value();
    desc.lens_rx = m_lens_scale_x->value();
    desc.lens_ry = m_lens_scale_y->value();

    for (int eye = 0; eye < 2; ++eye) {
        desc.center_u[eye] = center_u[eye];
        desc.center_v[eye] = center_v[eye];
        desc.lens_u[eye] = lens_u[eye];
        desc.lens_v[eye] = lens_v[eye];
    }

    // In mono / AFR the scene target holds a single eye per frame and the
    // generator reads its center from index 0. Point index 0 at whichever eye
    // this frame actually renders, so the right eye isn't foveated on the left
    // eye's optical/gaze center.
    if (!desc.double_wide) {
        const int cur_eye = vr->is_current_frame_left_eye() ? 0 : 1;
        desc.center_u[0] = center_u[cur_eye];
        desc.center_v[0] = center_v[cur_eye];
        desc.lens_u[0] = lens_u[cur_eye];
        desc.lens_v[0] = lens_v[cur_eye];
    }

    desc.enabled = true;

    // Snapshot the final pattern for the debug preview (same thread as the UI).
    m_preview.valid = true;
    m_preview.double_wide = desc.double_wide;
    m_preview.allow_4x4 = desc.allow_4x4;
    m_preview.gradient = desc.gradient;
    m_preview.full_cutoff_sq = full_cutoff * full_cutoff;
    m_preview.half_cutoff_sq = half_cutoff * half_cutoff;
    {
        const uint32_t num_eyes = desc.double_wide ? 2u : 1u;
        const float eye_w = (float)desc.scene_width / (float)num_eyes;
        m_preview.eye_aspect = desc.scene_height > 0 ? eye_w / (float)desc.scene_height : 1.0f;
    }
    m_preview.center_u[0] = desc.center_u[0];
    m_preview.center_v[0] = desc.center_v[0];
    m_preview.center_u[1] = desc.center_u[1];
    m_preview.center_v[1] = desc.center_v[1];
    m_preview.lens_mask = desc.lens_mask;
    m_preview.lens_u[0] = desc.lens_u[0];
    m_preview.lens_v[0] = desc.lens_v[0];
    m_preview.lens_u[1] = desc.lens_u[1];
    m_preview.lens_v[1] = desc.lens_v[1];
    m_preview.lens_rx = desc.lens_rx;
    m_preview.lens_ry = desc.lens_ry;
}

void FoveatedRendering::on_frame() {
    update_injected_path();
}

int FoveatedRendering::preview_rate_at(float u, float v) const {
    const auto& p = m_preview;

    // Which eye and where within that eye (0..1).
    int eye = 0;
    float eu = u;

    if (p.double_wide) {
        eye = (u >= 0.5f) ? 1 : 0;
        eu = (u - (float)eye * 0.5f) * 2.0f;
    }

    const float ev = v;
    const float du = eu - p.center_u[eye];
    const float dv = ev - p.center_v[eye];

    // Squared distance normalized by the per-eye half-diagonal, matching
    // VRSInjector's generation (factored to need only the eye aspect ratio).
    const float r = p.eye_aspect;
    const float r2 = r * r;
    const float d2 = 4.0f * (du * du * r2 + dv * dv) / (r2 + 1.0f);

    // Classes: 0 = 1x1, 1 = 2x1 band, 2 = 2x2, 3 = 4x2/4x4.
    const int outer = p.allow_4x4 ? 3 : 2;

    if (d2 > p.half_cutoff_sq) {
        return outer;
    }

    // Lens-mask ellipse in the same per-eye UV space as the generator
    // (ldx = du_px / (rx * eye_w/2) reduces to 2*du/rx in UV).
    if (p.lens_mask && p.lens_rx > 0.01f && p.lens_ry > 0.01f) {
        const float ldx = 2.0f * (eu - p.lens_u[eye]) / p.lens_rx;
        const float ldy = 2.0f * (ev - p.lens_v[eye]) / p.lens_ry;

        if (ldx * ldx + ldy * ldy > 1.0f) {
            return outer;
        }
    }

    if (d2 > p.full_cutoff_sq) {
        if (p.gradient) {
            const float mid_sq = p.full_cutoff_sq + 0.4f * (p.half_cutoff_sq - p.full_cutoff_sq);
            return d2 <= mid_sq ? 1 : 2;
        }

        return 2;
    }
    return 0;
}

void FoveatedRendering::draw_debug_preview() {
    if (!m_preview.valid) {
        ImGui::TextWrapped("Debug preview: injected VRS is not active yet (no scene target).");
        return;
    }

    // UE-style rate colors: green = full 1x1, lime = 2x1, yellow = 2x2, red = coarsest.
    const ImU32 col_1x1 = IM_COL32(40, 200, 60, 255);
    const ImU32 col_2x1 = IM_COL32(150, 210, 45, 255);
    const ImU32 col_2x2 = IM_COL32(230, 205, 40, 255);
    const ImU32 col_4x4 = IM_COL32(225, 55, 45, 255);
    const ImU32 rate_cols[4]{col_1x1, col_2x1, col_2x2, col_4x4};

    const int num_eyes = m_preview.double_wide ? 2 : 1;
    const float full_aspect = m_preview.eye_aspect * (float)num_eyes; // width/height

    float width = ImGui::GetContentRegionAvail().x;
    width = std::clamp(width, 120.0f, 460.0f);
    float height = full_aspect > 0.01f ? width / full_aspect : width * 0.5f;
    height = std::clamp(height, 60.0f, 320.0f);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();

    // Sample a grid of cells and fill by rate class.
    const int cols = 112;
    const int rows = std::max(1, (int)(cols / std::max(0.1f, full_aspect)));
    const float cw = width / (float)cols;
    const float ch = height / (float)rows;

    for (int y = 0; y < rows; ++y) {
        const float v = ((float)y + 0.5f) / (float)rows;
        for (int x = 0; x < cols; ++x) {
            const float u = ((float)x + 0.5f) / (float)cols;
            const int rate = preview_rate_at(u, v);
            const ImVec2 p0{origin.x + x * cw, origin.y + y * ch};
            const ImVec2 p1{p0.x + cw + 1.0f, p0.y + ch + 1.0f};
            dl->AddRectFilled(p0, p1, rate_cols[rate]);
        }
    }

    // Eye-split line and per-eye center crosshairs.
    if (m_preview.double_wide) {
        dl->AddLine(ImVec2(origin.x + width * 0.5f, origin.y),
                    ImVec2(origin.x + width * 0.5f, origin.y + height), IM_COL32(0, 0, 0, 160), 1.0f);
    }

    for (int eye = 0; eye < num_eyes; ++eye) {
        const float eye_off = m_preview.double_wide ? (float)eye * 0.5f : 0.0f;
        const float eye_w = m_preview.double_wide ? 0.5f : 1.0f;
        const float cx = origin.x + (eye_off + m_preview.center_u[eye] * eye_w) * width;
        const float cy = origin.y + m_preview.center_v[eye] * height;
        dl->AddCircle(ImVec2(cx, cy), 5.0f, IM_COL32(255, 255, 255, 230), 12, 1.5f);
        dl->AddLine(ImVec2(cx - 7, cy), ImVec2(cx + 7, cy), IM_COL32(255, 255, 255, 230), 1.0f);
        dl->AddLine(ImVec2(cx, cy - 7), ImVec2(cx, cy + 7), IM_COL32(255, 255, 255, 230), 1.0f);
    }

    dl->AddRect(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(255, 255, 255, 90));
    ImGui::Dummy(ImVec2(width, height));

    // Legend.
    auto swatch = [&](ImU32 c, const char* label) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + 12, pos.y + 12), c);
        ImGui::Dummy(ImVec2(14, 12));
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
    };
    swatch(col_1x1, "1x1 (full)");
    if (m_preview.gradient) {
        ImGui::SameLine();
        swatch(col_2x1, "2x1");
    }
    ImGui::SameLine();
    swatch(col_2x2, "2x2");
    if (m_preview.allow_4x4) {
        ImGui::SameLine();
        swatch(col_4x4, m_preview.gradient ? "4x2/4x4" : "4x4");
    }
}

void FoveatedRendering::on_device_reset() {
    render::VRSInjector::get().on_device_reset();
}

namespace {
// Small "(?)" hover-help marker, matching the terse style of the rest of UEVR.
void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
} // namespace

void FoveatedRendering::on_draw_ui() {
    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);

    if (!ImGui::TreeNode("Foveated Rendering (VRS)")) {
        return;
    }

    ImGui::TextWrapped(
        "Reduces shading rate in the periphery to reclaim GPU time. "
        "Requires DirectX 12 and a VRS Tier 2 GPU (NVIDIA Turing+/AMD RDNA2+/Intel Arc).");

    m_mode->draw("Mode");
    help_marker(
        "Disabled: off.\n"
        "Engine Native: drives the game's own UE VRS via cvars (r.VRS.Enable + xr.VRS.FoveationLevel). "
        "Lowest overhead, but only works on DX12 UE builds that shipped with VRS.\n"
        "Injected (D3D12): UEVR generates and binds its own shading-rate image. Works even when the "
        "game's engine VRS is unreachable, and supports UEVR's own eye tracking and per-eye centers.");

    const auto mode = get_mode();

    if (mode != MODE_DISABLED) {
        m_level->draw("Foveation Level");
        help_marker(
            "Preset aggressiveness. Low->High shrink the full-rate fovea and widen the reduced-rate "
            "periphery. 'High (Top-Shifted)' biases the fovea upward. 'Custom' exposes the raw radii below.");

        if (m_level->value() == LEVEL_CUSTOM) {
            if (mode == MODE_ENGINE_NATIVE) {
                ImGui::TextWrapped("Engine Native mode has no custom cutoffs; 'High' is used instead.");
            } else {
                m_full_rate_cutoff->draw("Full-Rate Radius");
                help_marker(
                    "Radius (0..1 of the per-eye half-diagonal) of the full-quality fovea. "
                    "Inside this circle every pixel is shaded (1x1). Smaller = more aggressive. 0 = no full-rate region.");
                m_half_rate_cutoff->draw("Half-Rate Radius");
                help_marker(
                    "Radius where shading drops to the coarsest rate. Between the two radii it is 2x2; "
                    "beyond it, 4x4 (if allowed). Set both to 0 to force the entire frame to the coarsest rate.");
            }
        }

        // Aggressiveness knob that applies to presets and custom alike, so keep
        // it out of the advanced-only block.
        if (mode == MODE_INJECTED) {
            m_allow_4x4->draw("Allow 4x4 Outer Ring");
            help_marker(
                "Lets the far periphery use the coarsest 4x4 rate (one shaded pixel per 16), for the biggest "
                "GPU saving at the cost of visible peripheral blockiness. Off caps the periphery at 2x2. "
                "Requires a GPU that supports the additional shading rates.");
            m_gradient->draw("Gradient Rings (Soft Transitions)");
            help_marker(
                "Inserts intermediate rates between the rings (1x1 -> 2x1 -> 2x2 -> 4x2 -> 4x4) so the "
                "quality falloff is a soft ramp instead of a visible hard edge. Costs a small part of the "
                "saving; recommended on.");
        }

        m_dynamic->draw("Dynamic (performance-based)");
        help_marker(
            "Automatically scales foveation strength between none and the selected level to hold a target "
            "GPU frame time, using a rolling average (mirrors UE's dynamic foveation).");

        if (m_dynamic->value() && mode == MODE_INJECTED) {
            m_dynamic_target_ms->draw("Dynamic Target Frame Time (ms)");
            ImGui::Text("Current dynamic amount: %.1f", m_dynamic_state.amount);
        }

        m_gaze_tracking->draw("Eye-Tracked (OpenXR)");

        if (m_gaze_tracking->value()) {
            auto& vr = VR::get();
            const auto openxr = (vr != nullptr && vr->get_runtime() != nullptr && vr->get_runtime()->is_openxr())
                ? vr->get_openxr_runtime() : nullptr;

            if (mode == MODE_ENGINE_NATIVE) {
                ImGui::TextWrapped(
                    "Note: the engine path only uses gaze if the game shipped with an eye tracker plugin, "
                    "which is rare. Use the Injected mode for UEVR's own eye tracking.");
            } else if (openxr == nullptr) {
                ImGui::TextWrapped("Eye tracking requires the OpenXR runtime.");
            } else if (!openxr->has_eye_gaze()) {
                ImGui::TextWrapped("Runtime does not expose XR_EXT_eye_gaze_interaction (or it failed to initialize).");
            } else {
                const auto gaze = openxr->get_eye_gaze_direction();
                ImGui::Text("Gaze: %s", gaze.has_value() ? "tracking" : "no data");
                m_gaze_smoothing->draw("Gaze Smoothing");
            }
        }
    }

    if (mode == MODE_ENGINE_NATIVE) {
        m_engine_preview->draw("Debug Overlay (r.VRS.Preview)");

        ImGui::Separator();
        const auto& ec = m_engine_cvars;

        if (!ec.resolved_any) {
            ImGui::TextWrapped(
                "Engine cvars not found yet (r.VRS.Enable / xr.VRS.FoveationLevel). "
                "If this persists, this game likely predates engine VRS or shipped without it - use Injected mode.");
        } else {
            ImGui::Text("Level cvar: %s", ec.level_cvar_name.c_str());

            const auto enable_val = try_get_cvar(ec.enable);
            const auto level_val = try_get_cvar(ec.level);

            ImGui::Text("r.VRS.Enable = %s", enable_val.has_value() ? std::to_string(*enable_val).c_str() : "?");
            ImGui::Text("%s = %s", ec.level_cvar_name.c_str(), level_val.has_value() ? std::to_string(*level_val).c_str() : "?");

            if (ec.gaze == nullptr && m_gaze_tracking->value()) {
                ImGui::TextWrapped("xr.VRS.GazeTrackedFoveation not present in this engine version.");
            }
        }
    }

    if (mode == MODE_INJECTED) {
        if (g_framework->is_advanced_view_enabled()) {
            ImGui::SeparatorText("Advanced (Injected)");
            m_require_depth->draw("Only Passes With Depth Bound");
            help_marker(
                "Restricts VRS to render-target binds that also have a depth buffer bound (geometry passes). "
                "If VRS binds show 0 applied per frame, turn this off to match more passes.");
            m_use_optical_centers->draw("Use Optical Centers From HMD Projection");
            help_marker(
                "Places each eye's fovea at the true optical center derived from the HMD's asymmetric "
                "projection, instead of the middle of the eye rect.");
            m_upscaler_compat->draw("Upscaler Compatibility (DLSS/FSR/TSR)");
            help_marker(
                "When the game renders internally at a reduced resolution (DLSS/FSR2/TSR/ScreenPercentage), "
                "build correctly-scaled shading-rate images for that resolution and stop touching "
                "display-resolution passes (which are post-upscale). If the status below reports "
                "'display-res passes excluded' while NO upscaler is running, the game has its own "
                "reduced-resolution pass fooling the detector - turn this off.");
            m_lens_mask->draw("Lens Mask (Coarsen Invisible Corners)");
            help_marker(
                "Forces tiles outside an ellipse anchored at each eye's optical center to the coarsest rate. "
                "Headset lenses cannot resolve the corners of the rendered rectangle, so this is nearly free "
                "performance - it mostly matters with eye tracking, when the gaze rings wander toward an edge. "
                "Tune the ellipse with the sliders below while watching the Debug Preview.");

            if (m_lens_mask->value()) {
                m_lens_scale_x->draw("Lens Mask Width");
                m_lens_scale_y->draw("Lens Mask Height");
            }

            m_center_offset_x->draw("Center Offset X");
            m_center_offset_y->draw("Center Offset Y");
            m_layout_override->draw("Stereo Layout");
            help_marker(
                "Auto follows the current rendering method (double-wide for native stereo, mono for AFR). "
                "Override only if the scene target layout is detected incorrectly.");
        }

        ImGui::Separator();

        const auto caps = render::VRSInjector::get().get_caps();
        const auto stats = render::VRSInjector::get().get_stats();

        if (!g_framework->is_dx12()) {
            ImGui::TextWrapped("Injected VRS requires DirectX 12. This game is running DirectX 11.");
        } else if (caps.queried && !caps.tier2) {
            ImGui::TextWrapped("GPU/driver does not support VRS Tier 2 (attachment-based shading rate).");
        } else {
            ImGui::Text("Status: %s", stats.active ? "active" : "inactive");
            ImGui::Text("Tile size: %u | Additional rates (4x4): %s", caps.tile_size, caps.additional_rates ? "yes" : "no");
            ImGui::Text("Shading-rate image: %ux%u | updates: %u", stats.sri_width, stats.sri_height, stats.sri_updates);
            ImGui::Text("RT binds seen/frame: %u | VRS binds applied/frame: %u", stats.rtv_binds, stats.vrs_binds);

            if (stats.subres_width != 0) {
                ImGui::Text("Upscaler render res: %ux%u | binds/frame: %u%s",
                    stats.subres_width, stats.subres_height, stats.subres_binds,
                    stats.suppressing_fullres ? " (display-res passes excluded)" : "");
            }

            if (stats.active && stats.vrs_binds == 0) {
                ImGui::TextWrapped(
                    "No render-target binds matched the scene target this frame. "
                    "Try disabling 'Only Passes With Depth Bound' in the advanced options.");
            }

            ImGui::Separator();
            m_debug_preview->draw("Debug Preview (Shading-Rate Map)");
            help_marker(
                "Injected-path analog of the engine's r.VRS.Preview: shows a color-coded map of the "
                "shading-rate image UEVR is generating this frame (green=1x1 full, yellow=2x2, red=4x4), "
                "with each eye's foveation center marked. Updates live as you adjust the settings.");

            if (m_debug_preview->value()) {
                draw_debug_preview();
            }
        }
    }

    ImGui::TreePop();
}
