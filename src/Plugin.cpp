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
        if (m_hook_id >= 0) {
            API::get()->param()->functions->unregister_inline_hook(m_hook_id);
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

    void* on_render_composite_layer_internal(FEndMenuRenderer* self, FEndMenuRenderContext* context) {
        const auto original_render_pass = context->cmd_list->bInsideRenderPass;
        const auto original_render_target = context->ui_render_target;

        if (context->ui_render_target != nullptr) {
            auto ui_render_target = API::StereoHook::get_ui_render_target();

            // Replace render target with ours
            if (ui_render_target != nullptr) {
                context->ui_render_target = ui_render_target;
                context->cmd_list->bInsideRenderPass = false; // Allows our render target to be set up
            }
        }

        auto res = m_orig_render_composite_layer(self, context);

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

        auto res = g_plugin->on_render_composite_layer_internal(self, context);

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
};


std::unique_ptr<FF7Plugin> g_plugin{std::make_unique<FF7Plugin>()};