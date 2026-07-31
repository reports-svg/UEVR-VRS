#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <wrl.h>

#include "../../Mod.hpp"

#include <SafetyHook.hpp>

#include <sdk/RHICommandList.hpp>
#include <sdk/StereoStuff.hpp>

namespace sdk {
class FRenderTargetPool;
class FPooledRenderTargetDesc;
}

class RenderTargetPoolHook : public ModComponent {
public:
    RenderTargetPoolHook();
    void on_pre_engine_tick(sdk::UGameEngine* engine, float delta) override;
    void activate() {
        m_wants_activate = true;
    }

    IPooledRenderTarget* get_render_target(std::wstring_view name) {
        std::scoped_lock _{m_mutex};
        if (auto it = m_render_targets.find(name); it != m_render_targets.end()) {
            return it->second;
        }

        return nullptr;
    }

    template<typename T>
    Microsoft::WRL::ComPtr<T> get_texture(std::wstring_view name) {
        std::scoped_lock _{m_mutex};
        if (auto it = m_render_targets.find(name); it != m_render_targets.end()) {
            const auto& rt = it->second;
            const auto& tex = rt->item.texture.texture;

            if (tex == nullptr) {
                return nullptr;
            }

            auto native_resource = (T*)tex->get_native_resource();

            if (native_resource == nullptr) {
                return nullptr;
            }

            return native_resource;
        }

        return nullptr;
    }

private:
    bool hook();

    // Stuff past name param is added in newer UE versions.
    static bool find_free_element_hook(
        sdk::FRenderTargetPool* pool, 
        sdk::FRHICommandListBase* cmd_list,
        sdk::FPooledRenderTargetDesc* desc,
        TRefCountPtr<IPooledRenderTarget>* out,
        const wchar_t* name,
        // these arent uintptrs, just defending against future changes to the size of the params
        uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10);

    static bool find_free_element_hook_ue5(
        sdk::FRenderTargetPool* pool,
        sdk::FPooledRenderTargetDesc* desc,
        TRefCountPtr<IPooledRenderTarget>* out,
        const wchar_t* name,
        // these arent uintptrs, just defending against future changes to the size of the params
        uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10
    );

    void on_post_find_free_element(sdk::FRenderTargetPool* pool, 
        sdk::FPooledRenderTargetDesc* desc, 
        TRefCountPtr<IPooledRenderTarget>* out, 
        const wchar_t* name
    );

    bool m_attempted_hook{false};
    bool m_hooked{false};
    bool m_wants_activate{false};

    // Transparent hashing so the per-FindFreeElement lookups (dozens+ per frame
    // on the render thread when depth submission is on) can key on the raw
    // wchar_t* without constructing a std::wstring per call.
    struct TransparentWStrHash {
        using is_transparent = void;
        size_t operator()(std::wstring_view s) const noexcept { return std::hash<std::wstring_view>{}(s); }
        size_t operator()(const std::wstring& s) const noexcept { return std::hash<std::wstring_view>{}(s); }
        size_t operator()(const wchar_t* s) const noexcept { return std::hash<std::wstring_view>{}(s); }
    };

    std::recursive_mutex m_mutex{};
    SafetyHookInline m_find_free_element_hook{};
    std::unordered_map<std::wstring, IPooledRenderTarget*, TransparentWStrHash, std::equal_to<>> m_render_targets{};
    std::unordered_set<std::wstring, TransparentWStrHash, std::equal_to<>> m_seen_names{};
};