#include <optional>
#include <mutex>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <ppl.h>

#include <utility/Scan.hpp>
#include <utility/Module.hpp>
#include <utility/Patch.hpp>

#include "uevr/Plugin.hpp"

using namespace uevr;

class FF7Plugin;
extern std::unique_ptr<FF7Plugin> g_plugin;

class SimpleScheduler {
public:
    SimpleScheduler() {
        m_evt = CreateEvent(NULL, FALSE, FALSE, NULL);

        Concurrency::SchedulerPolicy policy(1, Concurrency::ContextPriority, THREAD_PRIORITY_HIGHEST);
        m_impl = Concurrency::Scheduler::Create(policy);
        m_impl->RegisterShutdownEvent(m_evt);
        m_impl->Attach();
    }

    virtual ~SimpleScheduler() {
        if (m_impl != nullptr) {
            Concurrency::CurrentScheduler::Detach();
            m_impl->Release();

            SPDLOG_INFO("Waiting for the scheduler to shut down...");
            if (WaitForSingleObject(m_evt, 1000) == WAIT_OBJECT_0) {
                SPDLOG_INFO("Scheduler has shut down.");
                CloseHandle(m_evt);
            } else {
                SPDLOG_ERROR("Failed to wait for the scheduler to shut down.");
            }
        }
    }
private:
    Concurrency::Scheduler* m_impl{nullptr};
    HANDLE m_evt{nullptr};
};

class FF7Plugin final : public uevr::Plugin {
public:
    virtual ~FF7Plugin() {
        m_motion_blur_patch.reset();

        if (m_hook_id >= 0) {
            API::get()->param()->functions->unregister_inline_hook(m_hook_id);
        }

        if (m_post_process_settings_hook_id >= 0) {
            API::get()->param()->functions->unregister_inline_hook(m_post_process_settings_hook_id);
        }
    }

    void on_initialize() override {
        // We manually create a scheduler because doing so with the default WinRT scheduler (which is implicitly created if no scheduler is attached)
        // causes our DLL to fail to unload properly, which is bad for development for hot-reloading
        // The scan functions have concurrency calls within them which is why we need to do this
        SimpleScheduler current_thread_scheduler{};

        AllocConsole();
        freopen("CONOUT$", "w", stdout);

        // Set up spdlog to sink to the console
        spdlog::set_pattern("[%H:%M:%S] [%^%l%$] [ff7plugin] %v");
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
        spdlog::set_default_logger(spdlog::stdout_logger_mt("console"));

        SPDLOG_INFO("FF7Plugin entry point");
        hook_render_composite_layer();
        hook_post_process_settings();
        patch_motion_blur();
    }

private:
    struct FEndMenuRenderer {};
    struct FRHICommandList {
        char pad[0x204];
        bool bInsideRenderPass; // Not sure if this is its actual name but whatever
    };

    struct FEndMenuRenderContext {
        FRHICommandList* cmd_list;
        API::FRHITexture2D* ui_render_target;
    };

    using OnRenderCompositeLayerFn = void* (*)(FEndMenuRenderer* self, FEndMenuRenderContext* context);
    OnRenderCompositeLayerFn m_orig_render_composite_layer{nullptr};
    int m_hook_id{-1};
    FEndMenuRenderer* m_last_menu_renderer{nullptr};

    void* on_render_composite_layer_internal(FEndMenuRenderer* self, FEndMenuRenderContext* context, OnRenderCompositeLayerFn orig) {
        m_last_menu_renderer = self;

        if (!API::get()->param()->vr->is_hmd_active()) {
            return orig(self, context);
        }
        
        static bool once = true;

        if (once) {
            API::get()->param()->vr->set_mod_value("UI_InvertAlpha", "true");
            once = false;
        }

        const auto original_render_pass = context->cmd_list->bInsideRenderPass;
        const auto original_render_target = context->ui_render_target;

        auto ui_render_target = API::StereoHook::get_ui_render_target();

        // Replace render target with ours
        if (ui_render_target != nullptr) {
            context->ui_render_target = ui_render_target;
            context->cmd_list->bInsideRenderPass = false; // Allows our render target to be set up
        }

        orig(self, context);
        auto res = orig(self, context);

        context->ui_render_target = original_render_target;
        context->cmd_list->bInsideRenderPass = original_render_pass;

        return res;
    }

