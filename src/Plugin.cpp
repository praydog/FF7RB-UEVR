#include <optional>
#include <mutex>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <ppl.h>

#include <utility/Scan.hpp>
#include <utility/Module.hpp>
#include <utility/Patch.hpp>
#include <utility/String.hpp>

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

        if (m_startframe_hook_id >= 0) {
            API::get()->param()->functions->unregister_inline_hook(m_startframe_hook_id);
        }

        if (m_increment_frame_count_hook_id >= 0) {
            API::get()->param()->functions->unregister_inline_hook(m_increment_frame_count_hook_id);
        }

        if (m_update_transform_hook_id >= 0) {
            API::get()->param()->functions->unregister_inline_hook(m_update_transform_hook_id);
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

        const auto framenum_ref = utility::scan(utility::get_executable(), "FF 05 ? ? ? ? 48 8D 0D ? ? ? ? 48 89 9C 24 88 00 00 00");

        if (framenum_ref) {
            SPDLOG_INFO("Found GFrameNumberRenderThread at 0x{:x}", *framenum_ref);
            GFrameNumberRenderThread = (uint32_t*)utility::calculate_absolute(*framenum_ref + 2);
        }

        hook_render_composite_layer();
        hook_post_process_settings();
        patch_motion_blur();
        hook_startframe();
        hook_update_transform();
        hook_create_scene_renderer();
    }

    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {
        m_using_native_stereo = API::VR::get_mod_value<int>("VR_RenderingMethod") == 0;
        m_ghosting_fix_enabled = API::VR::get_mod_value<bool>("VR_GhostingFix") == true;
        m_is_hmd_active = API::VR::is_hmd_active();
    }