    static void* on_render_composite_layer(FEndMenuRenderer* self, FEndMenuRenderContext* context) {
        static bool once = true;

        if (once) {
            SPDLOG_INFO("FEndMenuRenderer::OnRenderCompositeLayer");
            once = false;
        }

        auto res = g_plugin->on_render_composite_layer_internal(self, context, g_plugin->m_orig_render_composite_layer);

        return res;
    }

    void hook_render_composite_layer() {
        const auto game = utility::get_executable();
        const auto ref = utility::find_function_from_string_ref(game, L"FEndMenuRenderer::OnRenderCompositeLayerEx", true);

        if (!ref) {
            API::get()->log_error("Failed to find FEndMenuRenderer::OnRenderCompositeLayer");
            return;
        }

        const auto fn = utility::find_function_start_with_call(*ref);

        if (!fn) {
            API::get()->log_error("Failed to find FEndMenuRenderer::OnRenderCompositeLayer function start");
            return;
        }

        m_hook_id = API::get()->param()->functions->register_inline_hook((void*)*fn, &on_render_composite_layer, (void**)&m_orig_render_composite_layer);

        API::get()->log_info("FEndMenuRenderer::OnRenderCompositeLayerEx hooked at 0x%p", (void*)*fn);
    }

    Patch::Ptr m_motion_blur_patch{nullptr};

    void patch_motion_blur() {
        SPDLOG_INFO("Scanning for MotionBlurIntermediate");

        const auto game = utility::get_executable();
        const auto motion_blur_intermediate_ref = utility::find_function_from_string_ref(game, L"MotionBlurIntermediate", true);

        if (!motion_blur_intermediate_ref) {
            API::get()->log_error("Failed to find MotionBlurIntermediate");
            SPDLOG_INFO("Failed to find MotionBlurIntermediate");
            return;
        }

        const auto fn = utility::find_function_start_with_call(*motion_blur_intermediate_ref);

        if (!fn) {
            API::get()->log_error("Failed to find MotionBlurIntermediate function start");
            SPDLOG_INFO("Failed to find MotionBlurIntermediate function start");
            return;
        }

        const auto fn_callsite = utility::scan_displacement_reference(game, *fn, [](uintptr_t addr) -> bool {
            return *(uint8_t*)(addr - 1) == 0xE8;
        });

        if (!fn_callsite) {
            API::get()->log_error("Failed to find MotionBlurIntermediate callsite");
            SPDLOG_INFO("Failed to find MotionBlurIntermediate callsite");
            return;
        }

        auto preceding_insns = utility::get_disassembly_behind(*fn_callsite);

        if (preceding_insns.empty()) {
            API::get()->log_error("Failed to disassemble preceding instructions for MotionBlurIntermediate");
            SPDLOG_INFO("Failed to disassemble preceding instructions for MotionBlurIntermediate");
            return;
        }

        std::reverse(preceding_insns.begin(), preceding_insns.end());

        // Find first conditional jmp
        auto jmp_insn = std::find_if(preceding_insns.begin(), preceding_insns.end(), [](const utility::Resolved& insn) {
            auto& ix = insn.instrux;
            return ix.BranchInfo.IsBranch && ix.BranchInfo.IsConditional;
        });

        if (jmp_insn == preceding_insns.end()) {
            API::get()->log_error("Failed to find conditional jmp for MotionBlurIntermediate");
            SPDLOG_INFO("Failed to find conditional jmp for MotionBlurIntermediate");
            return;
        }

        SPDLOG_INFO("Found conditional jmp at 0x{:x}", jmp_insn->addr);

        // Modify to always jmp
        //if (*(uint8_t*)(jmp_insn->addr) == 0x0F) {
        if (jmp_insn->instrux.RelOffsLength == 4) {
            SPDLOG_INFO("Patching conditional far jmp to always jmp");

            if (jmp_insn->instrux.Length == 6) {
                m_motion_blur_patch = Patch::create(jmp_insn->addr, { 0x90, 0xE9 });
                SPDLOG_INFO("Patched MotionBlurIntermediate (6 bytes)");
            } else if (jmp_insn->instrux.Length == 5) {
                m_motion_blur_patch = Patch::create(jmp_insn->addr, { 0xE9 });
                SPDLOG_INFO("Patched MotionBlurIntermediate (5 bytes)");
            } else {
                API::get()->log_error("Failed to patch MotionBlurIntermediate: unexpected instruction length");
                SPDLOG_INFO("Failed to patch MotionBlurIntermediate: unexpected instruction length");
            }
        } else {
            SPDLOG_INFO("Patching conditional jmp to always jmp");
            m_motion_blur_patch = Patch::create(jmp_insn->addr, { 0xEB });
        }

        SPDLOG_INFO("Patched MotionBlurIntermediate");
    }

    int m_post_process_settings_hook_id{-1};
    using PostProcessSettingsFn = void* (*)(void* self, void* a2, void* a3, void* a4);
    PostProcessSettingsFn m_orig_post_process_settings{nullptr};

    void* on_post_process_settings_internal(void* self, void* a2, void* a3, void* a4) {
        auto res = m_orig_post_process_settings(self, a2, a3, a4);

        static const auto post_process_settings_t = API::get()->find_uobject<API::UScriptStruct>(L"ScriptStruct /Script/Engine.PostProcessSettings");

        if (post_process_settings_t != nullptr) {
            static const auto vignette_intensity_prop = post_process_settings_t->find_property(L"VignetteIntensity");
            static const auto vignette_lens_intensity_prop = post_process_settings_t->find_property(L"LensVignetteIntensity");

            if (vignette_intensity_prop != nullptr) {
                static const auto offset = vignette_intensity_prop->get_offset();
                *(float*)((uintptr_t)self + offset) = 0.0f;
            }

            if (vignette_lens_intensity_prop != nullptr) {
                static const auto offset = vignette_lens_intensity_prop->get_offset();
                *(float*)((uintptr_t)self + offset) = 0.0f;
            }
        }

        return res;
    }

    static void* on_post_process_settings(void* self, void* a2, void* a3, void* a4) {
        static bool once = true;

        if (once) {
            SPDLOG_INFO("FPostProcessSettings::PostProcessSettings");
            once = false;
        }

        return g_plugin->on_post_process_settings_internal(self, a2, a3, a4);
    }

    void hook_post_process_settings() {
        const auto game = utility::get_executable();
        const auto str = utility::scan_string(game, L"r.DefaultFeature.AutoExposure.Bias");

        if (!str) {
            API::get()->log_error("Failed to find r.DefaultFeature.AutoExposure.Bias");
            return;
        }

        std::optional<uintptr_t> func_start{};

        const auto ref = utility::scan_displacement_reference(game, *str, [&](uintptr_t addr) -> bool {
            func_start = utility::find_function_start_with_call(addr);

            if (!func_start) {
                return false;
            }

            size_t refs{};
            utility::scan_displacement_reference(game, *func_start, [&](uintptr_t addr2) -> bool {
                ++refs;
                return false;
            });

            return refs > 3;
        });

        if (!ref) {
            API::get()->log_error("Failed to find r.DefaultFeature.AutoExposure.Bias callsite");
            return;
        }

        SPDLOG_INFO("r.DefaultFeature.AutoExposure.Bias callsite at 0x{:x}", *ref);
        SPDLOG_INFO("FPostProcessSettings::FPostProcessSettings at 0x{:x}", *func_start);

        m_post_process_settings_hook_id = API::get()->param()->functions->register_inline_hook((void*)*func_start, &on_post_process_settings, (void**)&m_orig_post_process_settings);
    }
};


std::unique_ptr<FF7Plugin> g_plugin{std::make_unique<FF7Plugin>()};