private:
    uint32_t* GFrameNumberRenderThread{nullptr};
    bool m_ghosting_fix_enabled{false};
    bool m_using_native_stereo{false};
    bool m_is_hmd_active{false};

    struct FEndMenuRenderer {
        uint8_t& counter() {
            return *(uint8_t*)((uintptr_t)this + 0x69);
        }

        uint32_t& max_counter() {
            return *(uint32_t*)((uintptr_t)this + 0x60);
        }

        uintptr_t& some_pointer() {
            return *(uintptr_t*)((uintptr_t)this + 0x58);
        }
    };

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
    API::FRHITexture2D* m_last_stereo_texture{nullptr};

    void* on_render_composite_layer_internal(FEndMenuRenderer* self, FEndMenuRenderContext* context, OnRenderCompositeLayerFn orig) {
        m_last_menu_renderer = self;

        if (!m_is_hmd_active) {
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

    using FScene_StartFrame = void* (*)(void* self, void* a2, void* a3, void* a4);
    FScene_StartFrame m_orig_startframe{nullptr};
    int m_startframe_hook_id{-1};
    uint32_t m_last_frame_count{0};
    size_t m_last_real_frame_count{0};
    size_t m_skipped_frames{0};
    uint32_t m_velocity_data_offset{0};
    uintptr_t m_start_frame_vtable_addr{0};
    uint32_t m_scene_frame_count_offset{0};
    uint32_t m_last_scene_frame_count{0};
    void* m_last_scene{nullptr};

    void* on_startframe_internal(void* self, void* a2, void* a3, void* a4) {
        uintptr_t velocity_data = ((uintptr_t)self + m_velocity_data_offset);
        size_t& internal_frame_count = *(size_t*)velocity_data;

        m_last_real_frame_count = internal_frame_count;

        // We don't care to do anything with this function if we're running in native stereo.
        if (!m_is_hmd_active || m_using_native_stereo || !m_ghosting_fix_enabled) {
            return m_orig_startframe(self, a2, a3, a4);
        }

        auto scene_frame_count = m_scene_frame_counts[(uintptr_t)self];

        void* res = nullptr;
        // Only update velocity stuff every other frame
        if ((*GFrameNumberRenderThread % 2) == 0) {
            res = m_orig_startframe(self, a2, a3, a4);
        }

        return res;
    }

    static void* on_startframe(void* self, void* a2, void* a3, void* a4) {
        static bool once = true;

        if (once) {
            SPDLOG_INFO("FScene::StartFrame");
            once = false;
        }

        return g_plugin->on_startframe_internal(self, a2, a3, a4);
    }

    using FScene_IncrementFrameCount = void* (*)(void* self, void* a2, void* a3, void* a4);
    FScene_IncrementFrameCount m_orig_increment_frame_count{nullptr};
    int m_increment_frame_count_hook_id{-1};
    uint32_t m_skipped_scene_frames{};

    void* on_increment_frame_count_internal(void* self, void* a2, void* a3, void* a4) {
        m_last_scene = self;

        uint32_t& scene_frame_count = *(uint32_t*)((uintptr_t)self + m_scene_frame_count_offset);
        m_last_scene_frame_count = scene_frame_count + 1;

        return g_plugin->m_orig_increment_frame_count(self, a2, a3, a4);
    }

    static void* on_increment_frame_count(void* self, void* a2, void* a3, void* a4) {
        static bool once = true;

        if (once) {
            SPDLOG_INFO("FScene::IncrementFrameCount");
            once = false;
        }

        return g_plugin->on_increment_frame_count_internal(self, a2, a3, a4);
    }

    using FVelocityData_UpdateTransform = void* (*)(void* self, void* a2, void* a3, void* a4);
    FVelocityData_UpdateTransform m_orig_update_transform{nullptr};
    int m_update_transform_hook_id{-1};

    std::unordered_map<uintptr_t, uint32_t> m_velocity_to_scene_frame_counts{};
    std::unordered_map<uintptr_t, uint32_t> m_scene_frame_counts{};

    void* on_update_transform_internal(void* self, void* a2, void* a3, void* a4) {
        const auto scene = ((uintptr_t)self - m_velocity_data_offset);
        const auto scene_frame_count = *(uint32_t*)(scene + m_scene_frame_count_offset);
        const auto velocity_frame_count = *(size_t*)self;

        if (m_is_hmd_active && !m_using_native_stereo) {
            // Don't update velocity transform on odd frames
            if ((*GFrameNumberRenderThread % 2) == 1) {
                return nullptr;
            }
        }

        return m_orig_update_transform(self, a2, a3, a4);
    }

    static void* on_update_transform(void* self, void* a2, void* a3, void* a4) {
        static bool once = true;

        if (once) {
            SPDLOG_INFO("FVelocityData::UpdateTransform");
            once = false;
        }

        return g_plugin->on_update_transform_internal(self, a2, a3, a4);
    }

    using FScene_UpdateAllPrimitiveSceneInfos = void* (*)(void* self, void* a2, void* a3, void* a4);
    FScene_UpdateAllPrimitiveSceneInfos m_orig_update_all_primitive_scene_infos{nullptr};
    int m_update_all_primitive_scene_infos_hook_id{-1};

    std::unordered_set<uintptr_t> m_scenes{};
    std::unordered_map<uintptr_t, size_t> m_prim_frame_count{};

    void* update_all_primitive_scene_infos_internal(void* scene, void* a2, void* a3, void* a4) {
        if (!m_scenes.contains((uintptr_t)scene)) {
            m_scenes.insert((uintptr_t)scene);
            SPDLOG_INFO("FScene::UpdateAllPrimitiveSceneInfos: scene = 0x{:x}", (uintptr_t)scene);
        }

        auto res = m_orig_update_all_primitive_scene_infos(scene, a2, a3, a4);

        return res;
    }

    static void* update_all_primitive_scene_infos(void* self, void* a2, void* a3, void* a4) {
        static bool once = true;

        if (once) {
            SPDLOG_INFO("FScene::UpdateAllPrimitiveSceneInfos");
            once = false;
        }

        return g_plugin->update_all_primitive_scene_infos_internal(self, a2, a3, a4);
    }

    struct FMatrix {
        float m[4][4]{};
    };

    using FScene_GetPrimitiveUniformShaderParameters_RenderThread = void* (*)(void* self, void* primitive_scene_info, void* a3, FMatrix* a4, int32_t& single_capture_index, bool& output_velocity);
    FScene_GetPrimitiveUniformShaderParameters_RenderThread m_orig_get_primitive_uniform_shader_parameters_render_thread{nullptr};
    int m_get_primitive_uniform_shader_parameters_render_thread_hook_id{-1};

    std::unordered_map<uint32_t, FMatrix> m_previous_local_to_worlds{};

    void* get_primitive_uniform_shader_parameters_render_thread_internal(void* scene, void* primitive_scene_info, void* a3, FMatrix* previous_local_to_world, int32_t& single_capture_index, bool& output_velocity) {
        auto velocity_data = (uintptr_t)scene + m_velocity_data_offset;
        auto& scene_frame_count = *(uint32_t*)((uintptr_t)scene + m_scene_frame_count_offset);
        auto& velocity_frame_count = *(size_t*)velocity_data;
        uint32_t prim_id = *(uint32_t*)((uintptr_t)primitive_scene_info + 0x10);

        if (scene_frame_count != m_scene_frame_counts[(uintptr_t)scene]) {
            m_scene_frame_counts[(uintptr_t)scene] = scene_frame_count;
            m_velocity_to_scene_frame_counts[velocity_data] = scene_frame_count;
        }

        if (true) {
            auto res = m_orig_get_primitive_uniform_shader_parameters_render_thread(scene, primitive_scene_info, a3, previous_local_to_world, single_capture_index, output_velocity);

            return res;
        }

#if 0
        if (((velocity_frame_count + m_skipped_frames) % 2) == 0) {
            *(bool*)a3 = false;
            void* proxy = *(void**)((uintptr_t)primitive_scene_info + 0x8);

            if (proxy != nullptr) {
                //memcpy(previous_local_to_world, (FMatrix*)((uintptr_t)proxy + 0x80), sizeof(FMatrix));
                m_orig_get_primitive_uniform_shader_parameters_render_thread(scene, primitive_scene_info, a3, previous_local_to_world, single_capture_index, output_velocity);
                if (output_velocity) {
                    auto real_last = *previous_local_to_world;
                    *previous_local_to_world = m_previous_local_to_worlds[prim_id];
                    m_previous_local_to_worlds[prim_id] = real_last;
                    output_velocity = true;
                    single_capture_index = *(int32_t*)((uintptr_t)primitive_scene_info + 0x80);
                }
            } else {
                auto res = m_orig_get_primitive_uniform_shader_parameters_render_thread(scene, primitive_scene_info, a3, previous_local_to_world, single_capture_index, output_velocity);
                m_previous_local_to_worlds[prim_id] = *previous_local_to_world;

                return res;
            }
        } else {
            auto res = m_orig_get_primitive_uniform_shader_parameters_render_thread(scene, primitive_scene_info, a3, previous_local_to_world, single_capture_index, output_velocity);
            m_previous_local_to_worlds[prim_id] = *previous_local_to_world;

            return res;
        }
#endif

        return nullptr;
    }

    static void* get_primitive_uniform_shader_parameters_render_thread(void* self, void* primitive_scene_info, void* a3, FMatrix* previous_local_to_world, int32_t& single_capture_index, bool& output_velocity) {
        static bool once = true;

        if (once) {
            SPDLOG_INFO("FScene::GetPrimitiveUniformShaderParameters_RenderThread");
            once = false;
        }

        return g_plugin->get_primitive_uniform_shader_parameters_render_thread_internal(self, primitive_scene_info, a3, previous_local_to_world, single_capture_index, output_velocity);
    }

    using FSceneRenderer_CreateSceneRenderer = void* (*)(void* self, void* a2, void* a3, void* a4);
    FSceneRenderer_CreateSceneRenderer m_orig_create_scene_renderer{nullptr};
    int m_create_scene_renderer_hook_id{-1};

    void* create_scene_renderer_internal(void* self, void* a2, void* a3, void* a4) {
        auto scene = m_last_scene;

        return m_orig_create_scene_renderer(self, a2, a3, a4);
    }

    static void* create_scene_renderer(void* self, void* a2, void* a3, void* a4) {
        static bool once = true;

        if (once) {
            SPDLOG_INFO("FSceneRenderer::CreateSceneRenderer");
            once = false;
        }

        return g_plugin->create_scene_renderer_internal(self, a2, a3, a4);
    }

    void hook_startframe() {
        SPDLOG_INFO("Scanning for FScene::StartFrame");
        const auto game = utility::get_executable();
        const auto game_size = utility::get_module_size(game).value_or(0);
        const auto game_end = (uintptr_t)game + game_size;

        std::optional<uintptr_t> fn{};

        uint64_t magic_constant = 0x47AE147AE147AE15;
        for (auto magic_constant_ref  = utility::scan_data(game, (uint8_t*)&magic_constant, sizeof(magic_constant));
            magic_constant_ref;
            magic_constant_ref = utility::scan_data(*magic_constant_ref + 1, game_end - (*magic_constant_ref + 1), (uint8_t*)&magic_constant, sizeof(magic_constant))) 
        {
            // There's a quirk with this function where it uses a tail jmp
            // optimization which causes us to unwind to something other than the basic block
            // found by find_function_start. This is useful for us because it's very obvious
            const auto fn_start_unwind = utility::find_function_start_unwind(*magic_constant_ref);

            if (!fn_start_unwind) {
                continue;
            }

            const auto fn_jmp = utility::scan_displacement_reference(game, *fn_start_unwind, [](uintptr_t addr) -> bool {
                return *(uint8_t*)(addr - 1) == 0xE9; // JMP
            });

            if (!fn_jmp) {
                continue;
            }

            const auto fn_start = utility::find_function_start_unwind(*fn_jmp);

            if (!fn_start) {
                continue;
            }

            // Means it's a virtual function which is what we want
            if (auto vtable_addr = utility::scan_ptr(game, *fn_start)) {
                m_start_frame_vtable_addr = *vtable_addr;
                const auto get_frame_count_fn = *(uintptr_t*)(m_start_frame_vtable_addr + sizeof(void*)); // + 1
                m_scene_frame_count_offset = *(uint32_t*)(get_frame_count_fn + 2);
                const auto increment_frame_count_fn = *(uintptr_t*)(m_start_frame_vtable_addr + (sizeof(void*) * 2));

                m_increment_frame_count_hook_id = API::get()->param()->functions->register_inline_hook((void*)increment_frame_count_fn, &on_increment_frame_count, (void**)&m_orig_increment_frame_count);
                
                SPDLOG_INFO("FScene::StartFrame vtable func at 0x{:x}", *vtable_addr);
                SPDLOG_INFO("FScene::StartFrame frame count offset at 0x{:x}", m_scene_frame_count_offset);

                const auto get_primitive_uniform_shader_parameters_render_thread_fn = *(uintptr_t*)(m_start_frame_vtable_addr - (sizeof(void*) * 48));

                m_get_primitive_uniform_shader_parameters_render_thread_hook_id = API::get()->param()->functions->register_inline_hook((void*)get_primitive_uniform_shader_parameters_render_thread_fn, &get_primitive_uniform_shader_parameters_render_thread, (void**)&m_orig_get_primitive_uniform_shader_parameters_render_thread);

                SPDLOG_INFO("FScene::GetPrimitiveUniformShaderParameters_RenderThread hooked at 0x{:x}", get_primitive_uniform_shader_parameters_render_thread_fn);


                fn = fn_start;

                const auto add_rcx = utility::scan_disasm(*fn, 20, "48 81 C1 ? ? ? ?");
                if (add_rcx) {
                    m_velocity_data_offset = *(uint32_t*)(*add_rcx + 3);
                    SPDLOG_INFO("FScene::StartFrame velocity data offset = 0x{:x}", m_velocity_data_offset);
                }

                break;
            }
        }

        if (!fn) {
            SPDLOG_ERROR("Failed to find FScene::StartFrame");
            return;
        }

        m_startframe_hook_id = API::get()->param()->functions->register_inline_hook((void*)*fn, &on_startframe, (void**)&m_orig_startframe);

        SPDLOG_INFO("FScene::StartFrame hooked at 0x{:x}", *fn);
    }

    void hook_update_transform() {
        SPDLOG_INFO("Scanning for FVelocityData::UpdateTransform");

        const auto game = utility::get_executable();
        const auto fn = utility::scan(game, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 55 41 56 41 57 B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 8B 41 10");

        if (!fn) {
            API::get()->log_error("Failed to find FVelocityData::UpdateTransform");
            return;
        }

        m_update_transform_hook_id = API::get()->param()->functions->register_inline_hook((void*)*fn, &on_update_transform, (void**)&m_orig_update_transform);

        SPDLOG_INFO("FVelocityData::UpdateTransform hooked at 0x{:x}", *fn);

        const auto call_ref = utility::scan_displacement_reference(game, *fn, [](uintptr_t addr) -> bool {
            return *(uint8_t*)(addr - 1) == 0xE8; // CALL
        });

        if (!call_ref) {
            API::get()->log_error("Failed to find FScene::UpdateAllPrimitiveSceneInfos");
            return;
        }

        const auto update_all_primitive_scene_infos_fn = utility::find_function_start_unwind(*call_ref);

        if (!update_all_primitive_scene_infos_fn) {
            API::get()->log_error("Failed to find FScene::UpdateAllPrimitiveSceneInfos");
            return;
        }

        m_update_all_primitive_scene_infos_hook_id = API::get()->param()->functions->register_inline_hook((void*)*update_all_primitive_scene_infos_fn, &update_all_primitive_scene_infos, (void**)&m_orig_update_all_primitive_scene_infos);

        SPDLOG_INFO("FScene::UpdateAllPrimitiveSceneInfos hooked at 0x{:x}", *update_all_primitive_scene_infos_fn);
    }

    void hook_create_scene_renderer() {
#if 0
        SPDLOG_INFO("Scanning for FSceneRenderer::CreateSceneRenderer");

        const auto game = utility::get_executable();
        // lord forgive me
        const auto fn = utility::scan(game, "48 89 5C 24 08 57 B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 48 8B F9 48 8B DA B9 ? ? ? ? E8 ? ? ? ? 48 8B C8 33 C0 48 85 C9 74 ? 4C 8B C3 48 8B D7 E8 ? ? ? ? 48 8B 5C 24 30 48 83 C4 ? 5F C3 CC 48 89 5C 24 08 48 89 74 24 10 57 B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 48 8B D9");

        if (!fn) {
            API::get()->log_error("Failed to find FSceneRenderer::CreateSceneRenderer");
            return;
        }

        m_create_scene_renderer_hook_id = API::get()->param()->functions->register_inline_hook((void*)*fn, &create_scene_renderer, (void**)&m_orig_create_scene_renderer);

        SPDLOG_INFO("FSceneRenderer::CreateSceneRenderer hooked at 0x{:x}", *fn);
#endif
    }
};


std::unique_ptr<FF7Plugin> g_plugin{std::make_unique<FF7Plugin>()};