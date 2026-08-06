#define NOMINMAX

#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtx/transform.hpp>

#include <utility/Profiler.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/TDBVer.hpp>
#include <reframework/API.hpp>
#include <sdk/GameIdentity.hpp>

#ifdef REFRAMEWORK_UNIVERSAL
#include "sdk/regenny/re9/via/Window.hpp"
#include "sdk/regenny/re9/via/SceneView.hpp"
#include <sdk/ViaDispatch.hpp>
#else
#if TDB_VER >= 83
#include "sdk/regenny/re9/via/Window.hpp"
#include "sdk/regenny/re9/via/SceneView.hpp"
#elif TDB_VER <= 49
#include "sdk/regenny/re7/via/Window.hpp"
#include "sdk/regenny/re7/via/SceneView.hpp"
#elif TDB_VER < 69
#include "sdk/regenny/re3/via/Window.hpp"
#include "sdk/regenny/re3/via/SceneView.hpp"
#elif TDB_VER == 69
#include "sdk/regenny/re8/via/Window.hpp"
#include "sdk/regenny/re8/via/SceneView.hpp"
#elif TDB_VER == 70
#include "sdk/regenny/re2_tdb70/via/Window.hpp"
#include "sdk/regenny/re2_tdb70/via/SceneView.hpp"
#elif TDB_VER >= 71
#ifdef SF6
#include "sdk/regenny/sf6/via/Window.hpp"
#include "sdk/regenny/sf6/via/SceneView.hpp"
#elif defined(RE4)
#include "sdk/regenny/re4/via/Window.hpp"
#include "sdk/regenny/re4/via/SceneView.hpp"
#elif defined(DD2) || TDB_VER >= 74
#include "sdk/regenny/dd2/via/Window.hpp"
#include "sdk/regenny/dd2/via/SceneView.hpp"
#else
#include "sdk/regenny/mhrise_tdb71/via/Window.hpp"
#include "sdk/regenny/mhrise_tdb71/via/SceneView.hpp"
#endif
#endif
#endif

#include "sdk/Math.hpp"
#include "sdk/SceneManager.hpp"
#include "sdk/RETypeDB.hpp"
#include "sdk/Renderer.hpp"
#include "sdk/Application.hpp"
#include "sdk/Renderer.hpp"
#include "sdk/REMath.hpp"
#include "sdk/REGameObject.hpp"

#include "utility/Scan.hpp"
#include "utility/FunctionHook.hpp"
#include "utility/Module.hpp"
#include "utility/Memory.hpp"
#include "utility/Registry.hpp"
#include "utility/ScopeGuard.hpp"

#include "FirstPerson.hpp"
#include "ManualFlashlight.hpp"
#include "TemporalUpscaler.hpp"
#include "vr/Flat3DGuiRedirect.hpp"
#include "VR.hpp"

bool inside_on_end = false;
uint32_t actual_frame_count = 0;

thread_local bool inside_gui_draw = false;

std::shared_ptr<VR>& VR::get() {
    static auto inst = std::make_shared<VR>();
    return inst;
}

std::unique_ptr<FunctionHook> g_input_hook{};
std::unique_ptr<FunctionHook> g_projection_matrix_hook2{};
std::unique_ptr<FunctionHook> g_overlay_draw_hook{};
std::unique_ptr<FunctionHook> g_post_effect_draw_hook{};
std::unique_ptr<FunctionHook> g_wwise_listener_update_hook{};
//std::unique_ptr<FunctionHook> g_get_sharpness_hook{};

#ifndef REFRAMEWORK_UNIVERSAL
#if TDB_VER <= 49
std::optional<regenny::via::Size> g_previous_size{};
#endif
#else
std::optional<regenny::via::Size> g_previous_size{};
#endif

// Purpose: spoof the render target size to the size of the HMD displays
void VR::on_view_get_size(REManagedObject* scene_view, float* result) {
    // There are some very dumb optimizations that cause set_DisplayType
    // to go through this hook. This function is actually something like "updateSceneView"
    static thread_local bool already_inside = false;

    if (already_inside) {
        return;
    }

    already_inside = true;

    utility::ScopeGuard _{ [&]() { already_inside = false; } };

    const auto& gi = sdk::GameIdentity::get();

    if (!g_framework->is_ready()) {
        return;
    }

    if (!get_runtime()->loaded) {
        return;
    }

    if (m_disable_backbuffer_size_override) {
        return;
    }

    // Flatscreen 3D: the game renders at its own native resolution and display
    // mode; no HMD sizing or display-type forcing.
    if (is_using_flat3d()) {
        return;
    }

    void* window = sdk::via::sv_window(scene_view);

    static auto via_scene_view = sdk::find_type_definition("via.SceneView");
    static auto set_display_type_method = via_scene_view != nullptr ? via_scene_view->get_method("set_DisplayType") : nullptr;

    // Force the display to stretch to the window size
    if (set_display_type_method != nullptr) {
        set_display_type_method->call(sdk::get_thread_context(), scene_view, via::DisplayType::Fit);
    } else {
        if (!gi.is_re7() || gi.tdb_ver() <= 49) {
            static auto is_sunbreak = utility::get_module_path(utility::get_executable())->find("MHRiseSunbreakDemo") != std::string::npos;

            if (is_sunbreak) {
                *(int32_t*)((uintptr_t)scene_view + sdk::via::sv_display_type_offset() + 4) = (int32_t)regenny::via::DisplayType::Fit;
            } else {
                sdk::via::sv_display_type(scene_view) = (int32_t)regenny::via::DisplayType::Fit;
            }
        } else {
            *(int32_t*)((uintptr_t)scene_view + sdk::via::sv_display_type_offset() + 4) = (int32_t)regenny::via::DisplayType::Fit;
        }
    }

    auto wanted_width = 0.0f;
    auto wanted_height = 0.0f;

    // Set the window size, which will increase the size of the backbuffer
    if (window != nullptr) {
        static const auto is_gng = utility::get_module_path(utility::get_executable())->find("makaimura_GG_RE.exe") != std::string::npos;

        auto& window_width = is_gng ? *(uint32_t*)((uintptr_t)window + 0x48) : sdk::via::window_width(window);
        auto& window_height = is_gng ? *(uint32_t*)((uintptr_t)window + 0x4C) : sdk::via::window_height(window);

        if (is_hmd_active()) {
            if (!g_previous_size) {
                g_previous_size = regenny::via::Size{ (float)sdk::via::window_width(window), (float)sdk::via::window_height(window) };
            }

            if (!TemporalUpscaler::get()->activated()) {
                if (!is_using_multipass()) {
                    window_width = get_hmd_width();
                    window_height = get_hmd_height();
                } else {
                    window_width = get_hmd_width() + 1;
                    window_height = get_hmd_height() + 1;
                }
            } else {
                window_width = get_hmd_width();
                window_height = get_hmd_height();
            }

            if (m_is_d3d12 && m_d3d12.is_initialized()) {
                const auto& backbuffer_size = m_d3d12.get_backbuffer_size();

                if (backbuffer_size[0] > 0 && backbuffer_size[1] > 0) {
                    if (std::abs((int)backbuffer_size[0] - (int)window_width) > 50 || std::abs((int)backbuffer_size[1] - (int)window_height) > 50) {
                        const auto now = get_game_frame_count();

                        if (!m_backbuffer_inconsistency) {
                            m_backbuffer_inconsistency_start = now;
                            m_backbuffer_inconsistency = true;
                        }

                        const auto is_true_inconsistency = (now - m_backbuffer_inconsistency_start) >= 5;

                        if (is_true_inconsistency) {
                            // Force a reset of the backbuffer size
                            window_width = window_width + 1;
                            window_height = window_height + 1;

                            spdlog::info("[VR] Previous backbuffer size: {}x{}", backbuffer_size[0], backbuffer_size[1]);
                            spdlog::info("[VR] Backbuffer size inconsistency detected, resetting backbuffer size to {}x{}", window_width, window_height);

                            // m_backbuffer_inconsistency gets set to false on device reset.
                        }
                    }
                } else {
                    m_backbuffer_inconsistency = false;
                }
            }
        } else {
            m_backbuffer_inconsistency = false;

            window_width = (uint32_t)g_previous_size->w;
            window_height = (uint32_t)g_previous_size->h;
            g_previous_size = std::nullopt;
        }

        wanted_width = (float)window_width;
        wanted_height = (float)window_height;

        // Might be usable in other games too
        if (gi.is_sf6() || gi.tdb_ver() >= 69) {
            if (!is_gng) {
                sdk::via::window_borderless_w(window) = (float)window_width;
                sdk::via::window_borderless_h(window) = (float)window_height;
            }
        }
    }

    //auto out = original_func(scene_view, result);

    if (!m_in_render) {
        //return original_func(scene_view, result);
    }

    // spoof the size to the HMD's size
    if (!TemporalUpscaler::get()->activated()) {
        if (!is_using_multipass()) {
            if (gi.tdb_ver() < 73) {
                result[0] = wanted_width;
                result[1] = wanted_height;
            } else {
                // Stupid optimizations cause the game to not use the result variant of this function
                // but rather update the current scene view's size directly.
                sdk::via::sv_size_w(scene_view) = wanted_width;
                sdk::via::sv_size_h(scene_view) = wanted_height;
            }
        } else {
            if (gi.tdb_ver() < 73) {
                result[0] = wanted_width - 1.0f;
                result[1] = wanted_height - 1.0f;
            } else {
                sdk::via::sv_size_w(scene_view) = wanted_width - 1.0f;
                sdk::via::sv_size_h(scene_view) = wanted_height - 1.0f;
            }
        }
    }
}

void VR::on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (result == nullptr || !g_framework->is_ready() || !is_hmd_active() || m_disable_projection_matrix_override) {
        return;
    }

    /*if (camera != sdk::get_primary_camera()) {
        return original_func(camera, result);
    }*/

    if (!m_in_render) {
       // return original_func(camera, result);
    }

    if (m_in_lightshaft) {
        //return original_func(camera, result);
    }

    if (is_using_multipass()) {
        if (camera != m_multipass_cameras[0] && camera != m_multipass_cameras[1]) {
            return;
        }
    }

#ifdef RE4
    if (const auto game_object = ((REComponent*)camera)->ownerGameObject; game_object != nullptr) {
        if (game_object->name != nullptr) {
            // Allows the sniper scope to work
            if (utility::re_string::get_view(game_object->name).starts_with(L"ScopeCamera")) {
                return;
            }
        }
    }
#endif

    if (is_using_flat3d()) {
        // Keep the game's own projection (FoV/aspect/near/far untouched); add
        // only the horizontal off-axis shear that provides convergence.
        const auto count = get_eye_pass_index();
        const auto is_left = count % 2 == m_left_eye_interval;

        if (is_left) {
            m_flat3d_game_p00 = (*result)[0][0];
            m_flat3d_game_p11 = (*result)[1][1];
        }

        const auto dir = is_left ? m_flat3d->shear_dir_left : -m_flat3d->shear_dir_left;
        const auto pre_shear = *result; // game projection before our convergence shear

        // AFW renders PARALLEL (shear-free): the plugin's reprojection is translation-driven and
        // ignores the shear, which parked the skybox at screen depth in the warped eye. Convergence
        // moves to the compose as the equivalent per-eye image shift (build_flat3d_params), applied
        // identically to both eyes - infinity lands at proper depth in both.
        if (!is_using_flat3d_afw()) {
            (*result)[2][0] += dir * (m_flat3d->separation_eff * 0.5f / m_flat3d->convergence) * (*result)[0][0];
        }

        // AFW: record BOTH eyes' projections same-tick (this pass's sheared one + the opposite
        // shear). Present-time warp indexes by its own fill parity.
        if (is_using_afr()) {
            const uint32_t e = is_left ? 0u : 1u;
            if (is_using_flat3d_afw()) {
                // Parallel projections: both eyes share the unsheared matrix.
                m_afw_frame.proj[e] = pre_shear;
                m_afw_frame.proj[e ^ 1] = pre_shear;
            } else {
                m_afw_frame.proj[e] = *result;
                m_afw_frame.proj[e ^ 1] = pre_shear;
                m_afw_frame.proj[e ^ 1][2][0] += -dir * (m_flat3d->separation_eff * 0.5f / m_flat3d->convergence) * pre_shear[0][0];
            }
        }
        return;
    }

    // Get the projection matrix for the correct eye
    // For some reason we need to flip the projection matrix here?
    *result = get_current_projection_matrix(false);
}

Matrix4x4f* VR::gui_camera_get_projection_matrix_hook(REManagedObject* camera, Matrix4x4f* result) {
    auto original_func = g_projection_matrix_hook2->get_original<decltype(VR::gui_camera_get_projection_matrix_hook)>();

    auto& vr = VR::get();

    if (result == nullptr || !g_framework->is_ready() || !vr->is_hmd_active() || vr->m_disable_gui_camera_projection_matrix_override || vr->is_using_flat3d()) {
        // Flatscreen 3D: the GUI camera stays fully game-controlled - the overlay-RT redirect
        // handles GUI depth at compose time (the retired world-space GUI experiment substituted
        // the scene perspective here).
        return original_func(camera, result);
    }

    /*if (camera != sdk::get_primary_camera()) {
        return original_func(camera, result);
    }*/

    if (!vr->m_in_render) {
       // return original_func(camera, result);
    }

    if (vr->m_in_lightshaft) {
        //return original_func(camera, result);
    }

    // Get the projection matrix for the correct eye
    // For some reason we need to flip the projection matrix here?
    if (sdk::GameIdentity::get().tdb_ver() > 49) {
        *result = vr->get_current_projection_matrix(false);
    } else {
        *result = vr->get_current_projection_matrix(true);
    }

    return result;
}

void VR::on_camera_get_view_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (result == nullptr || !g_framework->is_ready()) {
        return;
    }

    if (!is_hmd_active() || m_disable_view_matrix_override) {
        return;
    }

    auto cameras = get_cameras();

    if (camera != cameras[0] && camera != cameras[1]) {
        return;
    }

    // A little note on this. We can't actually use the camera (when using multipass) to detect how to adjust the view matrix
    // For some reason, the game *always* uses the main camera when calling this function, even though it's rendering the other camera
    // So we detect which pass is getting called and adjust the view matrix accordingly
    //if (!is_using_multipass()) {
        auto& mtx = *result;

        //get the flipped eye to get the correct transform. something something right->left handedness i think
        const auto current_eye_transform = get_current_eye_transform(true);
        //auto current_head_pos = -(glm::inverse(vr->get_rotation(0)) * ((vr->get_position(0)) - vr->m_standing_origin));
        //current_head_pos.w = 0.0f;

        // AFW: record BOTH eyes' views same-tick from this tick's base matrix (this pass's transform
        // + the opposite one). Present-time warp indexes by its own fill parity.
        if (is_using_flat3d() && is_using_afr()) {
            const auto other_eye_transform = get_current_eye_transform(false);
            const auto count = get_eye_pass_index();
            const uint32_t e = (count % 2 == m_left_eye_interval) ? 0u : 1u;
            m_afw_frame.view[e] = current_eye_transform * mtx;
            m_afw_frame.view[e ^ 1] = other_eye_transform * mtx;
            m_afw_frame.rfc = (int32_t)m_render_frame_count;
            m_afw_frame.valid = true;
            // Frame-stamped ring copy (proj hook filled m_afw_frame.proj earlier this tick):
            // present-time consumers pick THEIR frame's matrices even when the game thread has
            // already recorded the next frame's.
            m_afw_frame_ring[m_afw_frame.rfc & 3] = m_afw_frame;

            // ALTERNATION PROBE: does the RENDER actually alternate eyes frame to frame? Ring of the
            // last 6 applied-transform X offsets + pass parities, dumped as one line periodically.
            static uint32_t s_n = 0;
            static float s_x[6];
            static uint32_t s_p[6];
            s_x[s_n % 6] = current_eye_transform[3].x;
            s_p[s_n % 6] = e;
            ++s_n;
            if (m_flat3d_afw_debug->value() && s_n % 300 == 0) {
                spdlog::info("[Flat3D-AFW] viewhook ring: p=[{},{},{},{},{},{}] x=[{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f}]",
                    s_p[0], s_p[1], s_p[2], s_p[3], s_p[4], s_p[5],
                    s_x[0], s_x[1], s_x[2], s_x[3], s_x[4], s_x[5]);
            }
        }

        // Apply the complete eye transform. This fixes the need for parallel projections on all canted headsets like Pimax
        mtx = current_eye_transform * mtx;
    //}
}

HookManager::PreHookResult VR::pre_set_hdr_mode(std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>& arg_tys, uintptr_t ret_addr) {
    if (!VR::get()->is_hmd_active()) {
        return HookManager::PreHookResult::CALL_ORIGINAL;
    }

    if (args.size() >= 2) {
        args[1] = 0;
    }

    return HookManager::PreHookResult::CALL_ORIGINAL;
}

void VR::inputsystem_update_hook(void* ctx, REManagedObject* input_system) {
    auto original_func = g_input_hook->get_original<decltype(VR::inputsystem_update_hook)>();

    if (!g_framework->is_ready()) {
        original_func(ctx, input_system);
        return;
    }

    auto& mod = VR::get();
    const auto now = std::chrono::steady_clock::now();
    auto is_using_controller = (now - mod->get_last_controller_update()) <= std::chrono::seconds(10);

    if (mod->get_controllers().empty()) {
        // no controllers connected, don't do anything
        original_func(ctx, input_system);
        return;
    }

    auto lstick = sdk::call_object_func<REManagedObject*>(input_system, "get_LStick", sdk::get_thread_context());
    auto rstick = sdk::call_object_func<REManagedObject*>(input_system, "get_RStick", sdk::get_thread_context());

    if (lstick == nullptr || rstick == nullptr) {
        original_func(ctx, input_system);
        return;
    }

    auto button_bits_obj = sdk::call_object_func<REManagedObject*>(input_system, "get_ButtonBits", sdk::get_thread_context(), input_system);

    if (button_bits_obj == nullptr) {
        original_func(ctx, input_system);
        return;
    }

    auto left_axis = mod->get_left_stick_axis();
    auto right_axis = mod->get_right_stick_axis();
    const auto left_axis_len = glm::length(left_axis);
    const auto right_axis_len = glm::length(right_axis);

    // Current actual button bits used by the game
    auto& button_bits_down = *sdk::get_object_field<uint64_t>(button_bits_obj, "Down");
    auto& button_bits_on = *sdk::get_object_field<uint64_t>(button_bits_obj, "On");
    auto& button_bits_up = *sdk::get_object_field<uint64_t>(button_bits_obj, "Up");

    //button_bits_down |= mod->m_button_states_down.to_ullong();
    //button_bits_on |= mod-> m_button_states_on.to_ullong();
    //button_bits_up |= mod->m_button_states_up.to_ullong();

    auto keep_button_down = [&](app::ropeway::InputDefine::Kind button) {
        if ((mod->m_button_states_on.to_ullong() & (uint64_t)button) == 0 && (mod->m_button_states_down.to_ullong() & (uint64_t)button) == 0) {
            return;
        }

        if ((mod->m_button_states_on.to_ullong() & (uint64_t)button) == 0) {
            if (mod->m_button_states_down.to_ullong() & (uint64_t)button) {
                button_bits_on |= (uint64_t)button;
                button_bits_down &= ~(uint64_t)button;
            } else {
                button_bits_down |= (uint64_t)button;
            }
        } else {
            button_bits_down &= ~(uint64_t)button;
            button_bits_on |= (uint64_t)button;
        }
    };

    const auto deadzone = mod->m_joystick_deadzone->value();

    if (left_axis_len > deadzone) {
        mod->m_last_controller_update = now;
        is_using_controller = true;

        // Override the left stick's axis values to the VR controller's values
        Vector3f axis{ left_axis.x, left_axis.y, 0.0f };
        sdk::call_object_func<void*>(lstick, "update", sdk::get_thread_context(), lstick, &axis, &axis);

        keep_button_down(app::ropeway::InputDefine::Kind::UI_L_STICK);
    }

    if (right_axis_len > deadzone) {
        mod->m_last_controller_update = now;
        is_using_controller = true;

        // Override the right stick's axis values to the VR controller's values
        Vector3f axis{ right_axis.x, right_axis.y, 0.0f };
        sdk::call_object_func<void*>(rstick, "update", sdk::get_thread_context(), rstick, &axis, &axis);

        keep_button_down(app::ropeway::InputDefine::Kind::UI_R_STICK);
    }

    // Causes the right stick to take effect properly
    if (is_using_controller) {
        sdk::call_object_func<void*>(input_system, "set_InputMode", sdk::get_thread_context(), input_system, app::ropeway::InputDefine::InputMode::Pad);
    }

    original_func(ctx, input_system);

    mod->openvr_input_to_re2_re3(input_system);
}

bool VR::on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_ctx) {
    // just don't render anything at all.
    // overlays just seem to break stuff in VR.
    if (!is_hmd_active()) {
        vrmod::Flat3DGuiRedirect::get().disarm(); // never leave the GUI redirect armed while inactive
        return true;
    }

    // Flatscreen 3D: never suppress engine overlays; they render fine on a flat screen. The
    // per-eye harvest runs at the POST-overlay hook (on_overlay_layer_draw) so the HUD the overlay
    // draws INTO the main target is captured - copying here (pre-draw) misses it entirely.
    if (is_using_flat3d()) {
        // AFW hudless capture (AFR, single scene layer): clone+copy the overlay's main target BEFORE
        // the GUI draws. Feeds the plugin's ExtractUI so the warp re-composites the UI unwarped
        // (fixes GUI slicing). Reuses the multipass pre_left machinery downstream.
        if (is_using_flat3d_afw() && !is_using_multipass() && g_framework->is_dx12()
                && !TemporalUpscaler::get()->ready()) {
            const auto main_ts = layer->get_main_target_state().get();
            const auto rtv = main_ts != nullptr ? main_ts->get_rtv(0) : nullptr;
            const auto src = rtv != nullptr ? rtv->get_texture_d3d12() : nullptr;

            if (src != nullptr) {
                // Overlay-RT redirect: publish this frame's resources BEFORE recording the marker
                // copy. The pre/post clone copies below/in the post-hook are the in-stream markers
                // the D3D12-level redirect keys on (see Flat3DGuiRedirect).
                if (m_multipass.pre_left_texture.Get() != nullptr
                        && m_multipass.pre_right_texture.Get() != nullptr) {
                    const auto target_native = (ID3D12Resource*)sdk::renderer::get_engine_native_resource_d3d12(src.get());
                    // Same fresh-eye parity D3D12Component uses at present time for this frame.
                    const uint32_t fresh_eye = ((uint32_t)m_render_frame_count % 2 == (uint32_t)m_left_eye_interval) ? 0u : 1u;
                    // World-anchored GUI positions are computed during the game UPDATE phase,
                    // before this frame's counter increment - the content carries the PREVIOUS
                    // frame's eye projection, so it belongs in the OPPOSITE slot (user-validated).
                    const uint32_t capture_eye = fresh_eye ^ 1u;
                    vrmod::Flat3DGuiRedirect::get().arm(target_native,
                        m_multipass.pre_left_texture.Get(), m_multipass.pre_right_texture.Get(), capture_eye);
                } else {
                    vrmod::Flat3DGuiRedirect::get().disarm();
                }

                if (m_multipass.pre_left_copy == nullptr) {
                    m_multipass.pre_left_copy = src->clone();
                    if (m_multipass.pre_left_copy != nullptr) {
                        sdk::renderer::prime_copy_dest_state(m_multipass.pre_left_copy.get());
                        m_multipass.pre_left_texture =
                            (ID3D12Resource*)sdk::renderer::get_engine_native_resource_d3d12(m_multipass.pre_left_copy.get());
                        spdlog::info("[Flat3D-AFW] hudless clone={:x} native={:x}",
                            (uintptr_t)m_multipass.pre_left_copy.get(), (uintptr_t)m_multipass.pre_left_texture.Get());
                    }
                } else {
                    ((sdk::renderer::RenderContext*)render_ctx)->copy_texture(m_multipass.pre_left_copy.get(), src.get());
                }
            }
            return true;
        }

        // Not the AFW path this frame - make sure the GUI redirect can't hijack anything.
        vrmod::Flat3DGuiRedirect::get().disarm();

        return true;
    }

    // Flat3D off entirely: the redirect must never stay armed.
    vrmod::Flat3DGuiRedirect::get().disarm();

    // NOT RE3
    // for some reason RE3 has weird issues with the overlay rendering
    // causing double vision
    {
        const auto& gi = sdk::GameIdentity::get();
        if ((gi.tdb_ver() < 70 && !gi.is_re3()) || (gi.tdb_ver() >= 70 && !gi.is_re3() && !gi.is_re2() && !gi.is_re7() && !gi.is_re4() && !gi.is_sf6())) {
            if (m_allow_engine_overlays->value()) {
                return true;
            }
        }
    }

    return false;
}

void VR::on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_ctx) {
    // Flat3D per-eye harvest, POST-overlay: the Overlay's main target holds this eye's finished
    // scene + HUD (the overlay draw composited the HUD onto it). Copy it into this eye's clone
    // in-stream (engine state-tracked); the clones are tile-backed in D3D12Component so it lands.
    if (!is_using_flat3d() || !g_framework->is_dx12() || TemporalUpscaler::get()->ready()) {
        return;
    }


    const auto scene_layers = m_camera_duplicator.get_relevant_scene_layers();
    const auto parent = (sdk::renderer::layer::Scene*)layer->get_parent();

    // Engine-native UI target lead (see docs/FLAT3D_WILDS_RE.md 2.14): RE9's AFW build reads the
    // engine's own UI buffer instead of capturing the GUI. On Wilds only the ENABLE side is
    // reflected (via.render.layer.Scene::get_UseUIColorAlpha, DLSSUpscalingInterface::
    // set_UseUIColorAlpha) - the texture getters are native-only. Log whether the engine is
    // already producing a UI colour+alpha target: if it ever reads true, hunting the native
    // texture pointer becomes worthwhile and the whole D3D12 redirect could retire.
    if (is_using_flat3d()) {
        static bool s_ui_state_logged = false;
        if (!s_ui_state_logged && parent != nullptr) {
            s_ui_state_logged = true;
            const auto scene_uica = sdk::call_object_func_easy<bool>(parent, "get_UseUIColorAlpha");
            const auto mask_ui = sdk::call_object_func_easy<bool>(layer, "get_UseMaskUITarget");
            spdlog::info("[Flat3D] engine UI targets: Scene.UseUIColorAlpha={} Overlay.UseMaskUITarget={}",
                scene_uica, mask_ui);
        }
    }

    // AFW (AFR + warp): single camera, single scene layer. NO engine copies here - the engine's
    // copy_texture executor CRASHES on Wilds for depth (and cloning is fragile). Instead we just
    // capture the LIVE depth/MV natives; run_flat3d_afw copies them at present time with OUR OWN
    // command list + explicit barriers (frame is complete by then, engine tracker untouched).
    if (is_using_flat3d_afw() && !is_using_multipass() && parent != nullptr) {
        const auto depth_native = parent->get_depth_stencil_d3d12();
        const auto mv_native = parent->get_motion_vectors_d3d12();

        if (depth_native != nullptr && mv_native != nullptr) {
            if (m_afw_depth_tex.Get() != (ID3D12Resource*)depth_native || m_afw_mv_tex.Get() != (ID3D12Resource*)mv_native) {
                m_afw_depth_tex = (ID3D12Resource*)depth_native;
                m_afw_mv_tex = (ID3D12Resource*)mv_native;
                spdlog::info("[Flat3D-AFW] live natives: depth={:x} mv={:x}",
                    (uintptr_t)m_afw_depth_tex.Get(), (uintptr_t)m_afw_mv_tex.Get());
            }

            // Snapshot NOW (mid-frame): at present time the engine may already have cleared the
            // depth (observed as all-zero readbacks -> flat duplicated warp).
            m_d3d12.afw_snapshot_depth_mv(this, (ID3D12Resource*)depth_native, (ID3D12Resource*)mv_native);
        }

        // Same-stage POST-overlay capture: clone the main target right AFTER the GUI drew into it.
        // Paired with the PRE-overlay clone this brackets the GUI draw exactly, so the ExtractUI
        // diff is clean - the old backbuffer-vs-hudless diff spanned post-GUI passes and produced
        // alpha garbage. Reuses the dormant pre_right slots.
        {
            const auto main_ts = layer->get_main_target_state().get();
            const auto rtv = main_ts != nullptr ? main_ts->get_rtv(0) : nullptr;
            const auto src = rtv != nullptr ? rtv->get_texture_d3d12() : nullptr;

            if (src != nullptr) {
                if (m_multipass.pre_right_copy == nullptr) {
                    m_multipass.pre_right_copy = src->clone();
                    if (m_multipass.pre_right_copy != nullptr) {
                        sdk::renderer::prime_copy_dest_state(m_multipass.pre_right_copy.get());
                        m_multipass.pre_right_texture =
                            (ID3D12Resource*)sdk::renderer::get_engine_native_resource_d3d12(m_multipass.pre_right_copy.get());
                        spdlog::info("[Flat3D-AFW] post-overlay clone={:x} native={:x}",
                            (uintptr_t)m_multipass.pre_right_copy.get(), (uintptr_t)m_multipass.pre_right_texture.Get());
                    }
                } else {
                    ((sdk::renderer::RenderContext*)render_ctx)->copy_texture(m_multipass.pre_right_copy.get(), src.get());
                }
            }
        }
        return; // AFR: no multipass per-eye harvest below
    }

    // (flat3d multipass per-eye harvest removed - AFR+AFW is the flat3d path)
}

void VR::on_output_layer_draw(sdk::renderer::layer::Output* layer, void* render_ctx) {
    // (flat3d multipass output harvest removed)
}

bool VR::on_pre_overlay_layer_update(sdk::renderer::layer::Overlay* layer, void* render_ctx) {
    return true;
}

bool VR::on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_ctx) {
    if (!is_hmd_active()) {
        return true;
    }

    auto scene_layer = layer->get_parent();
    const auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return true;
    }
    
    static auto render_output_type = sdk::find_type_definition("via.render.RenderOutput")->get_type();
    auto render_output_component = camera->find(render_output_type);

    if (render_output_component == nullptr) {
        return true;
    }

    if (!m_disable_post_effect_fix) {
        // Set the distortion type back to flatscreen mode
        // this will fix various graphical bugs
        //sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 0); // None

        if (scene_layer != nullptr) {
            m_previous_distortion_type = sdk::call_object_func_easy<uint32_t>(scene_layer, "get_DistortionType");
            m_set_next_post_effect_distortion_type = true;
            sdk::call_object_func_easy<void*>(scene_layer, "set_DistortionType", 0); // None
        }
    }

    return true;
}

void VR::on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_ctx) {
    if (!is_hmd_active()) {
        return;
    }

    if (!m_disable_post_effect_fix && m_set_next_post_effect_distortion_type) {
        auto scene_layer = layer->get_parent();

        // Restore the distortion type back to VR mode
        // to fix TAA
        if (scene_layer != nullptr) {
            sdk::call_object_func_easy<void*>(scene_layer, "set_DistortionType", m_previous_distortion_type); // Left
        }

        //mod->fix_temporal_effects();
        m_set_next_post_effect_distortion_type = false;
    }
}

bool VR::on_pre_post_effect_layer_update(sdk::renderer::layer::PostEffect* layer, void* render_ctx) {
    return true;
}

bool VR::on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_ctx) {
    return true;
}

// Flatscreen 3D multipass harvest via engine-side RTV-swap redirect (the robust path on
// MHWilds, where reading engine targets externally races the GPU -> driver crash). Before
// each eye's PrepareOutput draws, we swap the output target's rtv[0] with our per-eye clone
// so the engine renders that eye INTO our private texture. Because the engine renders into
// it, it's state-tracked; we read it at PRESENT (never mid-frame) so there's no race.
// create_render_target_view resolves on MHWilds; create_target_state does not, so we swap
// the RTV (set_rtv) rather than clone the whole TargetState + set_output_state.
bool VR::on_pre_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) {
    // RTV-swap redirect: clone this eye's output RTV and set_rtv it into the eye's output
    // target so the engine renders the eye into OUR private texture, which we read at
    // present. create_render_target_view now resolves the RTV descriptor-pool device from a
    // render-system global (it's NOT thread-local - see the SDK), so the clone succeeds.
    if (!is_hmd_active() || !is_using_flat3d() || !is_using_multipass()) {
        return true;
    }

    // With the upscaler active it owns the scene-layer redirect and provides the eye
    // textures directly; don't fight it.
    if (TemporalUpscaler::get()->ready()) {
        return true;
    }

    auto scene_layers = m_camera_duplicator.get_relevant_scene_layers();

    if (scene_layers.size() < 2) {
        return true;
    }

    const auto parent_layer = layer->get_parent();

    if (parent_layer == nullptr) {
        return true;
    }

    // The RTV-swap redirect is ABANDONED: create_render_target_view device-removes the GPU
    // when called from this hook (proven by isolation - create_texture alone is safe, adding
    // create_render_target_view crashes). Flat3D instead uses the SAME proven copy-harvest as
    // VR multipass: native_res_copies are created (via create_texture) in on_end_rendering and
    // each eye's prepared color is copied into them, in-stream, by on_prepare_output_layer_draw
    // (context->copy_texture). Nothing to do in this pre-hook anymore.
    (void)parent_layer;
    (void)layer;

    return true;
}

void VR::on_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) {
    if (!is_hmd_active()) {
        return;
    }

    // Flatscreen 3D: keep an engine-side depth clone fresh for the
    // auto-convergence / dynamic-crosshair readback. Uses the engine's own
    // copy command, which handles resource states for us. SKIPPED under AFW: the engine's
    // copy_texture of DEPTH device-removes on Wilds (the original AFW lesson - this path caused a
    // D3D12 reinit loop the moment auto-convergence was first enabled); the sampler gets the
    // DLSS-harvested io depth as its external source instead.
    // PERMANENTLY disabled for flat3d: the engine's copy_texture of depth device-removes on Wilds
    // (this fired the moment AFW was unchecked with auto-convergence on - the !is_using_flat3d_afw
    // gate re-armed it). The depth sampler's ONLY source under flat3d is the DLSS external one.
    if (false && is_using_flat3d()
            && (m_flat3d_auto_convergence->value() || m_flat3d_dynamic_crosshair->value())) {
        const auto scene_layer = (sdk::renderer::layer::Scene*)layer->get_parent();

        if (scene_layer != nullptr) {
            auto is_primary = true;

            if (is_using_multipass()) {
                auto scene_layers = m_camera_duplicator.get_relevant_scene_layers();
                is_primary = !scene_layers.empty() && layer->get_parent() == scene_layers[0];
            }

            if (is_primary) {
                m_flat3d_depth_sampler.update_engine_copy((sdk::renderer::RenderContext*)render_context, scene_layer);
            }
        }
    }

    if (!is_using_multipass()) {
        return;
    }

    // We dont need to do anything here if upscaling is being used
    if (TemporalUpscaler::get()->ready()) {
        return;
    }
    
    auto context = (sdk::renderer::RenderContext*)render_context;
    auto scene_layer = (sdk::renderer::layer::Scene*)layer->get_parent();

    if (scene_layer == nullptr) {
        return;
    }

    // Flatscreen 3D uses the SAME copy-harvest as VR multipass below: copy each eye's
    // prepared color into its native_res_copies clone via the engine's in-stream
    // context->copy_texture (safe - engine state tracking, GPU-timeline ordered, no race).
    // The RTV-swap redirect was abandoned (create_render_target_view crashes the GPU).

    const auto output_state = layer->get_output_state();

    if (output_state == nullptr) {
        return;
    }

    const auto rtv = output_state->get_rtv(0);

    if (rtv == nullptr) {
        return;
    }

    const auto tex = rtv->get_texture_d3d12();

    if (tex == nullptr) {
        return;
    }

    auto scene_layers = m_camera_duplicator.get_relevant_scene_layers();

    if (scene_layers.size() < 2) {
        return;
    }

    const auto parent_layer = layer->get_parent();

    if (parent_layer == nullptr) {
        return;
    }

    // Flat3D harvests at the PRE-OVERLAY hook instead (on_pre_overlay_layer_draw): the shared
    // output target is still BLACK here (the final composite lands between this hook and the
    // overlay), so copying now would capture nothing. Skip the VR harvest below for flat3d.
    if (is_using_flat3d()) {
        return;
    }

    if (parent_layer == scene_layers[0]) {
        if (m_multipass.native_res_copies[0] != nullptr) {
            context->copy_texture(m_multipass.native_res_copies[0], tex);
        }
    } else {
        if (m_multipass.native_res_copies[1] != nullptr) {
            context->copy_texture(m_multipass.native_res_copies[1], tex);
        }
    }
}

bool VR::on_pre_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_ctx) {
    REF_PROFILE_FUNCTION();

    m_scene_update_mtx.lock();
    
    if (!is_hmd_active()) {
        return true;
    }

    if (!layer->is_fully_rendered()) {
        return true;
    }

    if (is_using_multipass()) {
        const auto real_main_camera = sdk::get_primary_camera();
        const auto layer_camera = layer->get_main_camera_if_possible();

        if (layer_camera != nullptr) {
            if (layer_camera == real_main_camera) {
                m_multipass.pass = 0;
                m_multipass_cameras[0] = real_main_camera;
                m_multipass_cameras[1] = m_camera_duplicator.get_new_camera_counterpart(real_main_camera);
            } else if (layer_camera == m_camera_duplicator.get_new_camera_counterpart(real_main_camera)) {
                m_multipass.pass = 1;
                m_multipass_cameras[1] = m_camera_duplicator.get_new_camera_counterpart(real_main_camera);
            } else {
                return true; // dont care
            }
        } else {
            return true; // dont care
        }
    }
    
    auto scene_info = layer->get_scene_info();
    auto depth_distortion_scene_info = layer->get_depth_distortion_scene_info();
    auto filter_scene_info = layer->get_filter_scene_info();
    auto jitter_disable_scene_info = layer->get_jitter_disable_scene_info();
    auto z_prepass_scene_info = layer->get_z_prepass_scene_info();

    auto& layer_data = m_scene_layer_data[layer];

    layer_data[0].setup(scene_info);
    layer_data[1].setup(depth_distortion_scene_info);
    layer_data[2].setup(filter_scene_info);
    layer_data[3].setup(jitter_disable_scene_info);
    layer_data[4].setup(z_prepass_scene_info);

    return true;
}

void VR::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_ctx) {
    REF_PROFILE_FUNCTION();

    utility::ScopeGuard ___([&]() {
        m_scene_update_mtx.unlock();
    });

    if (!is_hmd_active()) {
        return;
    }

    if (!layer->is_fully_rendered()) {
        return;
    }

    if (is_using_multipass()) {
        const auto layer_camera = layer->get_camera();

        if (layer_camera != m_multipass_cameras[0] && layer_camera != m_multipass_cameras[1]) {
            return;
        }
    }

    auto& layer_data = m_scene_layer_data[layer];

    const auto is_temporal_upscaler_active = TemporalUpscaler::get()->ready();
    const auto is_multipass = is_using_multipass();

    /*uint32_t pass = 0;

    if (is_multipass) {
        auto output_layer = sdk::renderer::get_output_layer();

        if (output_layer != nullptr) {
            static auto t = sdk::find_type_definition("via.render.layer.Scene")->get_type();
            const auto scenes = output_layer->find_layers(t);

            if (!scenes.empty()) {
                if (layer == scenes[0]) {
                    pass = 0;
                } else {
                    pass = 1;
                }
            }
        }
    }

    auto proj_mult = glm::identity<Matrix4x4f>();
    proj_mult[2].z = -1.0f;
    proj_mult[3].z = 1.0f;

    const auto projection_matrix = proj_mult * get_projection_matrix(pass);
    const auto current_eye_transform = get_eye_transform(pass + 1);*/

    for (auto& d : layer_data) {
        if (d.scene_info != nullptr) {
            /*if (is_multipass) {
                d.scene_info->view_matrix = current_eye_transform * d.scene_info->view_matrix;
                d.scene_info->projection_matrix = projection_matrix;
                d.scene_info->view_projection_matrix = d.scene_info->projection_matrix * d.scene_info->view_matrix;
                d.scene_info->inverse_view_matrix = glm::inverse(d.scene_info->view_matrix);
                d.scene_info->inverse_projection_matrix = glm::inverse(d.scene_info->projection_matrix);
                d.scene_info->inverse_view_projection_matrix = glm::inverse(d.scene_info->view_projection_matrix);
            }*/

            // Per-eye 2-slot history (feeds the game's DLSS/FSR/TAA the correct
            // previous-eye view-projection so it reprojects the opposite-eye
            // history into this eye instead of using it un-reprojected). This
            // needs the render cadence to ALTERNATE eyes frame-by-frame, which
            // holds for multipass and for Flat3D AFR. Flat3D SEQUENTIAL renders
            // BOTH eyes inside one game frame, so get_game_frame_count() does not
            // distinguish them - post_setup's 2 slots collapse and it feeds TAA a
            // wrong previous matrix, smearing the whole screen on motion. So
            // sequential (and normal VR non-multipass, which submits each eye
            // separately) take the "freeze" path instead.
            const auto flat3d_afr = is_using_flat3d() && is_using_afr();

            if ((is_multipass || flat3d_afr) && !is_temporal_upscaler_active) {
                const auto frame = this->get_game_frame_count();
                d.post_setup(frame);
            } else if (!is_temporal_upscaler_active) {
                // TAA fix / freeze
                d.scene_info->old_view_projection_matrix = d.view_projection_matrix;
            }
        }
    }
}

void VR::wwise_listener_update_hook(void* listener) {
    auto original_func = g_wwise_listener_update_hook->get_original<decltype(VR::wwise_listener_update_hook)>();

    if (!g_framework->is_ready()) {
        original_func(listener);
        return;
    }

    auto& mod = VR::get();

    // Flatscreen 3D: no HMD to orient audio to; leave the listener alone.
    if (!mod->is_hmd_active() || !mod->get_runtime()->loaded || mod->is_using_flat3d()) {
        original_func(listener);
        return;
    }

    if (!mod->m_hmd_oriented_audio->value()) {
        original_func(listener);
        return;
    }

    std::scoped_lock _{mod->m_wwise_mtx};

    const auto& gi = sdk::GameIdentity::get();

    const auto skip_camera_set = (gi.is_re2() || gi.is_re3()) ? FirstPerson::get()->will_be_used() : false;

    if (!skip_camera_set) {
        mod->update_audio_camera();
    }

    const auto CAMERA_OFFSET = gi.tdb_ver() > 49 ? 0x50 : 0x58;

    auto& listener_camera = *(::REManagedObject**)((uintptr_t)listener + CAMERA_OFFSET);
    bool changed = false;

    if (listener_camera == nullptr) {
        auto primary_camera = sdk::get_primary_camera();

        if (primary_camera != nullptr) {
            listener_camera = primary_camera;
            changed = true;
        }
    }

    original_func(listener);

    if (changed) {
        listener_camera = nullptr;
    }

    if (!skip_camera_set) {
        mod->restore_audio_camera();
    }
}

// put it on the backburner
/*
float VR::get_sharpness_hook(void* tonemapping) {
    auto original_func = g_get_sharpness_hook->get_original<decltype(get_sharpness_hook)>();
    
    if (!g_framework->is_ready()) {
        return original_func(tonemapping);
    }

    auto& mod = VR::get();

    if (mod->m_disable_sharpening) {
        return 0.0f;
    }

    return original_func(tonemapping);
}
*/

// Called when the mod is initialized
std::optional<std::string> VR::on_initialize_d3d_thread() try {
    auto openvr_error = initialize_openvr();

    if (openvr_error || !m_openvr->loaded) {
        if (m_openvr->error) {
            spdlog::info("OpenVR failed to load: {}", *m_openvr->error);
        }

        m_openvr->is_hmd_active = false;
        m_openvr->was_hmd_active = false;
        m_openvr->needs_pose_update = false;

        // Attempt to load OpenXR instead
        auto openxr_error = initialize_openxr();

        if (openxr_error || !m_openxr->loaded) {
            m_openxr->needs_pose_update = false;
        }
    } else {
        m_openxr->error =
R"(OpenVR loaded first.
If you want to use OpenXR, remove the openvr_api.dll from your game folder,
and place the openxr_loader.dll in the same folder.)";
    }

    // Flatscreen 3D: no HMD runtime required. Always active when no real VR
    // runtime is present (the enable option is retired - this build IS the
    // flatscreen-3D build).
    if (!m_openvr->loaded && !m_openxr->loaded) {
        initialize_flat3d();
    }

    if (!get_runtime()->loaded) {
        // this is okay. we're not going to fail the whole thing entirely
        // so we're just going to return OK, but
        // when the VR mod draws its menu, it'll say "VR is not available"
        return Mod::on_initialize();
    }

    // Check whether the user has Hardware accelerated GPU scheduling enabled
    const auto hw_schedule_value = utility::get_registry_dword(
        HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
        "HwSchMode");

    if (hw_schedule_value) {
        m_has_hw_scheduling = *hw_schedule_value == 2;
    }

    auto hijack_error = hijack_resolution();

    if (hijack_error) {
        return hijack_error;
    }

    hijack_error = hijack_input();

    if (hijack_error) {
        return hijack_error;
    }

    hijack_error = hijack_camera();

    if (hijack_error) {
        return hijack_error;
    }

    hijack_error = hijack_wwise_listeners();

    if (hijack_error) {
        return hijack_error;
    }

    const auto renderer_t = sdk::find_type_definition("via.render.Renderer");

    if (renderer_t != nullptr) {
        const auto set_hdr_method = renderer_t->get_method("set_HDRMode");

        if (set_hdr_method != nullptr) {
            spdlog::info("Hooking setHDRMode");
            g_hookman.add(set_hdr_method, &pre_set_hdr_mode, &post_set_hdr_mode);
        }
    }

    m_init_finished = true;

    // all OK
    return Mod::on_initialize();
} catch(...) {
    spdlog::error("Exception occurred in VR::on_initialize()");

    m_runtime->error = "Exception occurred in VR::on_initialize()";
    m_openxr->dll_missing = false;
    m_openvr->dll_missing = false;
    m_openxr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->loaded = false;
    m_openvr->is_hmd_active = false;
    m_openxr->loaded = false;
    m_flat3d->loaded = false;
    m_init_finished = false;

    return Mod::on_initialize();
}

void VR::on_lua_state_created(sol::state& lua) {
    lua.new_usertype<VR>("VR",
        "get_controllers", &VR::get_controllers,
        "get_position", &VR::get_position,
        "get_velocity", &VR::get_velocity,
        "get_angular_velocity", &VR::get_angular_velocity,
        "get_rotation", &VR::get_rotation,
        "get_transform", &VR::get_transform,
        "get_left_stick_axis", &VR::get_left_stick_axis,
        "get_right_stick_axis", &VR::get_right_stick_axis,
        "get_current_eye_transform", &VR::get_current_eye_transform,
        "get_current_projection_matrix", &VR::get_current_projection_matrix,
        "get_standing_origin", &VR::get_standing_origin,
        "set_standing_origin", &VR::set_standing_origin,
        "get_rotation_offset", &VR::get_rotation_offset,
        "set_rotation_offset", &VR::set_rotation_offset,
        "recenter_view", &VR::recenter_view,
        "get_gui_rotation_offset", &VR::get_gui_rotation_offset,
        "set_gui_rotation_offset", &VR::set_gui_rotation_offset,
        "recenter_gui", &VR::recenter_gui,
        "get_action_set", &VR::get_action_set,
        "get_active_action_set", &VR::get_active_action_set,
        "get_action_trigger", &VR::get_action_trigger,
        "get_action_grip", &VR::get_action_grip,
        "get_action_joystick", &VR::get_action_joystick,
        "get_action_joystick_click", &VR::get_action_joystick_click,
        "get_action_a_button", &VR::get_action_a_button,
        "get_action_b_button", &VR::get_action_b_button,
        "get_action_weapon_dial", &VR::get_action_weapon_dial,
        "get_action_minimap", &VR::get_action_minimap,
        "get_action_block", &VR::get_action_block,
        "get_action_dpad_up", &VR::get_action_dpad_up,
        "get_action_dpad_down", &VR::get_action_dpad_down,
        "get_action_dpad_left", &VR::get_action_dpad_left,
        "get_action_dpad_right", &VR::get_action_dpad_right,
        "get_action_heal", &VR::get_action_heal,
        "get_left_joystick", &VR::get_left_joystick,
        "get_right_joystick", &VR::get_right_joystick,
        "is_using_controllers", &VR::is_using_controllers,
        "is_openvr_loaded", &VR::is_openvr_loaded,
        "is_openxr_loaded", &VR::is_openxr_loaded,
        "is_hmd_active", &VR::is_hmd_active,
        "is_action_active", &VR::is_action_active,
        "is_using_hmd_oriented_audio", &VR::is_using_hmd_oriented_audio,
        "toggle_hmd_oriented_audio", &VR::toggle_hmd_oriented_audio,
        "apply_hmd_transform", [](VR* vr, glm::quat& rotation, Vector4f& position) {
            vr->apply_hmd_transform(rotation, position);
        },
        "trigger_haptic_vibration", &VR::trigger_haptic_vibration,
        "get_last_render_matrix", &VR::get_last_render_matrix,
        "should_handle_pause", [](VR* vr) { 
            return vr->get_runtime()->handle_pause;
        },
        "set_handle_pause", [](VR* vr, bool state) { 
            return vr->get_runtime()->handle_pause = state;
        },
        "unhide_crosshair", &VR::unhide_crosshair
    );

    lua["vrmod"] = this;
}

// One-shot reflection probe for the engine's NATIVE UI-color-alpha / hudless render targets.
// PureDark's RE9 AFW build reads the engine's own UI buffer straight off the overlay layer
// (reflection field "UIBufferTexturePtr") instead of capturing the GUI. Wilds' reflection DB
// contains the equivalent family (setUseUIColorAlpha / get_UIColorAlphaTexPtr /
// getDisplayUIColorAlphaTexPtr / get_HudlessTexPtr / getGUIBufferUITarget ...), which - if it is
// reachable and can be switched on - would replace BOTH the D3D12 overlay-RT redirect and the
// pre/post hudless clones with plain reflection reads. This logs every type exposing those
// members so we can find the owner; it runs once and costs nothing afterwards.
void VR::flat3d_probe_engine_ui_targets() {
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;

    const auto tdb = sdk::RETypeDB::get();
    if (tdb == nullptr) {
        spdlog::warn("[Flat3D-UIProbe] no TDB");
        return;
    }

    static constexpr std::string_view k_keys[]{
        "UIColorAlpha", "Hudless", "GUIBufferUITarget", "UITarget", "UIBufferTexture"};

    const auto matches = [](const char* name) {
        if (name == nullptr) {
            return false;
        }
        const std::string_view sv{name};
        for (const auto& k : k_keys) {
            if (sv.find(k) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    };

    uint32_t hits = 0;
    const auto num_types = tdb->get_num_types();

    for (uint32_t i = 0; i < num_types; ++i) {
        auto* t = tdb->get_type(i);
        if (t == nullptr) {
            continue;
        }

        std::string type_name{};

        for (auto& m : t->get_methods()) {
            const auto name = m.get_name();
            if (!matches(name)) {
                continue;
            }
            if (type_name.empty()) {
                type_name = t->get_full_name();
            }
            const auto ret = m.get_return_type();
            spdlog::info("[Flat3D-UIProbe] METHOD {}::{}() -> {} (params={})", type_name, name,
                ret != nullptr ? ret->get_full_name() : "?", m.get_num_params());
            ++hits;
        }

        for (auto* f : t->get_fields()) {
            if (f == nullptr) {
                continue;
            }
            const auto name = f->get_name();
            if (!matches(name)) {
                continue;
            }
            if (type_name.empty()) {
                type_name = t->get_full_name();
            }
            const auto ft = f->get_type();
            spdlog::info("[Flat3D-UIProbe] FIELD  {}::{} : {}", type_name, name,
                ft != nullptr ? ft->get_full_name() : "?");
            ++hits;
        }
    }

    spdlog::info("[Flat3D-UIProbe] scan complete: {} member(s) across {} types", hits, num_types);
}

std::optional<std::string> VR::initialize_flat3d() {
    spdlog::info("[VR] Initializing Flatscreen 3D output (no HMD)");

    if (m_flat3d_afw_debug->value()) {
        flat3d_probe_engine_ui_targets(); // ~321k types; opt-in (see docs/FLAT3D_WILDS_RE.md 2.14)
    }

    m_flat3d = std::make_shared<runtimes::Flat3D>();
    m_flat3d->loaded = true;
    m_flat3d->enabled = true; // stereo always active (the Active toggle is retired)
    m_runtime = m_flat3d;

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    return Mod::on_initialize();
}

void VR::update_flat3d_params() {
    if (m_flat3d == nullptr) {
        return;
    }

    // Publish the native-output override to the swapchain hook (ResizeBuffers substitution).
    // Always on under flat3d (the option is retired).
    D3D12Hook::s_force_native_resolution.store(is_using_flat3d());

    // FoV-aware separation auto-scaling (always on): screen disparity scales
    // with sep / tan(half_fov), so scaling the Depth setting by
    // tan(half_fov_now) / tan(half_fov_reference) keeps perceived depth
    // constant when the game zooms (aim, cutscenes).
    const auto p11 = m_flat3d_game_p11.load();
    auto fov_scale = 1.0f;

    if (p11 > 0.0f) {
        const auto tan_half_game = 1.0f / p11;
        const auto tan_half_ref = std::tan(glm::radians(m_flat3d_reference_fov->value()) * 0.5f);

        if (tan_half_ref > 0.0f && tan_half_game > 0.0f) {
            fov_scale = tan_half_game / tan_half_ref;
        }
    }

    // EMA smooths hard FoV cuts (cutscene transitions, weapon zoom pops).
    m_flat3d_fov_scale_ema += (fov_scale - m_flat3d_fov_scale_ema) * 0.2f;

    auto sep_eff = m_flat3d_depth->value() * m_flat3d_fov_scale_ema;
    const auto manual_conv = std::max(m_flat3d_convergence->value(), 0.01f); // a distance: no FoV term
    auto conv_applied = manual_conv;

    // Auto-convergence (dynamic3d 3.3-3.5): pull convergence in so the nearest
    // significant object stays under the pop-out budget, while co-scaling
    // separation so BACKGROUND disparity stays locked at the user's calibration.
    const auto z_near_raw = m_flat3d_auto_convergence->value() ? m_flat3d_depth_sampler.get_nearest_depth() : -1.0f;

    if (z_near_raw > 0.0f) {
        // Temporal median-of-5 prefilter, then asymmetric EMA with a RELATIVE deadband.
        m_flat3d_znear_history[m_flat3d_znear_history_idx] = z_near_raw;
        m_flat3d_znear_history_idx = (m_flat3d_znear_history_idx + 1) % (uint32_t)m_flat3d_znear_history.size();
        m_flat3d_znear_history_count = std::min<uint32_t>(m_flat3d_znear_history_count + 1, (uint32_t)m_flat3d_znear_history.size());

        auto sorted = m_flat3d_znear_history;
        std::sort(sorted.begin(), sorted.begin() + m_flat3d_znear_history_count);
        const auto z_median = sorted[m_flat3d_znear_history_count / 2];

        if (m_flat3d_znear_ema <= 0.0f) {
            m_flat3d_znear_ema = z_median;
        } else if (std::abs(z_median - m_flat3d_znear_ema) / m_flat3d_znear_ema > 0.005f) {
            // Fast toward the camera (comfort), slow away (stability).
            const auto alpha = z_median < m_flat3d_znear_ema ? 0.20f : 0.05f;
            m_flat3d_znear_ema += (z_median - m_flat3d_znear_ema) * alpha;
        }

        const auto p00 = m_flat3d_game_p00.load();
        const auto k = sep_eff * p00 * 0.25f; // per-eye shift as a fraction of eye width

        if (k > 0.0f && m_flat3d_znear_ema > 0.0f) {
            const auto target = m_flat3d_max_popout->value() * 0.01f;
            const auto d_at_manual = k * (1.0f / m_flat3d_znear_ema - 1.0f / manual_conv);

            auto conv_target = manual_conv; // manual value is a CEILING - auto only pulls IN

            if (d_at_manual > target) {
                conv_target = m_flat3d_znear_ema * (1.0f + target * manual_conv / k);
                conv_target = std::min(conv_target, manual_conv);
            }

            conv_target = std::max(conv_target, std::max(m_nearz * 1.5f, 0.01f));

            // Smooth in 1/convergence space - disparity is linear in 1/conv, so
            // smoothing conv itself would "lunge" as it gets small.
            const auto target_inv = 1.0f / conv_target;

            if (m_flat3d_inv_conv_ema <= 0.0f) {
                m_flat3d_inv_conv_ema = target_inv;
            } else {
                m_flat3d_inv_conv_ema += (target_inv - m_flat3d_inv_conv_ema) * m_flat3d_autoconv_smoothing->value();
            }

            conv_applied = std::min(1.0f / m_flat3d_inv_conv_ema, manual_conv);

            // Lock background disparity: sep_eff/conv_applied == sep/manual_conv.
            const auto depth_scale = std::clamp(conv_applied / manual_conv, 0.1f, 1.0f);
            sep_eff *= depth_scale;
        }
    } else if (m_flat3d_inv_conv_ema > 0.0f || m_flat3d_znear_ema > 0.0f) {
        // Disabled or no depth data: snap back to manual instantly and reset
        // all internal state (easing out reads as unexplained drift).
        m_flat3d_inv_conv_ema = -1.0f;
        m_flat3d_znear_ema = -1.0f;
        m_flat3d_znear_history_count = 0;
        m_flat3d_znear_history_idx = 0;
    }

    m_flat3d->separation_eff = sep_eff;
    m_flat3d->convergence = conv_applied;
    m_flat3d->shear_dir_left = m_flat3d_swap_shear_sign->value() ? -1.0f : 1.0f;
}

std::optional<std::string> VR::initialize_openvr() {
    m_openvr = std::make_shared<runtimes::OpenVR>();
    m_openvr->loaded = false;

    if (utility::load_module_from_current_directory(L"openvr_api.dll") == nullptr) {
        spdlog::info("[VR] Could not load openvr_api.dll");

        m_openvr->dll_missing = true;
        m_openvr->error = "Could not load openvr_api.dll";
        return Mod::on_initialize();
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openvr->needs_pose_update = true;
    m_openvr->got_first_poses = false;
    m_openvr->is_hmd_active = true;
    m_openvr->was_hmd_active = true;

    auto error = vr::VRInitError_None;
	m_openvr->hmd = vr::VR_Init(&error, vr::VRApplication_Scene);

    // check if error
    if (error != vr::VRInitError_None) {
        m_openvr->error = "VR_Init failed: " + std::string{vr::VR_GetVRInitErrorAsEnglishDescription(error)};
        return Mod::on_initialize();
    }

    if (m_openvr->hmd == nullptr) {
        m_openvr->error = "VR_Init failed: HMD is null";
        return Mod::on_initialize();
    }

    // get render target size
    m_openvr->update_render_target_size();

    if (vr::VRCompositor() == nullptr) {
        m_openvr->error = "VRCompositor failed to initialize.";
        return Mod::on_initialize();
    }

    auto input_error = initialize_openvr_input();

    if (input_error) {
        m_openvr->error = *input_error;
        return Mod::on_initialize();
    }

    auto overlay_error = m_overlay_component.on_initialize_openvr();

    if (overlay_error) {
        m_openvr->error = *overlay_error;
        return Mod::on_initialize();
    }
    
    m_openvr->loaded = true;
    m_openvr->error = std::nullopt;
    m_runtime = m_openvr;

    return Mod::on_initialize();
}

std::optional<std::string> VR::initialize_openvr_input() {
    const auto module_directory = REFramework::get_persistent_dir();

    // write default actions and bindings with the static strings we have
    for (auto& it : m_binding_files) {
        spdlog::info("Writing default binding file {}", it.first);

        std::ofstream file{ module_directory / it.first };
        file << it.second;
    }

    const auto actions_path = module_directory / "actions.json";
    auto input_error = vr::VRInput()->SetActionManifestPath(actions_path.string().c_str());

    if (input_error != vr::VRInputError_None) {
        return "VRInput failed to set action manifest path: " + std::to_string((uint32_t)input_error);
    }

    // get action set
    auto action_set_error = vr::VRInput()->GetActionSetHandle("/actions/default", &m_action_set);

    if (action_set_error != vr::VRInputError_None) {
        return "VRInput failed to get action set: " + std::to_string((uint32_t)action_set_error);
    }

    if (m_action_set == vr::k_ulInvalidActionSetHandle) {
        return "VRInput failed to get action set handle.";
    }

    for (auto& it : m_action_handles) {
        auto error = vr::VRInput()->GetActionHandle(it.first.c_str(), &it.second.get());

        if (error != vr::VRInputError_None) {
            return "VRInput failed to get action handle: (" + it.first + "): " + std::to_string((uint32_t)error);
        }

        if (it.second == vr::k_ulInvalidActionHandle) {
            return "VRInput failed to get action handle: (" + it.first + ")";
        }
    }

    m_active_action_set.ulActionSet = m_action_set;
    m_active_action_set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    m_active_action_set.nPriority = 0;

    detect_controllers();

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr() {
    m_openxr = std::make_shared<runtimes::OpenXR>();

    spdlog::info("[VR] Initializing OpenXR");

    if (utility::load_module_from_current_directory(L"openxr_loader.dll") == nullptr) {
        spdlog::info("[VR] Could not load openxr_loader.dll");

        m_openxr->loaded = false;
        m_openxr->error = "Could not load openxr_loader.dll";

        return std::nullopt;
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openxr->needs_pose_update = true;
    m_openxr->got_first_poses = false;

    // Step 1: Create an instance
    spdlog::info("[VR] Creating OpenXR instance");

    XrResult result{XR_SUCCESS};

    // We may just be restarting OpenXR, so try to find an existing instance first
    if (m_openxr->instance == XR_NULL_HANDLE) {
        std::vector<const char*> extensions{};

        if (g_framework->is_dx12()) {
            extensions.push_back(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        } else {
            extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        }

        XrInstanceCreateInfo instance_create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        instance_create_info.next = nullptr;
        instance_create_info.enabledExtensionCount = (uint32_t)extensions.size();
        instance_create_info.enabledExtensionNames = extensions.data();

        strcpy(instance_create_info.applicationInfo.applicationName, g_framework->get_game_name());
        instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        
        result = xrCreateInstance(&instance_create_info, &m_openxr->instance);

        // we can't convert the result to a string here
        // because the function requires the instance to be valid
        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr instance: " + std::to_string((int32_t)result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr instance");
    }
    
    // Step 2: Create a system
    spdlog::info("[VR] Creating OpenXR system");

    // We may just be restarting OpenXR, so try to find an existing system first
    if (m_openxr->system == XR_NULL_SYSTEM_ID) {
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = m_openxr->form_factor;

        result = xrGetSystem(m_openxr->instance, &system_info, &m_openxr->system);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr system: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr system");
    }

    // Step 3: Create a session
    spdlog::info("[VR] Initializing graphics info");

    XrSessionCreateInfo session_create_info{XR_TYPE_SESSION_CREATE_INFO};

    if (g_framework->is_dx12()) {
        m_d3d12.openxr().initialize(session_create_info);
    } else {
        m_d3d11.openxr().initialize(session_create_info);
    }

    spdlog::info("[VR] Creating OpenXR session");
    session_create_info.systemId = m_openxr->system;
    result = xrCreateSession(m_openxr->instance, &session_create_info, &m_openxr->session);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not create openxr session: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    // Step 4: Create a space
    spdlog::info("[VR] Creating OpenXR space");

    // We may just be restarting OpenXR, so try to find an existing space first

    if (m_openxr->stage_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->stage_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr stage space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    if (m_openxr->view_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->view_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr view space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    // Step 5: Get the system properties
    spdlog::info("[VR] Getting OpenXR system properties");

    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    result = xrGetSystemProperties(m_openxr->instance, m_openxr->system, &system_properties);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not get system properties: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    spdlog::info("[VR] OpenXR system Name: {}", system_properties.systemName);
    spdlog::info("[VR] OpenXR system Vendor: {}", system_properties.vendorId);
    spdlog::info("[VR] OpenXR system max width: {}", system_properties.graphicsProperties.maxSwapchainImageWidth);
    spdlog::info("[VR] OpenXR system max height: {}", system_properties.graphicsProperties.maxSwapchainImageHeight);
    spdlog::info("[VR] OpenXR system supports {} layers", system_properties.graphicsProperties.maxLayerCount);
    spdlog::info("[VR] OpenXR system orientation: {}", system_properties.trackingProperties.orientationTracking);
    spdlog::info("[VR] OpenXR system position: {}", system_properties.trackingProperties.positionTracking);

    // Step 6: Get the view configuration properties
    m_openxr->update_render_target_size();

    // Step 7: Create a view
    if (!m_openxr->view_configs.empty()){
        m_openxr->views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
        m_openxr->stage_views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
    }

    if (m_openxr->view_configs.empty()) {
        m_openxr->error = "No view configurations found";
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    m_openxr->loaded = true;
    m_runtime = m_openxr;

    if (auto err = initialize_openxr_input()) {
        m_openxr->error = err.value();
        m_openxr->loaded = false;
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    detect_controllers();

    if (m_init_finished) {
        // This is usually done in on_config_load
        // but the runtime can be reinitialized, so we do it here instead
        initialize_openxr_swapchains();
    }

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_input() {
    if (auto err = m_openxr->initialize_actions(VR::actions_json)) {
        m_openxr->error = err.value();
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }
    
    for (auto& it : m_action_handles) {
        auto openxr_action_name = m_openxr->translate_openvr_action_name(it.first);

        if (m_openxr->action_set.action_map.contains(openxr_action_name)) {
            it.second.get() = (decltype(it.second)::type)m_openxr->action_set.action_map[openxr_action_name];
            spdlog::info("[VR] Successfully mapped action {} to {}", it.first, openxr_action_name);
        }
    }

    m_left_joystick = (decltype(m_left_joystick))VRRuntime::Hand::LEFT;
    m_right_joystick = (decltype(m_right_joystick))VRRuntime::Hand::RIGHT;

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_swapchains() {
    // This depends on the config being loaded.
    if (!m_init_finished) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating OpenXR swapchain");

    if (g_framework->is_dx12()) {
        auto err = m_d3d12.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());

            return m_openxr->error;
        }
    } else {
        auto err = m_d3d11.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());
            return m_openxr->error;
        }
    }

    return std::nullopt;
}

std::optional<std::string> VR::hijack_resolution() {
    // moved to global hook class
    return std::nullopt;
}

std::optional<std::string> VR::hijack_input() {
    if (sdk::GameIdentity::get().is_re2() || sdk::GameIdentity::get().is_re3()) {
        spdlog::info("[VR] Hijacking InputSystem");

        // We're going to hook InputSystem.update so we can
        // override the analog stick values with the VR controller's
        auto func = sdk::find_native_method(game_namespace("InputSystem"), "update");

        if (func == nullptr) {
            return "VR init failed: InputSystem.update function not found.";
        }

        spdlog::info("InputSystem.update: {:x}", (uintptr_t)func);

        // Hook the native function
        g_input_hook = std::make_unique<FunctionHook>(func, inputsystem_update_hook);

        if (!g_input_hook->create()) {
            return "VR init failed: InputSystem.update native function hook failed.";
        }
    }

    return std::nullopt;
}

std::optional<std::string> VR::hijack_camera() {
    spdlog::info("[VR] Hijacking Camera");

    const auto get_projection_matrix = (uintptr_t)sdk::find_native_method("via.Camera", "get_ProjectionMatrix");

    ///////////////////////////////
    // Hook GUI camera projection matrix start
    ///////////////////////////////
    auto func = sdk::find_native_method("via.gui.GUICamera", "get_ProjectionMatrix");

    if (func != nullptr) {
        spdlog::info("via.gui.GUICamera.get_ProjectionMatrix: {:x}", (uintptr_t)func);

        // Pattern scan for the native function call - same fallback set as
        // Hooks::hook_camera_get_projection_matrix (the old single 49 8B C8 pattern doesn't exist
        // in newer TDBs; Wilds matches the 48 89 F2 variant).
        auto ref = utility::find_pattern_in_path((uint8_t*)func, 1000, false, "49 8B C8 E8");

        if (!ref) {
            ref = utility::find_pattern_in_path((uint8_t*)func, 1000, false, "48 8B CB E8");
        }

        if (!ref) {
            ref = utility::find_pattern_in_path((uint8_t*)func, 1000, false, "48 89 F2 E8"); // >= TDB74
        }

        if (ref) {
            auto native_func = utility::calculate_absolute(ref->addr + 4);

            if (native_func != get_projection_matrix) {
                // Hook the native function
                g_projection_matrix_hook2 = std::make_unique<FunctionHook>(native_func, gui_camera_get_projection_matrix_hook);

                if (g_projection_matrix_hook2->create()) {
                    spdlog::info("Hooked via.gui.GUICamera.get_ProjectionMatrix");
                }
            } else {
                spdlog::info("Did not hook via.gui.GUICamera.get_ProjectionMatrix, same as via.Camera.get_ProjectionMatrix");
            }
        } else {
            spdlog::info("via.gui.GUICamera.get_ProjectionMatrix native call pattern not found - GUI safe-area unavailable");
        }
    }

    return std::nullopt;
}

std::optional<std::string> VR::hijack_wwise_listeners() {
    const auto& gi = sdk::GameIdentity::get();
    if (!gi.is_re4() && !gi.is_sf6() && gi.tdb_ver() < 73) {
        spdlog::info("[VR] Hijacking WwiseListener");

        const auto t = sdk::find_type_definition("via.wwise.WwiseListener");

        if (t == nullptr) {
            return "VR init failed: via.wwise.WwiseListener type not found.";
        }

        const auto update_method = t->get_method("update");

        if (update_method == nullptr) {
            return "VR init failed: via.wwise.WwiseListener.update method not found.";
        }

        const auto func_wrapper = update_method->get_function();

        if (func_wrapper == nullptr) {
            return "VR init failed: via.wwise.WwiseListener.update native function not found.";
        }
    
        spdlog::info("via.wwise.WwiseListener.update: {:x}", (uintptr_t)func_wrapper);
    
        // Use hde to disassemble the method and find the first jmp, which jmps to the real function
        // in the vtable
        const auto jmp = utility::scan_disasm((uintptr_t)func_wrapper, 10, "48 FF");

        if (!jmp) {
            return "VR init failed: could not find jmp opcode in via.wwise.WwiseListener.update native function.";
        }

        const auto vtable_index = *(uint8_t*)(*jmp + 3) / sizeof(void*);
        spdlog::info("via.wwise.WwiseListener.update vtable index: {}", vtable_index);
        spdlog::info("Attempting to create fake via.wwise.WwiseListener instance");

        const void* fake_obj = t->create_instance_full();

        if (fake_obj == nullptr) {
            return "VR init failed: Failed to create fake via.wwise.WwiseListener instance.";
        }
    
        spdlog::info("Attempting to read vtable from fake via.wwise.WwiseListener instance");
        auto obj_vtable = *(void***)fake_obj;

        if (obj_vtable == nullptr) {
            return "VR init failed: via.wwise.WwiseListener vtable not found.";
        }

        spdlog::info("via.wwise.WwiseListener vtable: {:x}", (uintptr_t)obj_vtable - g_framework->get_module());

        auto update_native = obj_vtable[vtable_index];

        if (update_native == 0) {
            return "VR init failed: via.wwise.WwiseListener update native not found.";
        }

        spdlog::info("via.wwise.WwiseListener.update: {:x}", (uintptr_t)update_native);

        g_wwise_listener_update_hook = std::make_unique<FunctionHook>(update_native, wwise_listener_update_hook);

        if (!g_wwise_listener_update_hook->create()) {
            return "VR init failed: via.wwise.WwiseListener update native function hook failed.";
        }
    }

    return std::nullopt;
}

bool VR::detect_controllers() {
    // already detected
    if (!m_controllers.empty()) {
        return true;
    }

    if (get_runtime()->is_openvr()) {
        auto left_joystick_origin_error = vr::EVRInputError::VRInputError_None;
        auto right_joystick_origin_error = vr::EVRInputError::VRInputError_None;

        vr::InputOriginInfo_t left_joystick_origin_info{};
        vr::InputOriginInfo_t right_joystick_origin_info{};

        // Get input origin info for the joysticks
        // get the source input device handles for the joysticks
        auto left_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/left", &m_left_joystick);

        if (left_joystick_error != vr::VRInputError_None) {
            return false;
        }

        auto right_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/right", &m_right_joystick);

        if (right_joystick_error != vr::VRInputError_None) {
            return false;
        }

        left_joystick_origin_info = {};
        right_joystick_origin_info = {};

        left_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_left_joystick, &left_joystick_origin_info, sizeof(left_joystick_origin_info));
        right_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_right_joystick, &right_joystick_origin_info, sizeof(right_joystick_origin_info));
        if (left_joystick_origin_error != vr::EVRInputError::VRInputError_None || right_joystick_origin_error != vr::EVRInputError::VRInputError_None) {
            return false;
        }

        // Instead of manually going through the devices,
        // We do this. The order of the devices isn't always guaranteed to be
        // Left, and then right. Using the input state handles will always
        // Get us the correct device indices.
        m_controllers.push_back(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers.push_back(right_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(right_joystick_origin_info.trackedDeviceIndex);

        spdlog::info("Left Hand: {}", left_joystick_origin_info.trackedDeviceIndex);
        spdlog::info("Right Hand: {}", right_joystick_origin_info.trackedDeviceIndex);
    } else if (get_runtime()->is_openxr()) {
        // ezpz
        m_controllers.push_back(1);
        m_controllers.push_back(2);
        m_controllers_set.insert(1);
        m_controllers_set.insert(2);

        spdlog::info("Left Hand: {}", 1);
        spdlog::info("Right Hand: {}", 2);
    }


    return true;
}

bool VR::is_any_action_down() {
    if (!m_runtime->ready() || !is_using_controllers()) {
        return false;
    }

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();

    if (glm::length(left_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    if (glm::length(right_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    for (auto& it : m_action_handles) {
        if (is_action_active(it.second, m_left_joystick) || is_action_active(it.second, m_right_joystick)) {
            return true;
        }
    }

    return false;
}

void VR::update_hmd_state() {
    REF_PROFILE_FUNCTION();

    auto runtime = get_runtime();
    
    if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::EARLY) {
        if (runtime->is_openxr()) {
            if (g_framework->get_renderer_type() == REFramework::RendererType::D3D11) {
                if (!runtime->got_first_sync || runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                    return;
                }  
            } else if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                return;
            }

            m_openxr->begin_frame();
        } else {
            if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                return;
            }
        }
    }
    
    runtime->update_poses();

    // Update the poses used for the game
    // If we used the data directly from the WaitGetPoses call, we would have to lock a different mutex and wait a long time
    // This is because the WaitGetPoses call is blocking, and we don't want to block any game logic
    if (runtime->wants_reset_origin && runtime->ready() && runtime->got_first_valid_poses) {
        std::unique_lock _{ runtime->pose_mtx };
        set_rotation_offset(glm::identity<glm::quat>());
        m_standing_origin = get_position_unsafe(vr::k_unTrackedDeviceIndex_Hmd);

        runtime->wants_reset_origin = false;
    }

    if (runtime->is_flat3d()) {
        update_flat3d_params();
    }

    runtime->update_matrices(m_nearz, m_farz);

    runtime->got_first_poses = true;

    // Forcefully update the camera transform after submitting the frame
    // because the game logic thread does not run in sync with the rendering thread
    // This will massively improve HMD rotation smoothness for the user
    // if this is not done, the left eye will jitter a lot
    if (sdk::GameIdentity::get().is_re2() || sdk::GameIdentity::get().is_re3()) {
        const auto cameras = get_cameras();

        for (auto camera : cameras) {
            if (camera == nullptr) {
                continue;
            }

            if (camera->get_game_object() != nullptr && camera->get_game_object()->get_transform() != nullptr) {
                if (camera == cameras[0]) {
                    FirstPerson::get()->on_update_transform(camera->get_game_object()->get_transform());
                } else if (cameras[0] != nullptr && cameras[0]->get_game_object() != nullptr && cameras[0]->get_game_object()->get_transform() != nullptr) {
                    auto transform0 = cameras[0]->get_game_object()->get_transform();
                    auto transform1 = camera->get_game_object()->get_transform();

                    const auto camera_joint = utility::re_transform::get_joint(*transform1, 0);

                    if (camera_joint == nullptr) {
                        continue;
                    }

                    auto& fp = FirstPerson::get();

                    const auto mat = fp->get_last_camera_matrix();
                    const auto pos = mat[3];

                    transform1->get_world_transform() = transform0->get_world_transform();

                    sdk::set_joint_position(camera_joint, pos);
                    sdk::set_joint_rotation(camera_joint, mat);
                }
            }
        }
    }
}

void VR::update_action_states() {
    REF_PROFILE_FUNCTION();

    auto runtime = get_runtime();

    if (runtime->wants_reinitialize) {
        return;
    }

    if (runtime->is_openvr()) {
        const auto start_time = std::chrono::high_resolution_clock::now();

        auto error = vr::VRInput()->UpdateActionState(&m_active_action_set, sizeof(m_active_action_set), 1);

        if (error != vr::VRInputError_None) {
            spdlog::error("VRInput failed to update action state: {}", (uint32_t)error);
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto time_delta = end_time - start_time;

        m_last_input_delay = time_delta;
        m_avg_input_delay = (m_avg_input_delay + time_delta) / 2;

        if ((end_time - start_time) >= std::chrono::milliseconds(30)) {
            spdlog::warn("VRInput update action state took too long: {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

            //reinitialize_openvr();
            runtime->wants_reinitialize = true;
        }   
    } else {
        get_runtime()->update_input();
    }

    if (m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }

    if (m_set_standing_key->is_key_down_once()) {
        set_standing_origin(get_position(0));
    }
}

void VR::update_camera() {
    REF_PROFILE_FUNCTION();

    if (!is_hmd_active()) {
        m_needs_camera_restore = false;
        return;
    }

    if (inside_on_end) {
        return;
    }

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        spdlog::error("VR: Failed to get primary camera!");
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr) {
            break;
        }

        static auto via_camera = sdk::find_type_definition("via.Camera");
        static auto get_near_clip_plane_method = via_camera->get_method("get_NearClipPlane");
        static auto get_far_clip_plane_method = via_camera->get_method("get_FarClipPlane");
        static auto set_far_clip_plane_method = via_camera->get_method("set_FarClipPlane");
        static auto set_fov_method = via_camera->get_method("set_FOV");
        static auto set_vertical_enable_method = via_camera->get_method("set_VerticalEnable");
        static auto set_aspect_ratio_method = via_camera->get_method("set_AspectRatio");

        if (m_use_custom_view_distance->value()) {
            set_far_clip_plane_method->call<void*>(sdk::get_thread_context(), camera, m_view_distance->value());
        }

        m_nearz = get_near_clip_plane_method->call<float>(sdk::get_thread_context(), camera);
        m_farz = get_far_clip_plane_method->call<float>(sdk::get_thread_context(), camera);

        // Disable certain effects like the 3D overlay during the sewer gators
        // section in RE7
        disable_bad_effects();
        // Disable lens distortion
        set_lens_distortion(false);

        if ((sdk::GameIdentity::get().is_re2() || sdk::GameIdentity::get().is_re3()) && FirstPerson::get()->will_be_used()) {
            m_needs_camera_restore = false;

            auto camera_object = camera->get_game_object();

            if (camera_object == nullptr || camera_object->get_transform() == nullptr) {
                return;
            }

            auto camera_joint = utility::re_transform::get_joint(*camera_object->get_transform(), 0);

            if (camera_joint == nullptr) {
                return;
            }

            m_original_camera_position = sdk::get_joint_position(camera_joint);
            m_original_camera_rotation = sdk::get_joint_rotation(camera_joint);
            m_original_camera_matrix = Matrix4x4f{m_original_camera_rotation};
            m_original_camera_matrix[3] = m_original_camera_position;

            return;
        }

        // Flatscreen 3D: FoV/aspect stay game-controlled, and the runtime's
        // projections[] are identity placeholders anyway.
        if (!is_using_flat3d()) {
            auto projection_matrix = is_using_multipass() ? get_projection_matrix(0) : get_current_projection_matrix(true);

            // Steps towards getting lens flares and volumetric lighting working
            // Get the FOV from the projection matrix
            const auto vfov = glm::degrees(2.0f * std::atan(1.0f / projection_matrix[1][1]));
            const auto aspect = projection_matrix[1][1] / projection_matrix[0][0];
            const auto hfov = vfov * aspect;

            //spdlog::info("vFOV: {}", vfov);
            //spdlog::info("Aspect: {}", aspect);

            set_fov_method->call<void*>(sdk::get_thread_context(), camera, vfov);
            set_vertical_enable_method->call<void*>(sdk::get_thread_context(), camera, true);
            set_aspect_ratio_method->call<void*>(sdk::get_thread_context(), camera, aspect);
        }
    }

    update_camera_origin();
    m_needs_camera_restore = true;
}

void VR::update_camera_origin() {
    REF_PROFILE_FUNCTION();

    if (!is_hmd_active()) {
        return;
    }

    // this means that the user just put the headset on or something
    // and we caught them in a bad frame
    if (inside_on_end && !m_needs_camera_restore) {
        return;
    }

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        m_needs_camera_restore = false;
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr) {
            return;
        }

        auto camera_object = camera->get_game_object();

        if (camera_object == nullptr || camera_object->get_transform() == nullptr) {
            spdlog::error("VR: Failed to get camera game object or transform!");
            m_needs_camera_restore = false;
            return;
        }

        auto camera_joint = utility::re_transform::get_joint(*camera_object->get_transform(), 0);

        if (camera_joint == nullptr) {
            spdlog::error("VR: Failed to get camera joint!");
            m_needs_camera_restore = false;
            return;
        }

        if (!inside_on_end && camera == cameras[0]) {
            m_original_camera_position = sdk::get_joint_position(camera_joint);
            m_original_camera_rotation = sdk::get_joint_rotation(camera_joint);
            m_original_camera_matrix = Matrix4x4f{m_original_camera_rotation};
            m_original_camera_matrix[3] = m_original_camera_position;
        }

        apply_hmd_transform(camera_joint);
    }
}

void VR::apply_hmd_transform(glm::quat& rotation, Vector4f& position) {
    REF_PROFILE_FUNCTION();

    // Flatscreen 3D: no head tracking; camera orientation/position stay
    // game-controlled. The per-eye offset is applied in the view matrix hook.
    if (is_using_flat3d()) {
        return;
    }

    const auto rotation_offset = get_rotation_offset();
    const auto current_hmd_rotation = glm::normalize(rotation_offset * glm::quat{get_rotation(0)});
    
    glm::quat new_rotation{};
    glm::quat camera_rotation{};
    
    if (!m_decoupled_pitch->value()) {
        camera_rotation = rotation;
        new_rotation = glm::normalize(rotation * current_hmd_rotation);
    } else if (m_decoupled_pitch->value()) {
        // facing forward matrix
        const auto camera_rotation_matrix = utility::math::remove_y_component(Matrix4x4f{rotation});
        camera_rotation = glm::quat{camera_rotation_matrix};
        new_rotation = glm::normalize(camera_rotation * current_hmd_rotation);
    }

    auto current_relative_pos = rotation_offset * (get_position(0) - m_standing_origin) /*+ current_relative_eye_pos*/;
    current_relative_pos.w = 0.0f;

    auto current_head_pos = camera_rotation * current_relative_pos;

    rotation = new_rotation;
    position = position + current_head_pos;
}

void VR::apply_hmd_transform(::REJoint* camera_joint) {
    REF_PROFILE_FUNCTION();

    // Flatscreen 3D: leave the joint untouched entirely.
    if (is_using_flat3d()) {
        return;
    }

    auto rotation = m_original_camera_rotation;
    auto position = m_original_camera_position;

    apply_hmd_transform(rotation, position);

    sdk::set_joint_rotation(camera_joint, rotation);

    if (m_positional_tracking) {
        sdk::set_joint_position(camera_joint, position);   
    }
}

bool VR::is_hand_behind_head(VRRuntime::Hand hand, float sensitivity) const {
    if (hand > VRRuntime::Hand::RIGHT || !is_using_controllers()) {
        return false;
    }

    const auto hand_index = get_controllers()[(uint32_t)hand];

    const auto hmd = get_transform(0);
    const auto hand_pos = get_position(hand_index);
    const auto hmd_delta = Vector3f{hand_pos - hmd[3]};
    const auto distance = glm::length(hmd_delta);

    if (distance >= 0.3f) {
        return false;
    }

    const auto hmd_dir = glm::normalize(hmd_delta);

    const auto& hmd_forward = hmd[2];
    const auto flattened_forward = glm::normalize(Vector3f{hmd_forward.x, 0.0f, hmd_forward.z});

    const auto hand_dot_flat_raw = glm::dot(flattened_forward, hmd_dir);
    return hand_dot_flat_raw >= sensitivity;
}

void VR::update_audio_camera() {
    REF_PROFILE_FUNCTION();

    if (!is_hmd_active()) {
        return;
    }

    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    auto camera_object = camera->get_game_object();

    if (camera_object == nullptr || camera_object->get_transform() == nullptr) {
        return;
    }

    auto camera_joint = utility::re_transform::get_joint(*camera_object->get_transform(), 0);

    if (camera_joint == nullptr) {
        return;
    }

    m_original_audio_camera_position = sdk::get_joint_position(camera_joint);
    m_original_audio_camera_rotation = sdk::get_joint_rotation(camera_joint);
    m_needs_audio_restore = true;

    auto rotation = m_original_audio_camera_rotation;
    auto position = m_original_audio_camera_position;

    apply_hmd_transform(rotation, position);

    sdk::set_joint_rotation(camera_joint, rotation);

    if (m_positional_tracking) {
        sdk::set_joint_position(camera_joint, position);   
    }
}

void VR::update_render_matrix() {
    REF_PROFILE_FUNCTION();

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr || camera != cameras[0]) {
            return;
        }

        auto camera_object = camera->get_game_object();

        if (camera_object == nullptr || camera_object->get_transform() == nullptr) {
            return;
        }

        auto camera_joint = utility::re_transform::get_joint(*camera_object->get_transform(), 0);

        if (camera_joint == nullptr) {
            return;
        }

        m_render_camera_matrix = Matrix4x4f{sdk::get_joint_rotation(camera_joint)};
        m_render_camera_matrix[3] = sdk::get_joint_position(camera_joint);
    }
}

void VR::restore_audio_camera() {
    if (!m_needs_audio_restore) {
        return;
    }

    if ((sdk::GameIdentity::get().is_re2() || sdk::GameIdentity::get().is_re3()) && FirstPerson::get()->will_be_used()) {
        m_needs_audio_restore = false;
        return;
    }

    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        m_needs_audio_restore = false;
        return;
    }

    auto camera_object = camera->get_game_object();

    if (camera_object == nullptr || camera_object->get_transform() == nullptr) {
        m_needs_audio_restore = false;
        return;
    }

    //camera_object->get_transform()->worldTransform = m_original_camera_matrix;

    auto joint = utility::re_transform::get_joint(*camera_object->get_transform(), 0);

    if (joint == nullptr) {
        m_needs_audio_restore = false;
        return;
    }

    sdk::set_joint_rotation(joint, m_original_audio_camera_rotation);
    sdk::set_joint_position(joint, m_original_audio_camera_position);

    m_needs_audio_restore = false;
}

void VR::restore_camera() {
    REF_PROFILE_FUNCTION();

    if (!m_needs_camera_restore) {
        return;
    }

    if ((sdk::GameIdentity::get().is_re2() || sdk::GameIdentity::get().is_re3()) && FirstPerson::get()->will_be_used()) {
        m_needs_camera_restore = false;
        return;
    }

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        m_needs_camera_restore = false;
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr) {
            return;
        }

        auto camera_object = camera->get_game_object();

        if (camera_object == nullptr || camera_object->get_transform() == nullptr) {
            m_needs_camera_restore = false;
            return;
        }

        //camera_object->get_transform()->get_world_transform() = m_original_camera_matrix;

        auto joint = utility::re_transform::get_joint(*camera_object->get_transform(), 0);

        if (joint == nullptr) {
            m_needs_camera_restore = false;
            return;
        }

        sdk::set_joint_rotation(joint, m_original_camera_rotation);
        sdk::set_joint_position(joint, m_original_camera_position);
    }
    m_needs_camera_restore = false;
}

void VR::set_lens_distortion(bool value) {
    REF_PROFILE_FUNCTION();

    if (!m_force_lensdistortion_settings->value()) {
        return;
    }

    if (sdk::GameIdentity::get().is_re7()) {
        auto camera = sdk::get_primary_camera();

        if (camera == nullptr) {
            return;
        }

        static auto lens_distortion_tdef = sdk::find_type_definition(game_namespace("LensDistortionController"));
        static auto lens_distortion_t = lens_distortion_tdef->get_type();

        auto lens_distortion_component = camera->find(lens_distortion_t);

        if (lens_distortion_component != nullptr) {
            // Get "LensDistortion" field
            auto lens_distortion = *sdk::get_object_field<REManagedObject*>(lens_distortion_component, "LensDistortion");

            if (lens_distortion != nullptr) {
                // Call "set_Enabled" method
                sdk::call_object_func<void*>(lens_distortion, "set_Enabled", sdk::get_thread_context(), lens_distortion, value);
            }
        }
    }
}

void VR::disable_bad_effects() {
    REF_PROFILE_FUNCTION();

    auto context = sdk::get_thread_context();

    auto application = sdk::Application::get();

    // minor optimizations to prevent hashing and map lookups every frame
    static auto renderer_t = sdk::find_type_definition("via.render.Renderer");

    static auto get_render_config_method = renderer_t->get_method("get_RenderConfig");

    static auto render_config_t = sdk::find_type_definition("via.render.RenderConfig");

    static auto get_framerate_setting_method = render_config_t->get_method("get_FramerateSetting");
    static auto set_framerate_setting_method = render_config_t->get_method("set_FramerateSetting");

    static auto get_antialiasing_method = render_config_t->get_method("get_AntiAliasing");
    static auto set_antialiasing_method = render_config_t->get_method("set_AntiAliasing");

    static auto get_lens_distortion_setting_method = render_config_t->get_method("getLensDistortionSetting");
    static auto set_lens_distortion_setting_method = render_config_t->get_method("setLensDistortionSetting");

    static auto get_motion_blur_enable_method = render_config_t->get_method("get_MotionBlurEnable");
    static auto set_motion_blur_enable_method = render_config_t->get_method("set_MotionBlurEnable");

    static auto get_vsync_method = render_config_t->get_method("get_VSync");
    static auto set_vsync_method = render_config_t->get_method("set_VSync");

    static auto get_transparent_buffer_quality_method = render_config_t->get_method("get_TransparentBufferQuality");
    static auto set_transparent_buffer_quality_method = render_config_t->get_method("set_TransparentBufferQuality");

    static auto get_lensflare_enable_method = render_config_t->get_method("get_LensFlareEnable");
    static auto set_lensflare_enable_method = render_config_t->get_method("set_LensFlareEnable");

    static auto get_colorspace_method = render_config_t->get_method("get_ColorSpace");
    static auto set_colorspace_method = render_config_t->get_method("set_ColorSpace");

    static auto get_dynamic_shadow_enable_method = render_config_t->get_method("get_DynamicShadowEnable");
    static auto set_dynamic_shadow_enable_method = render_config_t->get_method("set_DynamicShadowEnable");

    static auto get_hdrmode_method = renderer_t->get_method("get_HDRMode");
    static auto set_hdrmode_method = renderer_t->get_method("set_HDRMode");

    static auto get_hdr_display_mode_enable_method = renderer_t->get_method("get_HDRDisplayModeEnable");
    static auto set_hdr_display_mode_enable_method = renderer_t->get_method("set_HDRDisplayModeEnable");

    static auto get_delay_render_enable_method = renderer_t->get_method("get_DelayRenderEnable");
    static auto set_delay_render_enable_method = renderer_t->get_method("set_DelayRenderEnable");

    auto renderer = renderer_t->get_instance();

    auto render_config = get_render_config_method->call<::REManagedObject*>(context, renderer);

    if (render_config == nullptr) {
        spdlog::info("No render config!");
        return;
    }

    static const auto is_sf6 = utility::get_module_path(utility::get_executable())->find("StreetFighter") != std::string::npos;

    if (!is_sf6 && m_force_fps_settings->value() && get_framerate_setting_method != nullptr && set_framerate_setting_method != nullptr) {
        const auto framerate_setting = get_framerate_setting_method->call<via::render::RenderConfig::FramerateType>(context, render_config);

        // Allow FPS to go above 60
        if (framerate_setting != via::render::RenderConfig::FramerateType::VARIABLE) {
            set_framerate_setting_method->call<void*>(context, render_config, via::render::RenderConfig::FramerateType::VARIABLE);
            spdlog::info("[VR] Set framerate to variable");
        }
    }
    
    // get_MaxFps on application
    if (!is_sf6 && m_force_fps_settings->value() && application->get_max_fps() <  600.0f) {
        application->set_max_fps(600.0f);

        static bool once = []() {
            spdlog::info("[VR] Max FPS set to {}", 600.0f);
            return true;
        }();
    }

    // Flat3D AFR/sequential MUST drop the game's temporal AA even when the
    // ForceAntiAliasing toggle is off (it defaults off on TDB>=69 because
    // multipass normally handles TAA). The game's TAA reprojects using motion
    // vectors that don't include Flat3D's per-eye camera offset, so it smears the
    // whole screen on motion and destabilizes foliage. Multipass is unavailable
    // on MHWilds (upscaler CreateFeature fails), so on Wilds this is the only way
    // to get a clean image. Switch to SMAA (spatial, no temporal history) rather
    // than NONE so edges/foliage still get anti-aliased.
    const auto flat3d_no_temporal = is_using_flat3d() && !is_using_multipass();

    // On MHWilds set_AntiAliasing does NOT stick - the game re-derives AA from its own graphics
    // options, so get_AntiAliasing still reports FXAA_TAA on the very next frame. That turned this
    // block into a per-frame "change the AA mode" storm the moment AFR/sequential armed
    // flat3d_no_temporal, and an AA change makes the engine rebuild its post-effect resources.
    // Doing that every frame from this hook crashed the game one frame after the switch
    // (c0000005, null deref inside the following RenderConfig call - see the multipass->AFR
    // crash log). Bound the flat3d attempts; if the game insists, leave its AA alone and say so.
    // The VR force-AA path is untouched: where the setter sticks, the retry never happens anyway.
    constexpr uint32_t k_max_flat3d_aa_attempts = 1;
    static uint32_t s_flat3d_aa_attempts = 0;
    static bool s_flat3d_aa_armed = false;

    if (flat3d_no_temporal != s_flat3d_aa_armed) {
        s_flat3d_aa_armed = flat3d_no_temporal;
        s_flat3d_aa_attempts = 0; // re-arm on every technique change
    }

    if ((m_force_aa_settings->value() || flat3d_no_temporal) && get_antialiasing_method != nullptr && set_antialiasing_method != nullptr) {
        const auto antialiasing = get_antialiasing_method->call<via::render::RenderConfig::AntiAliasingType>(context, render_config);
        const auto target_aa = flat3d_no_temporal
            ? via::render::RenderConfig::AntiAliasingType::SMAA
            : via::render::RenderConfig::AntiAliasingType::NONE;

        if (flat3d_no_temporal) {
            static via::render::RenderConfig::AntiAliasingType s_last_logged = (via::render::RenderConfig::AntiAliasingType)-1;
            if (antialiasing != s_last_logged) {
                s_last_logged = antialiasing;
                // 0=FXAA 1=TAA 2=FXAA_TAA 3=SMAA 4=NONE
                spdlog::info("[Flat3D] game AntiAliasing reports {} (0=FXAA 1=TAA 2=FXAA_TAA 3=SMAA 4=NONE)", (int)antialiasing);
            }
        }

        // Replace any TEMPORAL AA with the non-temporal target.
        switch (antialiasing) {
            case via::render::RenderConfig::AntiAliasingType::TAA: [[fallthrough]];
            case via::render::RenderConfig::AntiAliasingType::FXAA_TAA:
                if (flat3d_no_temporal && s_flat3d_aa_attempts >= k_max_flat3d_aa_attempts) {
                    static bool s_warned = false;

                    if (!s_warned) {
                        s_warned = true;
                        spdlog::warn("[Flat3D] the game keeps re-applying temporal AA; giving up rather than "
                            "fighting it every frame. Set Anti-Aliasing to SMAA/FXAA/None in the game's "
                            "graphics options - TAA smears badly in AFR.");
                    }

                    break;
                }

                if (flat3d_no_temporal) {
                    ++s_flat3d_aa_attempts;
                }

                set_antialiasing_method->call<void*>(context, render_config, target_aa);
                spdlog::info("[VR] Temporal AA replaced with {}", flat3d_no_temporal ? "SMAA (flat3d)" : "NONE");
                break;
            default:
                break;
        }
    }

    if (m_force_lensdistortion_settings->value() && get_lens_distortion_setting_method != nullptr && set_lens_distortion_setting_method != nullptr) {
        const auto lens_distortion_setting = get_lens_distortion_setting_method->call<via::render::RenderConfig::LensDistortionSetting>(context, render_config);

        // Disable lens distortion
        if (lens_distortion_setting != via::render::RenderConfig::LensDistortionSetting::OFF) {
            set_lens_distortion_setting_method->call<void*>(context, render_config, via::render::RenderConfig::LensDistortionSetting::OFF);
            spdlog::info("[VR] Lens distortion disabled");
        }
    }

    if (m_force_motionblur_settings->value() && get_motion_blur_enable_method != nullptr && set_motion_blur_enable_method != nullptr) {
        const auto is_motion_blur_enabled = get_motion_blur_enable_method->call<bool>(context, render_config);

        // Disable motion blur
        if (is_motion_blur_enabled) {
            set_motion_blur_enable_method->call<void*>(context, render_config, false);
            spdlog::info("[VR] Motion blur disabled");
        }
    }

    if (m_force_vsync_settings->value() && get_vsync_method != nullptr && set_vsync_method != nullptr) {
        if (!sdk::GameIdentity::get().is_mhrise()) {
            const auto vsync = get_vsync_method->call<bool>(context, render_config);

            // Disable vsync
            if (vsync) {
                set_vsync_method->call<void*>(context, render_config, false);
                spdlog::info("[VR] VSync disabled");
            }
        } else {
            // We are only calling set_vsync instead of checking with get_vsync in MHRise
            // because get_VSync has some insane code protection on it for some reason
            // which would increase frametimes to 15+
            set_vsync_method->call<void*>(context, render_config, false);
        }
    }

    if (m_force_volumetrics_settings->value() && get_transparent_buffer_quality_method != nullptr && set_transparent_buffer_quality_method != nullptr) {
        const auto transparent_buffer_quality = get_transparent_buffer_quality_method->call<via::render::RenderConfig::Quality>(context, render_config);

        // Disable volumetrics
        if (transparent_buffer_quality != via::render::RenderConfig::Quality::NONE) {
            set_transparent_buffer_quality_method->call<void*>(context, render_config, via::render::RenderConfig::Quality::NONE);
            // The game re-enables this every frame, so we re-disable every frame - but only log once.
            static bool s_logged_vol = false;
            if (!s_logged_vol) { s_logged_vol = true; spdlog::info("[VR] Volumetrics disabled"); }
        }
    }

    if (m_force_lensflares_settings->value() && get_lensflare_enable_method != nullptr && set_lensflare_enable_method != nullptr) {
        const auto is_lensflare_enabled = get_lensflare_enable_method->call<bool>(context, render_config);

        // Disable lensflares
        if (is_lensflare_enabled) {
            set_lensflare_enable_method->call<void*>(context, render_config, false);
            // The game re-enables this every frame, so we re-disable every frame - but only log once.
            static bool s_logged_lens = false;
            if (!s_logged_lens) { s_logged_lens = true; spdlog::info("[VR] Lensflares disabled"); }
        }
    }

    if (get_hdrmode_method != nullptr && set_hdrmode_method != nullptr) {
        // static
        const auto is_hdr_enabled = get_hdrmode_method->call<bool>(context);

        // Disable HDR
        if (is_hdr_enabled) {
            set_hdrmode_method->call<void*>(context, false);
            
            if (set_hdr_display_mode_enable_method != nullptr) {
                set_hdr_display_mode_enable_method->call<void*>(context, false);
            }

            spdlog::info("[VR] HDR disabled");
        }
    }

    if (get_colorspace_method != nullptr && set_colorspace_method != nullptr) {
        const auto is_hdr_enabled = get_colorspace_method->call<via::render::ColorSpace>(context, render_config) == via::render::ColorSpace::HDR10;

        if (is_hdr_enabled) {
            set_colorspace_method->call<void*>(context, render_config, via::render::ColorSpace::HDTV);
            spdlog::info("[VR] HDR disabled (ColorSpace)");
        }
    }

    if (m_force_dynamic_shadows_settings->value() && get_dynamic_shadow_enable_method != nullptr && set_dynamic_shadow_enable_method != nullptr) {
        const auto is_dynamic_shadow_enabled = get_dynamic_shadow_enable_method->call<bool>(context, render_config);

        // Enable dynamic shadows
        if (!is_dynamic_shadow_enabled) {
            set_dynamic_shadow_enable_method->call<void*>(context, render_config, true);
            spdlog::info("[VR] Dynamic shadows enabled");
        }
    }

    // Causes crashes on D3D11. May be a performance detriment in VR with new rendering method.
    const auto is_new_rendering_method = get_rendering_technique() == VR::RenderingTechnique::MULTIPASS;

    // Flat3D true-sequential MUST render synchronously (engine delay-render OFF): its re-run
    // re-enters BeginRendering/EndRendering every frame, and with delay-render ON (async
    // pipelining) the render thread runs a frame behind, so two consecutive re-runs overflow
    // the render pipeline and the next BeginRendering deadlocks. Force delay-render off for it
    // regardless of the async toggle. (Setting an explicit false here, NOT !async, because for
    // flat3d the async toggle is off and !false would wrongly re-ENABLE delay-render.)
    const bool async_wants_delay_off = !is_new_rendering_method && !is_sf6
        && g_framework->get_renderer_type() == REFramework::RendererType::D3D12
        && m_enable_asynchronous_rendering->value();
    const bool flat3d_wants_delay_off = false && is_using_flat3d_true_sequential()
        && g_framework->get_renderer_type() == REFramework::RendererType::D3D12;

    if (async_wants_delay_off || flat3d_wants_delay_off) {
        if (get_delay_render_enable_method != nullptr && set_delay_render_enable_method != nullptr) {
            const auto is_delay_render_enabled = get_delay_render_enable_method->call<bool>(context);

            if (is_delay_render_enabled == true) {
                set_delay_render_enable_method->call<void*>(context, false);
                spdlog::info("[VR] Delay render modified (disabled)");
            }
        }
    } else if (is_sf6) {
        // Must be on in SF6 or left eye gets stuck
        if (set_delay_render_enable_method != nullptr) {
            set_delay_render_enable_method->call<void*>(context, true);
        }
    }

    if (sdk::GameIdentity::get().is_re7()) {
        auto camera = sdk::get_primary_camera();

        if (camera == nullptr) {
            return;
        }

        auto camera_game_object = camera->get_game_object();

        if (camera_game_object == nullptr) {
            return;
        }
    
        auto camera_transform = camera_game_object->get_transform();

        if (camera_transform == nullptr) {
            return;
        }

        auto get_type = [](std::string name) {
            auto tdef = sdk::find_type_definition(name);
            return tdef->get_type();
        };

        static auto effect_player_t = get_type("via.effect.EffectPlayer");
        static auto hide_effect_for_vr_t = get_type(game_namespace("EPVStandard.HideEffectForVR"));

        // Do not draw the game object if it is hidden for VR (this was there for the original PSVR release i guess)
        for (auto child = camera_transform->get_child(); child != nullptr; child = child->get_child()) {
            if (child->find(effect_player_t) != nullptr) {
                if (child->find(hide_effect_for_vr_t) != nullptr) {
                    auto game_object = child->get_game_object();

                    game_object->set_shouldDraw(false);
                    //sdk::call_object_func<void*>(game_object, "set_Draw", sdk::get_thread_context(), game_object, false);
                }
            }
        }
    }
}

void VR::fix_temporal_effects() {
    // this is SO DUMB!!!!!
    const auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }
    
    static auto render_output_type = sdk::find_type_definition("via.render.RenderOutput")->get_type();
    auto render_output_component = camera->find(render_output_type);

    if (render_output_component == nullptr) {
        return;
    }

    if (!get_runtime()->ready() || m_disable_temporal_fix) {
        sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 0); // None
        return;
    }

    if (m_frame_count % 2 == m_left_eye_interval) {
        sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 1); // left
    } else {
        sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 2); // right
    }
}

int32_t VR::get_frame_count() const {
    return get_game_frame_count();
}

int32_t VR::get_game_frame_count() const {
    static auto renderer_type = sdk::find_type_definition("via.render.Renderer");

    if (renderer_type == nullptr) {
        renderer_type = sdk::find_type_definition("via.render.Renderer");
        spdlog::warn("VR: Failed to find renderer type, trying again next time");
        return 0;
    }

    auto renderer = renderer_type->get_instance();

    if (renderer == nullptr) {
        return 0;
    }

    static auto get_render_frame_method = renderer_type->get_method("get_RenderFrame");

    return get_render_frame_method->call<int32_t>(sdk::get_thread_context(), renderer);
}

float VR::get_standing_height() {
    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin.y;
}

Vector4f VR::get_standing_origin() {
    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin;
}

void VR::set_standing_origin(const Vector4f& origin) {
    std::unique_lock _{ get_runtime()->pose_mtx };
    
    m_standing_origin = origin;
}

glm::quat VR::get_rotation_offset() {
    std::shared_lock _{ m_rotation_mtx };

    return m_rotation_offset;
}

void VR::set_rotation_offset(const glm::quat& offset) {
    std::unique_lock _{ m_rotation_mtx };

    m_rotation_offset = offset;
}

void VR::recenter_view() {
    const auto new_rotation_offset = glm::normalize(glm::inverse(utility::math::flatten(glm::quat{get_rotation(0)})));

    set_rotation_offset(new_rotation_offset);
}

glm::quat VR::get_gui_rotation_offset() {
    std::shared_lock _{ m_gui_mtx };

    return m_gui_rotation_offset;
}

void VR::set_gui_rotation_offset(const glm::quat& offset) {
    std::unique_lock _{ m_gui_mtx };

    m_gui_rotation_offset = offset;
}

void VR::recenter_gui(const glm::quat& from) {
    const auto new_gui_offset = glm::normalize(glm::inverse(utility::math::flatten(from)));
    set_gui_rotation_offset(new_gui_offset);
}

uint32_t VR::get_eye_pass_index() const {
    if (is_using_multipass()) {
        return m_multipass.pass;
    }
    // Flat3D true-sequential: the main render is the LEFT eye; the engine re-run (inside_on_end)
    // is the RIGHT eye - both at the same tick. Keying off inside_on_end (instead of frame parity)
    // is what makes the re-run render the OTHER eye rather than "the same eye twice".
    if (is_using_flat3d() && is_using_sequential()) {
        return inside_on_end ? 1u : 0u;
    }
    return (uint32_t)m_frame_count;
}

Vector4f VR::get_current_offset() {
    if (!is_hmd_active()) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->eyes_mtx };

    const auto count = get_eye_pass_index();

    if (count % 2 == m_left_eye_interval) {
        //return Vector4f{m_eye_distance * -1.0f, 0.0f, 0.0f, 0.0f};
        return get_runtime()->eyes[vr::Eye_Left][3];
    }
    
    return get_runtime()->eyes[vr::Eye_Right][3];
    //return Vector4f{m_eye_distance, 0.0f, 0.0f, 0.0f};
}

Matrix4x4f VR::get_current_eye_transform(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    const auto count = get_eye_pass_index();
    const auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (count % 2 == mod_count) {
        return get_runtime()->eyes[vr::Eye_Left];
    }

    return get_runtime()->eyes[vr::Eye_Right];
}

Matrix4x4f VR::get_current_projection_matrix(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    const auto count = get_eye_pass_index();
    const auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (count % 2 == mod_count) {
        return get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
    }

    return get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
}

Matrix4x4f VR::get_projection_matrix(uint32_t pass) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    if (pass % 2 == 0) {
        return get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
    }

    return get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
}

Matrix4x4f VR::get_eye_transform(uint32_t pass) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    if (pass % 2 == 0) {
        return get_runtime()->eyes[vr::Eye_Left];
    }

    return get_runtime()->eyes[vr::Eye_Right];
}

void VR::on_pre_imgui_frame() {
    REF_PROFILE_FUNCTION();

    if (!get_runtime()->ready()) {
        return;
    }

    // Flatscreen 3D: no VR overlay; the framework menu draws to the backbuffer.
    if (is_using_flat3d()) {
        return;
    }

    m_overlay_component.on_pre_imgui_frame();
}

void VR::on_present() {
    REF_PROFILE_FUNCTION();

    // General flat3d present heartbeat (any technique) so an external monitor can tell healthy
    // rendering (heartbeat keeps growing) from a real freeze/device-removal (heartbeat stops).
    if (is_using_flat3d()) {
        static uint32_t s_fhb = 0;
        if ((s_fhb++ % 120) == 0) { spdlog::info("[Flat3D] present HEARTBEAT #{}", s_fhb - 1); }
    }


    // Flat3D true-sequential: with delay-render disabled the engine's EndRendering runs
    // synchronously, so the RE-RUN's EndRendering triggers an EXTRA swapchain present per frame
    // (inside_on_end). Two real presents per frame cycle the swapchain buffers twice, which
    // corrupts the per-backbuffer command-context/fence assumptions in D3D12Component::on_frame
    // and deadlocks. Fully suppress the re-run's present: skip our compose AND tell the D3D12
    // hook to skip the actual swapchain flip - only the real frame present composes+flips (both
    // eyes are already harvested by then).
    if (is_using_flat3d_true_sequential() && inside_on_end) {
        if (g_framework->is_dx12()) {
            if (auto& hook = g_framework->get_d3d12_hook(); hook != nullptr) {
                hook->ignore_next_present();
            }
        }

        return;
    }

    // UI extraction: the engine drew the UI into our clone (RENDER_TARGET) during this frame's
    // EndRendering (before this present). Transition it back to COMMON so next frame's
    // COMMON->RENDER_TARGET barrier is valid. (When we wire the compose read, this becomes
    // RENDER_TARGET->PIXEL_SHADER_RESOURCE instead, read in on_frame, then ->COMMON.)
    if (false) { // flat3d UI-extraction experiment removed
    }

    if (is_using_multipass() || is_using_flat3d_true_sequential() || (m_render_frame_count + 1) % 2 == m_left_eye_interval) {
        ResetEvent(m_present_finished_event);
    }

    auto runtime = get_runtime();

    if (!get_runtime()->loaded) {
        return;
    }

    auto openvr = get_runtime<runtimes::OpenVR>();

    if (runtime->is_openvr()) {
        if (openvr->got_first_poses) {
            const auto hmd_activity = openvr->hmd->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            auto hmd_active = hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction || hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;

            if (hmd_active) {
                openvr->last_hmd_active_time = std::chrono::system_clock::now();
            }

            const auto now = std::chrono::system_clock::now();

            if (now - openvr->last_hmd_active_time <= std::chrono::seconds(5)) {
                hmd_active = true;
            }

            openvr->is_hmd_active = hmd_active;

            // upon headset re-entry, reinitialize OpenVR
            if (openvr->is_hmd_active && !openvr->was_hmd_active) {
                openvr->wants_reinitialize = true;
            }

            openvr->was_hmd_active = openvr->is_hmd_active;

            if (!is_hmd_active()) {
                return;
            }
        } else {
            openvr->is_hmd_active = true; // We need to force out an initial WaitGetPoses call
            openvr->was_hmd_active = true;
        }
    }

    // attempt to fix crash when reinitializing openvr
    std::scoped_lock _{m_openvr_mtx};
    m_submitted = false;

    const auto renderer = g_framework->get_renderer_type();
    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (renderer == REFramework::RendererType::D3D11) {
        // if we don't do this then D3D11 OpenXR freezes for some reason.
        if (!runtime->got_first_sync) {
            runtime->synchronize_frame();
            runtime->update_poses();
        }

        m_is_d3d12 = false;
        e = m_d3d11.on_frame(this);
    } else if (renderer == REFramework::RendererType::D3D12) {
        m_is_d3d12 = true;
        e = m_d3d12.on_frame(this);
    }

    // force a waitgetposes call to fix this...
    if (e == vr::EVRCompositorError::VRCompositorError_AlreadySubmitted && runtime->is_openvr()) {
        openvr->got_first_poses = false;
        openvr->needs_pose_update = true;
    }

    m_last_frame_count = m_render_frame_count;

    if (m_submitted || runtime->needs_pose_update) {
        if (m_submitted) {
            m_overlay_component.on_post_compositor_submit();

            if (runtime->is_openvr()) {
                //vr::VRCompositor()->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
                vr::VRCompositor()->PostPresentHandoff();
            }
        }

        runtime->needs_pose_update = true;
        m_submitted = false;
    }

    if (is_using_multipass() || is_using_flat3d_true_sequential() || (m_render_frame_count + 1) % 2 == m_left_eye_interval) {
        SetEvent(m_present_finished_event);
    }
}

void VR::on_post_present() {
    REF_PROFILE_FUNCTION();

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    const auto renderer = g_framework->get_renderer_type();

    if (renderer == REFramework::RendererType::D3D12) {
        m_d3d12.on_post_present(this);
    }
    
    if (is_using_multipass() || (m_render_frame_count + 1) % 2 == m_left_eye_interval) {
        runtime->consume_events(nullptr);
    }

    if (!inside_on_end && runtime->wants_reinitialize) {
        std::scoped_lock _{m_openvr_mtx};

        if (runtime->is_openvr()) {
            m_openvr->wants_reinitialize = false;
            reinitialize_openvr();
        } else if (runtime->is_openxr()) {
            m_openxr->wants_reinitialize = false;
            reinitialize_openxr();
        }
    }
}

void VR::on_update_transform(RETransform* transform) {
    
}

void VR::on_update_camera_controller(RopewayPlayerCameraController* controller) {
    // get headset rotation
    /*const auto& headset_pose = m_game_poses[0];

    if (!headset_pose.bPoseIsValid) {
        return;
    }

    auto headset_matrix = Matrix4x4f{ *(Matrix3x4f*)&headset_pose.mDeviceToAbsoluteTracking };
    auto headset_rotation = glm::extractMatrixRotation(glm::rowMajor4(headset_matrix));

    headset_rotation *= get_current_rotation_offset();

    *(glm::quat*)&controller->worldRotation = glm::quat{ headset_rotation  };

    controller->worldPosition += get_current_offset();*/
}

struct GUIRestoreData {
    REComponent* element{nullptr};
    REComponent* view{nullptr};
    Vector4f original_position{ 0.0f, 0.0f, 0.0f, 1.0f };
    via::gui::ViewType view_type{ via::gui::ViewType::Screen };
    bool overlay{false};
    bool detonemap{true};
};

thread_local std::vector<std::unique_ptr<GUIRestoreData>> g_elements_to_reset{};

bool VR::on_pre_gui_draw_element(REComponent* gui_element, void* primitive_context) {
    REF_PROFILE_FUNCTION();

    inside_gui_draw = true;

    if (!get_runtime()->ready()) {
        return true;
    }

    // Flatscreen 3D: GUI stays game-controlled - GUI depth is handled at compose time by the
    // overlay-RT redirect (the world-space GUI experiment is retired).
    if (is_using_flat3d()) {
        return true;
    }

    auto game_object = gui_element->get_game_object();

    if (game_object != nullptr && game_object->get_transform() != nullptr) {
        auto context = sdk::get_thread_context();
        const auto& gi = sdk::GameIdentity::get();

        const auto name = game_object->get_name();
        const auto name_hash = utility::hash(name);

        switch (name_hash) {
        // Don't mess with this, causes weird black boxes on the sides of the screen
        case "GUI_PillarBox"_fnv:
        case "GUIEventPillar"_fnv:
        // These allow the cutscene transitions to display (fade to black)
        case "BlackFade"_fnv:
        case "WhiteFade"_fnv:
        case "Fade_In_Out_Black"_fnv:
        case "Fade_In_Out_White"_fnv:
        case "FadeInOutBlack"_fnv:
        case "FadeInOutWhite"_fnv:
        case "GUIBlackMask"_fnv:
        case "GenomeCodexGUI"_fnv:
        case "sm42_020_keystrokeDevice01A_gimmick"_fnv: // this one is the keypad in the locker room...
            return true;

        // the weird buggy overlay in the inventory
        case "GuiBack"_fnv:
            if (gi.is_re2() || gi.is_re3()) {
                return false;
            }
            break;

        case "Gui_ui2510"_fnv: // Black bars in cutscenes
            if (gi.is_re4()) {
                return false;
            }
            break;

        case "Gui_ui0440"_fnv: // Black bars in cutscenes
            if (gi.is_re9()) {
                game_object->set_shouldDraw(false);
                return false;
            }
            break;

        case "ui0199"_fnv: // weird black bars in Kunitsu-Gami
            if (gi.tdb_ver() >= 73) {
                return false;
            }
            break;

        default:
            break;
        };

        // Certain UI elements we want to remove when in VR (FirstPerson enabled)
        if ((gi.is_re2() || gi.is_re3()) && std::chrono::steady_clock::now() - m_last_crosshair_hide > std::chrono::seconds(1)) {
            auto& fp = FirstPerson::get();

            if (fp->is_enabled() && fp->will_be_used()) {
                const auto has_motion_controls = this->is_using_controllers();

                switch(name_hash) {
                case "GUI_Reticle"_fnv: // Crosshair
                    if (has_motion_controls) {
                        return false;
                    }
                    
                    break;
                default:
                    break;
                }
            }
        }

        if (gi.is_re7()) {
            if (name_hash == "HUD"_fnv) { // not a hero
                // Stops HUD element from being stuck to the screen
                sdk::call_object_func<REComponent*>(gui_element, "set_RenderTarget", context, gui_element, nullptr);
            }
        }

        //spdlog::info("VR: on_pre_gui_draw_element: {}", name);
        //spdlog::info("VR: on_pre_gui_draw_element: {} {:x}", name, (uintptr_t)game_object);

        auto view = sdk::call_object_func<REComponent*>(gui_element, "get_View", context, gui_element);

        if (view != nullptr) {
            const auto current_view_type = sdk::call_object_func<uint32_t>(view, "get_ViewType", context, view);

            if (current_view_type == (uint32_t)via::gui::ViewType::Screen) {
                static sdk::RETypeDefinition* via_render_mesh_typedef = nullptr;

                if (via_render_mesh_typedef == nullptr) {
                    via_render_mesh_typedef = sdk::find_type_definition("via.render.Mesh");

                    // wait
                    if (via_render_mesh_typedef == nullptr) {
                        return true;
                    }
                }

                // we don't want to mess with any game object that has a mesh
                // because it might be something physical in the game world
                // that the player can interact with
                if (game_object->get_transform()->find(via_render_mesh_typedef->get_type()) != nullptr) {
                    return true;
                }

                static auto ui_world_pos_attach_typedef = sdk::find_type_definition("app.UIWorldPosAttach");

                auto ui_scale = m_ui_scale_option->value();
                auto world_ui_scale = m_world_ui_scale_option->value();

                // (Flat3D analytic auto-sizing removed - flat3d early-returns above; GUI depth is
                // handled at compose time by the overlay-RT redirect.)

                auto& restore_data = g_elements_to_reset.emplace_back(std::make_unique<GUIRestoreData>());
                auto original_game_object_pos = sdk::get_transform_position(game_object->get_transform());

                restore_data->element = gui_element;
                restore_data->view = view;
                restore_data->original_position = original_game_object_pos;
                restore_data->overlay = sdk::call_object_func<bool>(view, "get_Overlay", context, view);
                restore_data->detonemap = sdk::call_object_func<bool>(view, "get_Detonemap", context, view);
                restore_data->view_type = (via::gui::ViewType)current_view_type;

                original_game_object_pos.w = 0.0f;

                // Set view type to world
                sdk::call_object_func<void*>(view, "set_ViewType", context, view, (uint32_t)via::gui::ViewType::World);

                // Set overlay = true (fixes double vision in VR)
                sdk::call_object_func<void*>(view, "set_Overlay", context, view, true);

                // Set detonemap = true (fixes weird tint)
                sdk::call_object_func<void*>(view, "set_Detonemap", context, view, true);

                // Go through the children until we hit a blur filter
                // And then remove it
                /*for (auto child = sdk::call_object_func<REComponent*>(view, "get_Child", sdk::get_thread_context(), view); child != nullptr; child = sdk::call_object_func<REComponent*>(child, "get_Child", sdk::get_thread_context(), child)) {
                    if (child->is_a("via.gui.BlurFilter")) {
                        // Call remove()
                        sdk::call_object_func<void*>(child, "remove", sdk::get_thread_context(), child);
                        break;
                    }
                }*/

                auto camera = sdk::get_primary_camera();

                // Set the gui element's position to be in front of the camera
                if (camera != nullptr) {
                    auto camera_object = camera->get_game_object();

                    if (camera_object != nullptr && camera_object->get_transform() != nullptr) {
                        auto& gui_matrix = game_object->get_transform()->get_world_transform();
                        auto child = sdk::call_object_func<REManagedObject*>(view, "get_Child", context, view);

                        auto fix_2d_position = [&](const Vector4f& target_position, 
                                                    bool screen_correction = true,
                                                    std::optional<float> custom_ui_scale = std::nullopt)
                        {
                            if (!custom_ui_scale) {
                                custom_ui_scale = world_ui_scale;
                            }

                            auto delta = target_position - m_render_camera_matrix[3];
                            delta.w = 0.0f;

                            auto dir = glm::normalize(delta);
                            dir.w = 0.0f;
                            
                            // make matrix from dir
                            const auto look_mat = glm::rowMajor4(glm::lookAtLH(Vector3f{}, Vector3f{ dir }, Vector3f(0.0f, 1.0f, 0.0f)));
                            const auto look_rot = glm::quat{look_mat};

                            auto new_pos = target_position;
                            new_pos.w = 1.0f;

                            gui_matrix = look_mat;
                            gui_matrix[3] = new_pos;

                            // LESSON: DO NOT CALL THESE METHODS ON THE TRANSFORM!
                            // THEY CAUSE SOME STRANGE BUGS WHEN THE GUI ELEMENT HAS A PARENT TRANSFORM!
                            // THE GUI RENDERING FUNCTIONS PERFORM ON THE WORLD MATRIX, SO THIS IS NOT NECESSARY.
                            //sdk::set_transform_position(game_object->get_transform(), new_pos);
                            //sdk::set_transform_rotation(game_object->get_transform(), look_rot);
                            
                            const auto scaled_ui_scale = *custom_ui_scale * 0.01f;
                            const auto distance = glm::length(delta);
                            const auto scale = std::clamp<float>(distance * scaled_ui_scale, 0.1f, 100.0f);

                            regenny::via::Size gui_size{};
                            sdk::call_object_func<void*>(view, "get_ScreenSize", &gui_size, context, view);
                            
                            auto fix_transform_object = [&](::REManagedObject* object) {
                                static auto transform_object_type = sdk::find_type_definition("via.gui.TransformObject");

                                if (object == nullptr) {
                                    return;
                                }

                                const auto t = object->get_type_definition();

                                if (t == nullptr || !t->is_a(transform_object_type)) {
                                    return;
                                }

                                //sdk::call_object_func<void*>(object, "set_ResolutionAdjust", context, object, true);

                                if (screen_correction) {
                                    Vector3f half_size{ gui_size.w / 2.0f, gui_size.h / 2.0f, 0.0f };
                                    sdk::call_object_func<void*>(object, "set_Position", context, object, &half_size);
                                }

                                //Vector4f new_scale{ scale, scale, scale, 1.0f };
                                const auto old_scale = sdk::call_object_func_easy<Vector4f>(object, "get_Scale");
                                Vector4f new_scale{ old_scale.y, old_scale.y, old_scale.z, 1.0f };
                                sdk::call_object_func<void*>(object, "set_Scale", context, object, &new_scale);
                            };

                            for (auto c = child; c != nullptr; c = sdk::call_object_func<REManagedObject*>(c, "get_Next", context, c)) {
                                fix_transform_object(c);
                            }

                            // Fix for other kinds of world pos attach elements.
                            if (gi.is_re7()) {
                                if (name_hash == "InteractOperationCursor"_fnv) {
                                    auto world_pos_attach_comp = game_object->get_transform()->find(ui_world_pos_attach_typedef->get_type());

                                    // Fix the world position of the gui element
                                    if (world_pos_attach_comp != nullptr) {
                                        auto target_cache = sdk::get_object_field<::REManagedObject*>(world_pos_attach_comp, "_TargetGUIElem");

                                        if (target_cache != nullptr && *target_cache != nullptr) {
                                            auto element = sdk::get_object_field<::REManagedObject*>(*target_cache, "_Element");

                                            if (element != nullptr && *element != nullptr) {
                                                Vector3f zero_size{ 0.0f, 0.0f, 0.0f };
                                                sdk::call_object_func<void*>(*element, "set_Position", context, *element, &zero_size);
                                            }
                                        }
                                    }
                                }
                            }

                            gui_matrix = glm::scale(gui_matrix, Vector3f{ scale, scale, scale });
                        };

                        auto camera_transform = camera_object->get_transform();

                        const auto& camera_matrix = m_original_camera_matrix;
                        const auto& camera_position = camera_matrix[3];

                        glm::quat wanted_rotation{};
                        
                        bool wants_face_glue = false;
                        auto ui_distance = m_ui_distance_option->value();

                        switch (name_hash) {
                        case "damage_ui2102"_fnv:
                        case "NightVision_Filter"_fnv:
                            wants_face_glue = true;
                            break;
                        default:
                            break;
                        };

                        float right_world_adjust{};

                        switch (name_hash) {
                        case "GuiFront"_fnv:
                        case "GUI_Pause"_fnv:
                        case "GUIPause"_fnv:
                        case "GUIMapBg"_fnv:
                        case "BG"_fnv:
                            ui_distance += 0.1f; // give everything a little pop in the inventory.
                            break;
                        case "GUI_MapBG"_fnv:
                        case "GUI_Map"_fnv:
                        case "GUI_MapGrid"_fnv:
                        case "GUIMap"_fnv:
                        case "GUIMapIcon"_fnv:
                            ui_distance -= 0.025f; // give everything a little pop in the inventory.
                            break;
                        case "GuiCaption"_fnv:
                        case "GUIInventory"_fnv:
                        case "GUIInventoryCraft"_fnv:
                        case "GUIInventoryKeyItem"_fnv:
                        case "GUIInventoryTreasure"_fnv:
                        case "GUIBinder"_fnv:
                        case "GUIEquip"_fnv:
                            ui_distance -= 0.1f; // give everything a little pop in the inventory.
                            break;
                        case "GUI_RemainingBullet"_fnv:
                            right_world_adjust = 0.15f;
                            break;
                        case "GUI_Reticle"_fnv:
                        case "ReticleGUI"_fnv:
                        case "GUIReticle"_fnv:
                            ui_distance = 5.0f;
                            ui_scale = 2.0f;
                            break;
                        case "hud_hunterwirewindow"_fnv:
                            ui_distance = 10.0f;
                            ui_scale = 1.0f;
                            break;
                        default:
                            break;
                        }

                        if (ui_distance < 0.0f) {
                            ui_distance = 0.0f;
                        }

                        // glues the GUI to the camera rotation and position
                        if (wants_face_glue) {
                            ui_distance = 5.0f;

                            wanted_rotation = glm::extractMatrixRotation(m_render_camera_matrix) * Matrix4x4f{
                                -1, 0, 0, 0,
                                0, 1, 0, 0,
                                0, 0, -1, 0,
                                0, 0, 0, 1
                            };
                        } else {
                            const auto gui_rotation_offset = get_gui_rotation_offset();

                            wanted_rotation = glm::extractMatrixRotation(camera_matrix) * Matrix4x4f{
                                -1, 0, 0, 0,
                                0, 1, 0, 0,
                                0, 0, -1, 0,
                                0, 0, 0, 1
                            };

                            if (m_decoupled_pitch->value()) {
                                bool is_exception = false;

                                switch (name_hash) {
                                    case "GUIReticle"_fnv:[[fallthrough]];
                                    case "ReticleGUI"_fnv:[[fallthrough]];
                                    case "GUI_Reticle"_fnv:
                                        is_exception = true;
                                        break;
                                    default:
                                        break;
                                }

                                if (!is_exception) {
                                    wanted_rotation = utility::math::flatten(wanted_rotation);
                                }
                            }

                            wanted_rotation = gui_rotation_offset * wanted_rotation;
                        }

                        const auto wanted_rotation_mat = Matrix4x4f{wanted_rotation};

                        gui_matrix = wanted_rotation_mat;
                        gui_matrix[3] = camera_position + (wanted_rotation_mat[2] * ui_distance) + (wanted_rotation_mat[0] * right_world_adjust);
                        gui_matrix[3].w = 1.0f;

                        // Scales the GUI so it's not massive. Dynamic sizing: scale grows
                        // proportionally with the plane distance, so the APPARENT size stays
                        // constant as the depth slider pushes the GUI away (anchored at d=1,
                        // where the flat3d auto-fit was calibrated; HMD default d=1 unchanged).
                        if (!wants_face_glue) {
                            const auto scale = std::max(ui_distance, 0.05f) / ui_scale;
                            gui_matrix = glm::scale(gui_matrix, Vector3f{ scale, scale, scale });
                        }

                        static auto gui_driver_typedef = sdk::find_type_definition(game_namespace("GUIDriver"));
                        static auto mhrise_npc_head_message_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonNpcHeadMessage"));
                        static auto mhrise_speech_balloon_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonNpcSpeechBalloon"));
                        static auto mhrise_head_message_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonHeadMessage"));
                        static auto mhrise_otomo_head_message_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonOtomoHeadMessage"));
                        static auto kg_float_icon_behavior = sdk::find_type_definition("app.FloatIconBehavior");
                        static auto kg_general_vital_gauge_behavior = sdk::find_type_definition("app.GeneralVitalGaugeBehavior");

                        static auto gameobject_elements_list = {
                            mhrise_npc_head_message_typedef,
                            mhrise_speech_balloon_typedef,
                            mhrise_head_message_typedef,
                            mhrise_otomo_head_message_typedef,
                            kg_float_icon_behavior,
                            kg_general_vital_gauge_behavior
                        };
                        
                        // Fix position of interaction icons
                        if (name_hash == "GUI_FloatIcon"_fnv || name_hash == "RogueFloatIcon"_fnv || name_hash == "Gui_FloatIcon"_fnv) { // RE2, RE3, RE4
                            if (name_hash == "GUI_FloatIcon"_fnv || name_hash == "Gui_FloatIcon"_fnv) {
                                m_last_interaction_display = std::chrono::steady_clock::now();
                            }
                        
                            fix_2d_position(original_game_object_pos);
                        } else if(gui_driver_typedef != nullptr) { // RE8
                            auto interact_icon_comp = game_object->get_transform()->find(gui_driver_typedef->get_type());

                            if (interact_icon_comp != nullptr) {
                                auto interact_icon_object = sdk::call_object_func<REGameObject*>(interact_icon_comp, "get_attachTarget", context, interact_icon_comp);

                                if (interact_icon_object != nullptr && interact_icon_object->get_transform() != nullptr) {
                                    // call get_Position on the object
                                    Vector4f interact_icon_position{};
                                    sdk::call_object_func<Vector4f*>(interact_icon_object->get_transform(), "get_Position", &interact_icon_position, context, interact_icon_object->get_transform());

                                    fix_2d_position(interact_icon_position);
                                }
                            }
                        } else {
                            // MHRise
                            for (auto element_type : gameobject_elements_list) {
                                if (element_type == nullptr) {
                                    continue;
                                }

                                auto element_comp = game_object->get_transform()->find(element_type->get_type());

                                if (element_comp == nullptr) {
                                    continue;
                                }

                                static auto get_parent_method = sdk::get_object_method(game_object->get_transform(), "get_Parent");
                                auto parent = get_parent_method->call<::RETransform*>(context, game_object->get_transform());

                                if (parent != nullptr) {
                                    Vector4f offset{};

                                    std::optional<float> custom_ui_scale = std::nullopt;

                                    if (element_type == mhrise_speech_balloon_typedef) {
                                        static auto pos_data_field = mhrise_speech_balloon_typedef->get_field("posData");
                                        static auto npc_message_pos = pos_data_field->get_type()->get_field("NpcMessagePos");

                                        auto pos_data = pos_data_field->get_data<::REManagedObject*>(element_comp);

                                        if (pos_data != nullptr) {
                                            const auto y_offset = npc_message_pos->get_data<float>(pos_data);
                                            offset = Vector4f{0.0f, y_offset, 0.0f, 0.0f};
                                        } else {
                                            offset = Vector4f{0.0f, 1.0f, 0.0f, 0.0f};
                                        }
                                    } else if (element_type == mhrise_npc_head_message_typedef) {
                                        static auto pos_data_field = mhrise_npc_head_message_typedef->get_field("posData");
                                        static auto npc_message_pos = pos_data_field->get_type()->get_field("NpcMessagePos");
                                        
                                        auto pos_data = pos_data_field->get_data<::REManagedObject*>(element_comp);

                                        if (pos_data != nullptr) {
                                            const auto y_offset = npc_message_pos->get_data<float>(pos_data);
                                            offset = Vector4f{0.0f, y_offset, 0.0f, 0.0f};
                                        } else {
                                            offset = Vector4f{0.0f, 1.0f, 0.0f, 0.0f};
                                        }
                                    } else if (element_type == mhrise_head_message_typedef) {
                                        static auto message_pos_y_field = mhrise_head_message_typedef->get_field("MessagePosY");

                                        const auto y_offset = message_pos_y_field->get_data<float>(element_comp);
                                        offset = Vector4f{0.0f, y_offset, 0.0f, 0.0f};
                                    } else if (element_type == mhrise_otomo_head_message_typedef) { //airou and dog
                                        offset = Vector4f{0.0, 1.0f, 0.0f, 0.0f};
                                    } else if (element_type == kg_float_icon_behavior) {
                                        static auto param_field = kg_float_icon_behavior->get_field("_Param");
                                        static auto world_pos_field = param_field != nullptr ? param_field->get_type()->get_field("WorldPos") : nullptr;

                                        if (world_pos_field != nullptr) {
                                            auto param = param_field->get_data<::REManagedObject*>(element_comp);

                                            if (param != nullptr) {
                                                offset = world_pos_field->get_data<Vector4f>(param);
                                                custom_ui_scale = 5.0f;
                                            }
                                        }
                                    } else if (element_type == kg_general_vital_gauge_behavior) {
                                        static auto param_field = kg_general_vital_gauge_behavior->get_field("_OpenParam");
                                        static auto world_pos_field = param_field != nullptr ? param_field->get_type()->get_field("WorldPos") : nullptr;

                                        if (world_pos_field != nullptr) {
                                            auto param = param_field->get_data<::REManagedObject*>(element_comp);

                                            if (param != nullptr) {
                                                offset = world_pos_field->get_data<Vector4f>(param);
                                                custom_ui_scale = 5.0f;
                                            }
                                        }
                                    }

                                    fix_2d_position(sdk::get_transform_position(parent) + offset, true, custom_ui_scale);
                                } else {
                                    fix_2d_position(original_game_object_pos);
                                }
                            }
                        }

                        // ... RE7
                        if (gi.is_re7()) {
                            auto world_pos_attach_comp = game_object->get_transform()->find(ui_world_pos_attach_typedef->get_type());

                            // Fix the world position of the gui element
                            if (world_pos_attach_comp != nullptr) {
                                const auto& target_pos = *sdk::get_object_field<Vector4f>(world_pos_attach_comp, "_NowTargetPos");
                            
                                fix_2d_position(target_pos);
                            }
                        }
                    }
                }
            }
        }
    } else {
        spdlog::info("VR: on_pre_gui_draw_element: nullptr gameobject");
    }

    return true;
}

void VR::on_gui_draw_element(REComponent* gui_element, void* primitive_context) {
    REF_PROFILE_FUNCTION();

    //spdlog::info("VR: on_gui_draw_element");

    auto context = sdk::get_thread_context();

    // Restore elements back to original states
    for (auto& data : g_elements_to_reset) {
        sdk::call_object_func<void*>(data->view, "set_ViewType", context, data->view, (uint32_t)via::gui::ViewType::Screen);
        sdk::call_object_func<void*>(data->view, "set_Overlay", context, data->view, data->overlay);
        sdk::call_object_func<void*>(data->view, "set_Detonemap", context, data->view, data->detonemap);
        
        auto game_object = data->element->get_game_object();

        if (game_object != nullptr && game_object->get_transform() != nullptr) {
            //sdk::set_transform_position(game_object->get_transform(), data->original_position);

            auto& gui_matrix = game_object->get_transform()->get_world_transform();
            gui_matrix[3] = data->original_position;
        }
    }

    g_elements_to_reset.clear();
    inside_gui_draw = false;
}

void VR::on_pre_update_before_lock_scene(void* ctx) {
    /*auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    auto projection_matrix = get_current_projection_matrix(true);

    // Steps towards getting lens flares and volumetric lighting working
    // Get the FOV from the projection matrix
    const auto vfov = glm::degrees(2.0f * std::atan(1.0f / projection_matrix[1][1]));
    const auto aspect = projection_matrix[1][1] / projection_matrix[0][0];
    const auto hfov = vfov * aspect;
    
    spdlog::info("vFOV: {}", vfov);
    spdlog::info("Aspect: {}", aspect);

    sdk::call_object_func<void*>(camera, "set_FOV", sdk::get_thread_context(), camera, vfov);
    sdk::call_object_func<void*>(camera, "set_VerticalEnable", sdk::get_thread_context(), camera, true);
    sdk::call_object_func<void*>(camera, "set_AspectRatio", sdk::get_thread_context(), camera, aspect);*/
}

void VR::on_pre_lightshaft_draw(void* shaft, void* render_context) {
    m_in_lightshaft = true;

    /*static auto transparent_layer_t = sdk::find_type_definition("via.render.layer.Transparent");
    auto transparent_layer = sdk::renderer::find_layer(transparent_layer_t->type);

    spdlog::info("transparent layer: {:x}", (uintptr_t)transparent_layer);

    if (transparent_layer == nullptr) {
        return;
    }

    static auto scene_layer_t = sdk::find_type_definition("via.render.layer.Scene");
    auto scene_layer = transparent_layer->find_parent(scene_layer_t->type);

    spdlog::info("scene layer: {:x}", (uintptr_t)scene_layer);

    if (scene_layer == nullptr) {
        return;
    }

    scene_layer->update();

    spdlog::info("scene layer update: {:x}", (uintptr_t)(*(void***)scene_layer)[sdk::renderer::RenderLayer::UPDATE_VTABLE_INDEX]);*/
}

void VR::on_lightshaft_draw(void* shaft, void* render_context) {
    m_in_lightshaft = false;
}

thread_local bool timed_out = false;

void VR::on_pre_begin_rendering(void* entry) {
    REF_PROFILE_FUNCTION();

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    m_in_render = true;

    // Use the gamepad/motion controller sticks to lerp the standing origin back to the center
    if (m_via_hid_gamepad.update()) {
        auto pad = sdk::call_native_func_easy<REManagedObject*>(m_via_hid_gamepad.object, m_via_hid_gamepad.t, "get_LastInputDevice");

        if (pad != nullptr) {
            // Move direction
            // It's not a Vector2f because via.vec2 is not actually 8 bytes, we don't want stack corruption to occur.
            const auto axis_l = (Vector2f)*pad->get_reflection_property<Vector3f*>("AxisL");
            const auto axis_r = (Vector2f)*pad->get_reflection_property<Vector3f*>("AxisR");

            // Lerp the standing origin back to HMD position
            // if the user is moving
            if (glm::length(axis_l) > 0.0f || glm::length(axis_r) > 0.0f) {
                const auto highest_length = std::max<float>(glm::length(axis_l), glm::length(axis_r));
                auto new_pos = get_position(vr::k_unTrackedDeviceIndex_Hmd);

                const auto delta = sdk::Application::get()->get_delta_time();

                new_pos.y = m_standing_origin.y;
                // Don't set the Y because it would look really strange
                m_standing_origin = glm::lerp(m_standing_origin, new_pos, ((float)highest_length * delta) * 0.01f);
            }
        }

        // TODO: do the same thing for the keyboard
        // will probably require some game-specific code
    }

    detect_controllers();

    //actual_frame_count = get_game_frame_count();
    m_frame_count++;
    actual_frame_count = m_frame_count;

    /*if (!inside_on_end) {
        spdlog::info("VR: frame count: {}", m_frame_count);
    } else {
        spdlog::info("VR: frame count: {} (inside on_end)", m_frame_count);
    }*/

    // if we timed out, just return. we're assuming that the rendering will go on as normal
    if (timed_out) {
        if (inside_on_end) {
            spdlog::warn("VR: on_pre_wait_rendering: timed out inside_on_end");
        } else {
            spdlog::warn("VR: on_pre_wait_rendering: timed out");
        }

        return;
    }

    if (runtime->needs_pose_update && inside_on_end) {
        spdlog::info("VR: on_pre_wait_rendering: inside on end!");
    }
    
    // Call WaitGetPoses
    // Flat3D true-sequential renders BOTH eyes in one frame (main pass + re-run), so it must
    // update the pose/camera every frame like multipass - never gate to even frames (which
    // would leave odd frames rendering a stale camera and, via the present-event pacing below,
    // stall to ~3fps).
    if (is_using_multipass() || (is_using_flat3d_true_sequential() && !inside_on_end) || (!inside_on_end && m_frame_count % 2 == m_left_eye_interval)) {
        update_hmd_state();
    }

    const auto should_update_camera = (m_frame_count % 2 == m_left_eye_interval) || is_using_afr() || is_using_multipass() || is_using_flat3d_true_sequential();

    if (!inside_on_end && should_update_camera) {
        update_camera();
    } else if (inside_on_end) {
        update_camera_origin();
    }

    // update our internally stored render matrix
    update_render_matrix();
    //fix_temporal_effects(); // BAD way to do it!
}

void VR::on_begin_rendering(void* entry) {
    //spdlog::info("BeginRendering");
}

void VR::on_pre_end_rendering(void* entry) {
    REF_PROFILE_FUNCTION();

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    if (runtime->ready() && (m_frame_count % 2 == m_left_eye_interval || is_using_multipass())) {
        const auto stage = runtime->get_synchronize_stage();

        if (stage == VRRuntime::SynchronizeStage::LATE && runtime->synchronize_frame() == VRRuntime::Error::SUCCESS) {
            if (runtime->is_openxr()) {
                m_openxr->begin_frame();
            }
        }
    }
}

void VR::on_end_rendering(void* entry) {
    REF_PROFILE_FUNCTION();

    if (is_using_flat3d_true_sequential()) {
    }

    // we set this because we've enabled asynchronous rendering
    // by the time the next frame (right eye) starts,
    // the frame count might get modified, screwing up our logic
    // so we need a render frame count to compare against
    m_render_frame_count = m_frame_count;

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    // TODO: Check later if this is even necessary
    if ((!runtime->ready()) && !inside_on_end) {
        restore_camera();

        inside_on_end = false;
        m_in_render = false;
        return;
    }

    if (!inside_on_end) {
        auto app = sdk::Application::get();

        static auto app_type = sdk::find_type_definition("via.Application");
        static auto set_max_delta_time_fn = app_type->get_method("set_MaxDeltaTime");

        // RE8 and onwards...
        // defaults to 2, and will slow the game down if frame rate is too low
        if (set_max_delta_time_fn != nullptr) {
            // static func, no need for app
            set_max_delta_time_fn->call<void*>(sdk::get_thread_context(), 10.0f);
        }
    }

    if (is_using_afr() || inside_on_end) {
        if (is_using_afr()) {
            restore_camera();
            m_in_render = false;
        }

        return;
    }

    if (is_using_multipass()) {
        restore_camera();
        m_in_render = false;
        inside_on_end = false;

        const auto temporal_upscaler = TemporalUpscaler::get();

        if (temporal_upscaler->activated()) {
            m_multipass.eye_textures[0] = (ID3D12Resource*)temporal_upscaler->get_upscaled_texture<void*>(0);
            m_multipass.eye_textures[1] = (ID3D12Resource*)temporal_upscaler->get_upscaled_texture<void*>(1);
        } else {
            const auto scene_layers = m_camera_duplicator.get_relevant_scene_layers();

            if (scene_layers.size() < 2) {
                spdlog::warn("VR: on_end_rendering: scene layers are less than 2: {}", scene_layers.size());
                return;
            }

            static auto potype = sdk::find_type_definition("via.render.layer.PrepareOutput")->get_type();

            std::array<sdk::renderer::layer::PrepareOutput**, 2> prepare_output_layers{
                (sdk::renderer::layer::PrepareOutput**)scene_layers[0]->find_layer(potype),
                (sdk::renderer::layer::PrepareOutput**)scene_layers[1]->find_layer(potype)
            };

            if (prepare_output_layers[0] == nullptr || prepare_output_layers[1] == nullptr) {
                spdlog::warn("VR: on_end_rendering: prepare output layers are null: {:x} {:x}", (uintptr_t)prepare_output_layers[0], (uintptr_t)prepare_output_layers[1]);
                return;
            }

            std::array<sdk::renderer::TargetState*, 2> output_states{
                (*prepare_output_layers[0])->get_output_state(),
                (*prepare_output_layers[1])->get_output_state()
            };

            if (output_states[0] == nullptr || output_states[1] == nullptr) {
                spdlog::warn("VR: on_end_rendering: output states are null: {:x} {:x}", (uintptr_t)output_states[0], (uintptr_t)output_states[1]);
                return;
            }

            if (g_framework->is_dx12()) {
                bool force_reset = false;

                // Multipass harvest (VR and Flat3D both): allocate the per-eye native_res_copies
                // clones once (via create_texture). Each eye's prepared color is copied into
                // them in-stream by on_prepare_output_layer_draw (context->copy_texture); the
                // eye_texture population below points at the clones' native resources, which
                // D3D12Component reads at present (Flat3D converts HDR -> 8-bit per eye).
                //
                if (m_multipass.allocated_size[0] != get_hmd_width() || m_multipass.allocated_size[1] != get_hmd_height()) {
                    const auto rtv0 = output_states[0]->get_rtv(0);
                    const auto rtv1 = output_states[1]->get_rtv(0);

                    if (rtv0 != nullptr && rtv1 != nullptr) {
                        // Flat3D clones the OUTPUT target (fmt 24) - the copy sources the overlay's
                        // main target (the same shared output resource, populated at the pre-overlay
                        // hook). The scene intermediates (PostMainTarget) are recycled/black at every
                        // hook we can reach, so we harvest the final composited output instead.
                        const auto tex0 = rtv0->get_texture_d3d12();
                        const auto tex1 = rtv1->get_texture_d3d12();

                        if (tex0 != nullptr && tex1 != nullptr) {
                            m_multipass.native_res_copies[0] = tex0->clone();
                            m_multipass.native_res_copies[1] = tex1->clone();

                            // Flat3D (Wilds): the engine copy_texture into a create_texture clone
                            // crashes because the clone's per-subresource state tracker doesn't
                            // match the copy's desired COPY_DEST state, so the executor tries to
                            // record a transition against an unregistered resource (find() walks
                            // off the end). Prime the tracker to COPY_DEST (0x400) so the copy's
                            // dst-resolve takes the no-transition fast path - no registry, no
                            // crash. (See prime_copy_dest_state; verified from executor find() at
                            // exe+0xab4e222 which passes desired=0x400 for the copy dst.)

                            m_multipass.allocated_size[0] = get_hmd_width();
                            m_multipass.allocated_size[1] = get_hmd_height();

                            force_reset = true;

                            spdlog::info("Allocated native res copies");
                        } else {
                            spdlog::warn("VR: on_end_rendering: texs are null: {:x} {:x}", (uintptr_t)tex0.get(), (uintptr_t)tex1.get());
                        }
                    } else {
                        spdlog::warn("VR: on_end_rendering: rtvs are null: {:x} {:x}", (uintptr_t)rtv0.get(), (uintptr_t)rtv1.get());
                    }
                }

                if (m_multipass.native_res_copies[0] != nullptr) {
                    const auto container = m_multipass.native_res_copies[0]->get_d3d12_resource_container();

                    if (container != nullptr) {
                        m_multipass.eye_textures[0] = container->get_native_resource();

                        if (m_multipass.eye_textures[0] == nullptr) {
                            spdlog::warn("VR: on_end_rendering: eye texture 0 is null");
                        }
                    }
                }

                if (!is_using_flat3d() && m_multipass.native_res_copies[1] != nullptr) {
                    const auto container = m_multipass.native_res_copies[1]->get_d3d12_resource_container();

                    if (container != nullptr) {
                        m_multipass.eye_textures[1] = container->get_native_resource();

                        if (m_multipass.eye_textures[1] == nullptr) {
                            spdlog::warn("VR: on_end_rendering: eye texture 1 is null");
                        }
                    }
                }

                if (force_reset) {
                    m_d3d12.force_reset();
                }
            } else {
                // TODO!
            }
        }

        return;
    }

    // VR sequential re-runs only on even (left) frames (perf: half the re-render cost, at the price
    // of a 1-frame eye stagger). Flat3D TRUE-sequential re-runs EVERY frame so BOTH eyes render at
    // the same tick from the real camera - no stagger, and the second eye gets the full VFX/lighting
    // render the multipass clone can't. Costs a full 2x scene render per frame.
    if (!inside_on_end && (is_using_flat3d_true_sequential() || m_render_frame_count % 2 == m_left_eye_interval)) {
        inside_on_end = true;


        // Try to render again for the right eye
        auto app = sdk::Application::get();

        static auto chain = app->generate_chain("WaitRendering", "EndRendering");
        static bool do_once = true;

        if (do_once) {
            do_once = false;

            // Remove these from the chain (std::vector)
            auto entries_to_remove = std::vector<std::string> {
                "WaitRendering",
                "UpdatePhysicsCharacterController",
                "UpdateTelemetry",
                "UpdateMovie", // Causes movies to play twice as fast if ran again
                "UpdateSpeedTree",
                "UpdateHansoft",
                "UpdatePuppet",
                // The dynamics stuff causes a cloth physics step in the right eye
                "BeginRenderingDynamics",
                "BeginDynamics",
                "EndRenderingDynamics",
                "EndDynamics",
                "EndPhysics",
                "RenderDynamics",
                "DevelopRenderer",
                "DrawWidget"
            };

            for (auto& entry : entries_to_remove) {
                chain.erase(std::remove_if(chain.begin(), chain.end(), [&](auto& func) {
                    return entry == func->get_description();
                }), chain.end());
            }

            if (sdk::GameIdentity::get().tdb_ver() < 73) {
                chain.erase(std::remove_if(chain.begin(), chain.end(), [](auto& func) {
                    return func->get_description() == std::string_view{"RenderLandscape"};
                }), chain.end());
            }
        }

        sdk::renderer::wait_rendering();
        sdk::renderer::begin_update_primitive();

        //static auto update_geometry = app->get_function("UpdateGeometry");
        static auto begin_update_effect = app->get_function("BeginUpdateEffect");
        static auto update_effect = app->get_function("UpdateEffect");
        static auto end_update_effect = app->get_function("EndUpdateEffect");
        static auto prerender_gui = app->get_function("PrerenderGUI");
        static auto begin_update_primitive = app->get_function("BeginUpdatePrimitive");

        // SO. Let me explain what's happening here.
        // If we try and just render a frame in this naive way in this order:
        // BeginUpdatePrimitive,
        // WaitRendering,
        // BeginRendering,
        // UpdatePrimitive,
        // EndPrimitive,
        // EndRendering,
        // This will end up having a chance to crash when rendering fluid effects for some reason when calling UpdatePrimitive.
        // The crash happens because some pipeline state inside the fluid simulator gets set to null.
        // So, we manually call BeginEffect, and then EndUpdateEffect
        // Which somehow solves the crash.
        // We don't call UpdateEffect because it will make effects appear to run at a higher framerate
        if (begin_update_effect != nullptr) {
            begin_update_effect->func(begin_update_effect->entry);
        }

        /*if (update_effect != nullptr) {
            update_effect->func(update_effect->entry);
        }*/

        if (end_update_effect != nullptr) {
            end_update_effect->func(end_update_effect->entry);
        }

        if (prerender_gui != nullptr) {
            prerender_gui->func(prerender_gui->entry);
        }

        for (auto func : chain) {
            func->func(func->entry);
        }

        restore_camera();

        m_in_render = false;
        inside_on_end = false;
    }
}

void VR::on_pre_wait_rendering(void* entry) {
}

void VR::on_wait_rendering(void* entry) {
    REF_PROFILE_FUNCTION();

    if (!get_runtime()->loaded) {
        return;
    }

    timed_out = false;

    if (!is_hmd_active()) {
        return;
    }

    if (is_using_multipass()) {
        return;
    }

    // Flat3D true-sequential renders BOTH eyes synchronously within one frame (main pass +
    // engine re-run). With the engine's delay-render disabled, rendering is already fully
    // serialized, so the present-event throttle is redundant AND harmful here: it injects an
    // uneven per-frame stall (frame N waits on frame N-1's present) that reads as stutter and
    // jerky parallax ("squish") during camera motion. Skip it - delay-render-off paces us.
    if (is_using_flat3d_true_sequential()) {
        return;
    }

    // wait for m_present_finished (std::condition_variable)
    // to be signaled
    // only on the left eye interval because we need the right eye
    // to start render work as soon as possible
    if (((m_frame_count + 1) % 2) == m_left_eye_interval || is_using_multipass()) {
        if (WaitForSingleObject(m_present_finished_event, 333) == WAIT_TIMEOUT) {
            timed_out = true;
        }

        ResetEvent(m_present_finished_event);
    }
}

void VR::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    if (!get_runtime()->loaded) {
        return;
    }

    m_camera_duplicator.on_pre_application_entry(entry, name, hash);

    switch (hash) {
        case "UpdateHID"_fnv:
            on_pre_update_hid(entry);
            break;
        case "WaitRendering"_fnv:
            on_pre_wait_rendering(entry);
            break;
        case "BeginRendering"_fnv:
            on_pre_begin_rendering(entry);
            break;
        case "EndRendering"_fnv:
            on_pre_end_rendering(entry);
            break;
        default:
            break;
    }
}

void VR::on_application_entry(void* entry, const char* name, size_t hash) {
    if (!get_runtime()->loaded) {
        return;
    }

    m_camera_duplicator.on_application_entry(entry, name, hash);

    switch (hash) {
        case "UpdateHID"_fnv:
            on_update_hid(entry);
            break;
        case "WaitRendering"_fnv:
            on_wait_rendering(entry);
            break;
        case "BeginRendering"_fnv:
            on_begin_rendering(entry);
            break;
        case "EndRendering"_fnv:
            on_end_rendering(entry);
            break;
        default:
            break;
    }
}

void VR::on_pre_update_hid(void* entry) {
    // Flatscreen 3D is display-only: no motion controllers, input stays
    // game-controlled. Never inject VR input (there is no runtime behind it -
    // driving the RE Engine input reflection here with no controller state
    // walks a degenerate object and recurses until the stack overflows).
    if (!get_runtime()->loaded || !is_hmd_active() || is_using_flat3d()) {
        return;
    }

    update_action_states();
}

void VR::on_update_hid(void* entry) {
    if (!get_runtime()->loaded || !is_hmd_active() || is_using_flat3d()) {
        return;
    }

    {
        const auto& gi = sdk::GameIdentity::get();
        if (!gi.is_re2() && !gi.is_re3()) {
            this->openvr_input_to_re_engine();
        }

        if (gi.is_re8() && get_runtime()->handle_pause) {
            auto padman = sdk::get_managed_singleton<::REManagedObject>(game_namespace("HIDPadManager"));

            if (padman != nullptr) {
                auto merged_pad = sdk::call_object_func_easy<::REManagedObject*>(padman, "get_mergedPad");

                if (merged_pad != nullptr) {
                    auto device = sdk::get_object_field<::REManagedObject*>(merged_pad, "Device");

                    if (device != nullptr && *device != nullptr) {
                        sdk::call_object_func_easy<void*>(*device, "set_Button", via::hid::GamePadButton::CRight);
                    }
                }
            }

            get_runtime()->handle_pause = false;
        }
    }
}

void VR::openvr_input_to_re2_re3(REManagedObject* input_system) {
    if (!get_runtime()->loaded) {
        return;
    }

    const auto& gi = sdk::GameIdentity::get();
    static auto gui_master_type = sdk::find_type_definition(game_namespace("gui.GUIMaster"));
    static auto gui_master_get_instance = gui_master_type->get_method("get_Instance");
    static auto gui_master_get_input = gui_master_type->get_method("get_Input");
    static auto gui_master_input_type = gui_master_get_input->get_return_type();

    // ??????
    static auto input_set_is_trigger_move_up_d = gui_master_input_type->get_method("set_IsTriggerMoveUpD");
    static auto input_set_is_trigger_move_down_d = gui_master_input_type->get_method("set_IsTriggerMoveDownD");

    auto gui_master = gui_master_get_instance->call<::REManagedObject*>(sdk::get_thread_context());
    auto gui_input = gui_master != nullptr ? 
                     gui_master_get_input->call<::REManagedObject*>(sdk::get_thread_context(), gui_master) : 
                     (::REManagedObject*)nullptr;

    auto ctx = sdk::get_thread_context();

    static auto get_lstick_method = sdk::get_object_method(input_system, "get_LStick");
    static auto get_rstick_method = sdk::get_object_method(input_system, "get_RStick");
    auto lstick = get_lstick_method->call<::REManagedObject*>(ctx);
    auto rstick = get_rstick_method->call<::REManagedObject*>(ctx);

    if (lstick == nullptr || rstick == nullptr) {
        return;
    }

    static auto get_button_bits_method = sdk::get_object_method(input_system, "get_ButtonBits");
    auto button_bits_obj = get_button_bits_method->call<::REManagedObject*>(ctx, input_system);

    if (button_bits_obj == nullptr) {
        return;
    }

    static auto get_active_user_input_unit_method = sdk::get_object_method(input_system, "getActiveUserInputUnit");
    auto input_unit_obj = get_active_user_input_unit_method->call<::REManagedObject*>(ctx, input_system);

    if (input_unit_obj == nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    auto is_using_controller = (now - get_last_controller_update()) <= std::chrono::seconds(10);

    const auto is_grip_down = is_action_active(m_action_grip, m_right_joystick);
    const auto is_trigger_down = is_action_active(m_action_trigger, m_right_joystick);
    const auto is_left_grip_down = is_action_active(m_action_grip, m_left_joystick);
    const auto is_left_trigger_down = is_action_active(m_action_trigger, m_left_joystick);
    const auto is_left_joystick_click_down = is_action_active(m_action_joystick_click, m_left_joystick);
    const auto is_right_joystick_click_down = is_action_active(m_action_joystick_click, m_right_joystick);

    const auto is_minimap_down = is_action_active(m_action_minimap, m_left_joystick) || is_action_active(m_action_minimap, m_right_joystick);
    const auto is_left_a_button_down = is_action_active(m_action_a_button, m_left_joystick);
    const auto is_left_b_button_down = !is_minimap_down && is_action_active(m_action_b_button, m_left_joystick);
    const auto is_right_a_button_down = is_action_active(m_action_a_button, m_right_joystick);
    const auto is_right_b_button_down = is_action_active(m_action_b_button, m_right_joystick);

    const auto is_dpad_up_down = is_action_active(m_action_dpad_up, m_left_joystick) || is_action_active(m_action_dpad_up, m_right_joystick);
    const auto is_dpad_right_down = is_action_active(m_action_dpad_right, m_left_joystick) || is_action_active(m_action_dpad_right, m_right_joystick);
    const auto is_dpad_down_down = is_action_active(m_action_dpad_down, m_left_joystick) || is_action_active(m_action_dpad_down, m_right_joystick);
    const auto is_dpad_left_down = is_action_active(m_action_dpad_left, m_left_joystick) || is_action_active(m_action_dpad_left, m_right_joystick);

    const auto is_weapon_dial_down = is_action_active(m_action_weapon_dial, m_left_joystick) || is_action_active(m_action_weapon_dial, m_right_joystick);
    const auto is_re3_dodge_down = is_action_active(m_action_re3_dodge, m_left_joystick) || is_action_active(m_action_re3_dodge, m_right_joystick);
    const auto is_quickturn_down = is_action_active(m_action_re2_quickturn, m_left_joystick) || is_action_active(m_action_re2_quickturn, m_right_joystick);
    const auto is_reset_view_down = is_action_active(m_action_re2_reset_view, m_left_joystick) || is_action_active(m_action_re2_reset_view, m_right_joystick);
    const auto is_change_ammo_down = is_action_active(m_action_re2_change_ammo, m_left_joystick) || is_action_active(m_action_re2_change_ammo, m_right_joystick);
	const auto is_toggle_flashlight_down = is_action_active(m_action_re2_toggle_flashlight, m_left_joystick) || is_action_active(m_action_re2_toggle_flashlight, m_right_joystick);

    const auto is_left_system_button_down = is_action_active(m_action_system_button, m_left_joystick);
    const auto is_right_system_button_down = is_action_active(m_action_system_button, m_right_joystick);

    
    
    if ((gi.is_re2() || gi.is_re3() || gi.is_re8()) && is_toggle_flashlight_down && !m_was_flashlight_toggle_down) {
        ManualFlashlight::g_manual_flashlight->toggle_flashlight();
    }

    if (gi.is_re2() || gi.is_re3() || gi.is_re8()) {
        m_was_flashlight_toggle_down = is_toggle_flashlight_down;
    }


    if (gi.is_re2() || gi.is_re3()) {
        const auto is_firstperson_toggle_down = is_action_active(m_action_re2_firstperson_toggle, m_left_joystick) || is_action_active(m_action_re2_firstperson_toggle, m_right_joystick);

        if (is_firstperson_toggle_down && !m_was_firstperson_toggle_down) {
            FirstPerson::get()->toggle();
        }

        m_was_firstperson_toggle_down = is_firstperson_toggle_down;
    }

    const auto is_gripping_weapon = (gi.is_re2() || gi.is_re3()) ? FirstPerson::get()->was_gripping_weapon() : false;

    // Current actual button bits used by the game
    auto& button_bits_down = *sdk::get_object_field<uint64_t>(button_bits_obj, "Down");
    auto& button_bits_on = *sdk::get_object_field<uint64_t>(button_bits_obj, "On");
    auto& button_bits_up = *sdk::get_object_field<uint64_t>(button_bits_obj, "Up");

    // Set button state based on our own history we keep that doesn't get overwritten by the game
    auto set_button_state = [&](app::ropeway::InputDefine::Kind kind, bool state) {
        const auto kind_uint64 = (uint64_t)kind;

        if (state) {
            m_last_controller_update = now;
            is_using_controller = true;

            button_bits_up &= ~kind_uint64;
            m_button_states_up &= ~kind_uint64;

            // if "On" state is not set
            if ((m_button_states_on.to_ullong() & kind_uint64) == 0) {
                if (m_button_states_down.to_ullong() & kind_uint64) {
                    m_button_states_on |= kind_uint64;
                    m_button_states_down &= ~kind_uint64;

                    button_bits_on |= kind_uint64;
                    button_bits_down &= ~kind_uint64;
                } else {
                    m_button_states_on &= ~kind_uint64;
                    m_button_states_down |= kind_uint64;

                    button_bits_on &= ~kind_uint64;
                    button_bits_down |= kind_uint64;
                }
            } else {
                m_button_states_down &= ~kind_uint64;
                button_bits_down &= ~kind_uint64;

                m_button_states_on |= kind_uint64;
                button_bits_on |= kind_uint64;
            }
        } else {
            if (m_button_states_down.to_ullong() & kind_uint64 || m_button_states_on.to_ullong() & kind_uint64) {
                m_button_states_up |= kind_uint64;
                button_bits_up |= kind_uint64;

                m_last_controller_update = now;
                is_using_controller = true;
            } else if (is_using_controller) {
                m_button_states_up &= ~kind_uint64;
                button_bits_up &= ~kind_uint64;
            }

            // Don't want to screw with the user's input if they aren't actively
            // Using their VR controllers
            if (is_using_controller) {
                button_bits_down &= ~kind_uint64;
                m_button_states_down &= ~kind_uint64;
                
                button_bits_on &= ~kind_uint64;
                m_button_states_on &= ~kind_uint64;
            }
        }
    };

    // Right Grip: Aim, UI Right (RB)
    set_button_state(app::ropeway::InputDefine::Kind::HOLD, is_grip_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_RIGHT, is_grip_down);

    // Left Grip: Alternate aim (grenades, knives, etc), UI left (LB)
    set_button_state(app::ropeway::InputDefine::Kind::SUPPORT_HOLD, is_left_grip_down && !is_gripping_weapon);   
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_LEFT, is_left_grip_down);

    // Right Trigger (RB): Attack, Alternate UI right (RT), GE_RTrigBottom (quick time event), GE_RTrigTop (another quick time event)
    set_button_state(app::ropeway::InputDefine::Kind::ATTACK, is_trigger_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_RIGHT_2, is_trigger_down);
    set_button_state((app::ropeway::InputDefine::Kind)18014398509481984, is_trigger_down);
    set_button_state((app::ropeway::InputDefine::Kind)9007199254740992, is_trigger_down);
    //set_button_state((app::ropeway::InputDefine::Kind)4503599627370496, is_trigger_down);

    // Left Trigger (LB): Alternate UI left (LT), DEFENSE (LB)
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_LEFT_2, is_left_trigger_down);
    set_button_state(app::ropeway::InputDefine::Kind::DEFENSE, is_left_trigger_down);

    // L3: Sprint
    set_button_state(app::ropeway::InputDefine::Kind::JOG1, is_left_joystick_click_down);

    // R3: Reset camera
    set_button_state(app::ropeway::InputDefine::Kind::RESET_CAMERA, is_reset_view_down);

    // Left B: Inventory, PRESS_START
    set_button_state(app::ropeway::InputDefine::Kind::INVENTORY, is_left_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::PRESS_START, is_left_b_button_down);

    // Left A: QUICK_TURN, PRESS_START, CANCEL, DIALOG_CANCEL
    set_button_state(app::ropeway::InputDefine::Kind::QUICK_TURN, is_quickturn_down); // unique, unbound by default as it causes issues
    set_button_state(app::ropeway::InputDefine::Kind::PRESS_START, is_left_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::CANCEL, is_left_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::DIALOG_CANCEL, is_left_a_button_down);
    
    // Right A: Action, ITEM, PRESS_START, DECIDE, DIALOG_DECIDE, (1 << 51)
    set_button_state(app::ropeway::InputDefine::Kind::ACTION, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::ITEM, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::PRESS_START, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::DECIDE, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::DIALOG_DECIDE, is_right_a_button_down);

    // only allow dodging if there's nothing to interact with nearby
    if (!is_right_a_button_down || (now - m_last_interaction_display) >= std::chrono::milliseconds(200)) {
        set_button_state((app::ropeway::InputDefine::Kind)((uint64_t)1 << 51), is_re3_dodge_down); // RE3 dodge
    } else {
        set_button_state((app::ropeway::InputDefine::Kind)((uint64_t)1 << 51), false); // RE3 dodge
    }
    
    // Right B: Reload, Skip Event, UI_EXCHANGE, UI_RESET, (1 << 52) (that one is RE3 only? don't see it in the enum)
    set_button_state(app::ropeway::InputDefine::Kind::RELOAD, is_right_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::SKIP_EVENT, is_right_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_EXCHANGE, is_right_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_RESET, is_right_b_button_down);
    set_button_state((app::ropeway::InputDefine::Kind)((uint64_t)1 << 52), is_right_b_button_down);

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();
    const auto left_axis_len = glm::length(left_axis);
    const auto right_axis_len = glm::length(right_axis);

    if (!is_weapon_dial_down) {
        // DPad Up: Shortcut Up
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_UP, is_dpad_up_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_MAP_UP, is_dpad_up_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_UP, is_dpad_up_down); // IsTriggerMoveUpD in RE3

        // DPad Right: Shortcut Right
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_RIGHT, is_dpad_right_down);

        // DPad Down: Shortcut Down
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_DOWN, is_dpad_down_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_MAP_DOWN, is_dpad_down_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_DOWN, is_dpad_down_down); // IsTriggerMoveDownD in RE3

        // DPad Left: Shortcut Left
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_LEFT, is_dpad_left_down);

        // well this was really annoying to figure out
        if (gi.is_re2()) {
            if (is_dpad_up_down && gui_input != nullptr) {
                input_set_is_trigger_move_up_d->call<void*>(ctx, gui_input, true);
            }

            if (is_dpad_down_down && gui_input != nullptr) {
                input_set_is_trigger_move_down_d->call<void*>(ctx, gui_input, true);
            }
        }
    } else {
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_UP, left_axis.y > 0.9f);
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_RIGHT, left_axis.x > 0.9f);
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_DOWN, left_axis.y < -0.9f);
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_LEFT, left_axis.x < -0.9f);
    }

    // Change Ammo
    set_button_state(app::ropeway::InputDefine::Kind::CHANGE_BULLET, is_change_ammo_down);

    // MiniMap
    set_button_state(app::ropeway::InputDefine::Kind::MINIMAP, is_minimap_down);

    // Left or Right System Button: Pause
    set_button_state(app::ropeway::InputDefine::Kind::PAUSE, is_left_system_button_down || is_right_system_button_down || get_runtime()->handle_pause);
    get_runtime()->handle_pause = false;

    // Fixes QTE bound to triggers
    if (is_using_controller) {
        const auto left_trigger_state = is_left_trigger_down ? 1.0f : 0.0f;
        const auto right_trigger_state = is_trigger_down ? 1.0f : 0.0f;

        static auto set__analog_l_method = sdk::get_object_method(input_unit_obj, "set__AnalogL");
        static auto set__analog_r_method = sdk::get_object_method(input_unit_obj, "set__AnalogR");
        set__analog_l_method->call<void*>(ctx, input_unit_obj, left_trigger_state);
        set__analog_r_method->call<void*>(ctx, input_unit_obj, right_trigger_state);
    }

    bool moved_sticks = false;
    const auto deadzone = m_joystick_deadzone->value();

    if (left_axis_len > deadzone) {
        moved_sticks = true;

        // Override the left stick's axis values to the VR controller's values
        Vector3f axis{ left_axis.x, left_axis.y, 0.0f };

        static auto update_method = sdk::get_object_method(lstick, "update");
        update_method->call<void*>(ctx, lstick, &axis, &axis);
    }

    if (right_axis_len > deadzone) {
        moved_sticks = true;

        // Override the right stick's axis values to the VR controller's values
        Vector3f axis{ right_axis.x, right_axis.y, 0.0f };

        static auto update_method = sdk::get_object_method(rstick, "update");
        update_method->call<void*>(ctx, rstick, &axis, &axis);
    }

    if (moved_sticks) {
        auto new_pos = get_position(vr::k_unTrackedDeviceIndex_Hmd);

        const auto delta = sdk::Application::get()->get_delta_time();
        const auto highest_length = std::max<float>(glm::length(right_axis), glm::length(left_axis));

        new_pos.y = m_standing_origin.y;
        // Don't set the Y because it would look really strange
        m_standing_origin = glm::lerp(m_standing_origin, new_pos, ((float)highest_length * delta) * 0.01f);
    }

    set_button_state(app::ropeway::InputDefine::Kind::MOVE, left_axis_len > deadzone);
    set_button_state(app::ropeway::InputDefine::Kind::UI_L_STICK, left_axis_len > deadzone);

    set_button_state(app::ropeway::InputDefine::Kind::WATCH, right_axis_len > deadzone);
    set_button_state(app::ropeway::InputDefine::Kind::UI_R_STICK, right_axis_len > deadzone);
    //set_button_state(app::ropeway::InputDefine::Kind::RUN, right_axis_len > 0.01f);

    // Causes the right stick to take effect properly
    if (is_using_controller) {
        static auto set_input_mode_method = sdk::get_object_method(input_system, "set_InputMode");
        set_input_mode_method->call<void*>(ctx, input_system, app::ropeway::InputDefine::InputMode::Pad);
    }
}

void VR::openvr_input_to_re_engine() {
    // TODO: Get the "merged pad" and actually modify some inputs!

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();
    const auto left_axis_len = glm::length(left_axis);
    const auto right_axis_len = glm::length(right_axis);
    const auto now = std::chrono::steady_clock::now();

    bool moved_sticks = false;

    if (left_axis_len > m_joystick_deadzone->value()) {
        moved_sticks = true;
    }

    if (right_axis_len > m_joystick_deadzone->value()) {
        moved_sticks = true;
    }

    if (moved_sticks) {
        auto new_pos = get_position(vr::k_unTrackedDeviceIndex_Hmd);

        const auto delta = sdk::Application::get()->get_delta_time();
        const auto highest_length = std::max<float>(glm::length(right_axis), glm::length(left_axis));

        new_pos.y = m_standing_origin.y;
        // Don't set the Y because it would look really strange
        m_standing_origin = glm::lerp(m_standing_origin, new_pos, ((float)highest_length * delta) * 0.01f);
        m_last_controller_update = now;
    }

    for (auto& it : m_action_handles) {
        if (is_action_active(it.second, m_left_joystick) || is_action_active(it.second, m_right_joystick)) {
            m_last_controller_update = now;
        }
    }
}

void VR::on_draw_ui() {
    // create VR tree entry in menu (imgui)
    if (get_runtime()->loaded) {
        ImGui::SetNextItemOpen(m_has_hw_scheduling, ImGuiCond_::ImGuiCond_FirstUseEver);
    } else {
        if (m_openvr->error && !m_openvr->dll_missing) {
            ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_FirstUseEver);
        } else {
            ImGui::SetNextItemOpen(false, ImGuiCond_::ImGuiCond_FirstUseEver);
        }
    }

    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    auto display_error = [](auto& runtime, std::string dll_name) {
        if (runtime == nullptr || !runtime->error && runtime->loaded) {
            return;
        }

        if (runtime->error && runtime->dll_missing) {
            ImGui::TextWrapped("%s not loaded: %s not found", runtime->name().data(), dll_name.data());
            ImGui::TextWrapped("Please drop the %s file into the game's directory if you want to use %s", dll_name.data(), runtime->name().data());
        } else if (runtime->error) {
            ImGui::TextWrapped("%s not loaded: %s", runtime->name().data(), runtime->error->c_str());
        } else {
            ImGui::TextWrapped("%s not loaded: Unknown error", runtime->name().data());
        }

        ImGui::Separator();
    };

    display_error(m_openxr, "openxr_loader.dll");
    display_error(m_openvr, "openvr_api.dll");

    if (!get_runtime()->loaded) {
        ImGui::TextWrapped("No runtime loaded.");
        return;
    }

    ImGui::TextWrapped("Hardware scheduling: %s", m_has_hw_scheduling ? "Enabled" : "Disabled");

    if (m_has_hw_scheduling) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
        ImGui::TextWrapped("WARNING: Hardware-accelerated GPU scheduling is enabled. This will cause the game to run slower.");
        ImGui::TextWrapped("Go into your Windows Graphics settings and disable \"Hardware-accelerated GPU scheduling\"");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    ImGui::TextWrapped("VR Runtime: %s", get_runtime()->name().data());
    ImGui::TextWrapped("Render Resolution: %d x %d", get_runtime()->get_width(), get_runtime()->get_height());

    if (get_runtime()->is_flat3d()) {
        draw_flat3d_ui();
        return;
    }

    if (get_runtime()->is_openvr()) {
        ImGui::TextWrapped("Resolution can be changed in SteamVR");
    } else if (get_runtime()->is_openxr()) {
        if (ImGui::TreeNode("Bindings")) {
            m_openxr->display_bindings_editor();
            ImGui::TreePop();
        }

        if (m_resolution_scale->draw("Resolution Scale")) {
            m_openxr->resolution_scale = m_resolution_scale->value();
        }
    }
    
    ImGui::Combo("Sync Mode", (int*)&get_runtime()->custom_stage, "Early\0Late\0Very Late\0");
    ImGui::Separator();

    if (ImGui::Button("Set Standing Height")) {
        m_standing_origin.y = get_position(0).y;
    }

    if (ImGui::Button("Set Standing Origin") || m_set_standing_key->is_key_down_once()) {
        m_standing_origin = get_position(0);
    }

    if (ImGui::Button("Recenter View") || m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }

    if (ImGui::Button("Reinitialize Runtime")) {
        get_runtime()->wants_reinitialize = true;
    }

    //ImGui::DragFloat4("Right Bounds", (float*)&m_right_bounds, 0.005f, -2.0f, 2.0f);
    //ImGui::DragFloat4("Left Bounds", (float*)&m_left_bounds, 0.005f, -2.0f, 2.0f);

    ImGui::Separator();

    m_set_standing_key->draw("Set Standing Origin Key");
    m_recenter_view_key->draw("Recenter View Key");

    ImGui::Separator();

    m_rendering_technique->draw("Rendering Technique");
    m_decoupled_pitch->draw("Decoupled Camera Pitch");

    if (ImGui::Checkbox("Positional Tracking", &m_positional_tracking)) {
    }

    m_hmd_oriented_audio->draw("Head Oriented Audio");
    m_use_custom_view_distance->draw("Use Custom View Distance");
    m_view_distance->draw("View Distance/FarZ");
    m_motion_controls_inactivity_timer->draw("Inactivity Timer");
    m_joystick_deadzone->draw("Joystick Deadzone");

    m_ui_scale_option->draw("2D UI Scale");
    m_ui_distance_option->draw("2D UI Distance");
    m_world_ui_scale_option->draw("World-Space UI Scale");

    ImGui::DragFloat3("Overlay Rotation", (float*)&m_overlay_rotation, 0.01f, -360.0f, 360.0f);
    ImGui::DragFloat3("Overlay Position", (float*)&m_overlay_position, 0.01f, -100.0f, 100.0f);

    ImGui::Separator();
    ImGui::Text("Graphical Options");

    m_force_fps_settings->draw("Force Uncap FPS");
    m_force_aa_settings->draw("Force Disable TAA");
    m_force_motionblur_settings->draw("Force Disable Motion Blur");
    m_force_vsync_settings->draw("Force Disable V-Sync");
    m_force_lensdistortion_settings->draw("Force Disable Lens Distortion");
    m_force_volumetrics_settings->draw("Force Disable Volumetrics");
    m_force_lensflares_settings->draw("Force Disable Lens Flares");
    m_force_dynamic_shadows_settings->draw("Force Enable Dynamic Shadows");
    m_allow_engine_overlays->draw("Allow Engine Overlays");
    m_enable_asynchronous_rendering->draw("Enable Asynchronous Rendering");

    if (ImGui::TreeNode("Desktop Recording Fix")) {
        ImGui::PushID("Desktop");
        m_desktop_fix->draw("Enabled");
        m_desktop_fix_skip_present->draw("Skip Present");
        ImGui::PopID();
        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::Text("Debug info");
    m_camera_duplicator.on_draw_ui();
    

    ImGui::Checkbox("Disable Projection Matrix Override", &m_disable_projection_matrix_override);
    ImGui::Checkbox("Disable GUI Projection Matrix Override", &m_disable_gui_camera_projection_matrix_override);
    ImGui::Checkbox("Disable View Matrix Override", &m_disable_view_matrix_override);
    ImGui::Checkbox("Disable Backbuffer Size Override", &m_disable_backbuffer_size_override);
    ImGui::Checkbox("Disable Temporal Fix", &m_disable_temporal_fix);
    ImGui::Checkbox("Disable Post Effect Fix", &m_disable_post_effect_fix);
    
    const double min_ = 0.0;
    const double max_ = 25.0;
    ImGui::SliderScalar("Prediction Scale", ImGuiDataType_Double, &m_openxr->prediction_scale, &min_, &max_);

    ImGui::DragFloat4("Raw Left", (float*)&m_raw_projections[0], 0.01f, -100.0f, 100.0f);
    ImGui::DragFloat4("Raw Right", (float*)&m_raw_projections[1], 0.01f, -100.0f, 100.0f);

    // convert m_avg_input_delay (std::chrono::nanoseconds) to milliseconds (float)
    auto duration_float = std::chrono::duration<float, std::milli>(m_avg_input_delay).count();

    ImGui::DragFloat("Avg Input Processing Delay (MS)", &duration_float, 0.00001f);
}

void VR::draw_flat3d_ui() {
    ImGui::Separator();
    ImGui::Text("Flatscreen 3D");

    m_flat3d_output_mode->draw("Output Mode");

    // Pixel-exactness warning: interlaced/checkerboard/LeiaSR need the backbuffer 1:1 on the panel.
    if (m_flat3d_native_mismatch.load()) {
        const auto mode = (Flat3DOutputMode)m_flat3d_output_mode->value();
        if (mode == FLAT3D_ROW_INTERLACED || mode == FLAT3D_COLUMN_INTERLACED
                || mode == FLAT3D_CHECKERBOARD || mode == FLAT3D_LEIA_SR) {
            ImGui::TextColored(ImVec4{1.0f, 0.8f, 0.1f, 1.0f},
                "Warning: output is not display-native - this mode's pixel pattern will be rescaled\n"
                "and broken. Set the game resolution to the desktop resolution (use DLSS render\n"
                "scaling for performance instead).");
        }
    }

    const auto mode = m_flat3d_output_mode->value();

    if (mode == FLAT3D_ROW_INTERLACED || mode == FLAT3D_COLUMN_INTERLACED || mode == FLAT3D_CHECKERBOARD) {
        // Interlaced/checkerboard patterns must be display-pixel-exact: warn
        // when the backbuffer is not the same size as the display showing it.
        static std::chrono::steady_clock::time_point s_last_check{};
        static bool s_display_native = true;

        const auto now = std::chrono::steady_clock::now();

        if (now - s_last_check >= std::chrono::seconds(2)) {
            s_last_check = now;

            IDXGISwapChain* swapchain = nullptr;

            if (g_framework->is_dx12()) {
                if (auto& hook = g_framework->get_d3d12_hook(); hook != nullptr) {
                    swapchain = hook->get_swap_chain();
                }
            } else {
                if (auto& hook = g_framework->get_d3d11_hook(); hook != nullptr) {
                    swapchain = hook->get_swap_chain();
                }
            }

            if (swapchain != nullptr) {
                DXGI_SWAP_CHAIN_DESC desc{};
                ComPtr<IDXGIOutput> output{};

                if (SUCCEEDED(swapchain->GetDesc(&desc)) && SUCCEEDED(swapchain->GetContainingOutput(&output))) {
                    DXGI_OUTPUT_DESC out_desc{};

                    if (SUCCEEDED(output->GetDesc(&out_desc))) {
                        const auto out_w = (uint32_t)(out_desc.DesktopCoordinates.right - out_desc.DesktopCoordinates.left);
                        const auto out_h = (uint32_t)(out_desc.DesktopCoordinates.bottom - out_desc.DesktopCoordinates.top);
                        s_display_native = desc.BufferDesc.Width == out_w && desc.BufferDesc.Height == out_h;
                    }
                }
            }
        }

        if (!s_display_native) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            ImGui::TextWrapped("WARNING: the game window is not display-native. Interlaced/checkerboard patterns will not line up with the panel; use borderless fullscreen at the display's native resolution.");
            ImGui::PopStyleColor();
        }
    }

    m_flat3d_eye_swap->draw("Swap Eyes");
    m_flat3d_depth->draw("Depth (Separation)");
    m_flat3d_convergence->draw("Convergence");
    m_flat3d_reference_fov->draw("Reference FOV");

    // Live game FOV readout (recorded from the projection hook; vfov = 2*atan(1/P11)).
    {
        const auto p00 = m_flat3d_game_p00.load();
        const auto p11 = m_flat3d_game_p11.load();

        if (p00 > 0.0f && p11 > 0.0f) {
            const auto vfov = glm::degrees(2.0f * std::atan(1.0f / p11));
            const auto hfov = glm::degrees(2.0f * std::atan(1.0f / p00));
            ImGui::TextDisabled("Game FOV now: %.1f deg vertical / %.1f deg horizontal", vfov, hfov);
        } else {
            ImGui::TextDisabled("Game FOV now: (no projection recorded yet)");
        }
    }

    m_flat3d_crop_eyes_169->draw("Crop Eyes to 16:9 (SbS/TaB)");

    ImGui::TextDisabled("AFW plugin: %s", m_flat3d_afw.status());
    m_flat3d_afw_enabled->draw("AFW: warp missing AFR eye (needs AFR technique + plugin)");
    if (m_flat3d_afw_enabled->value() && m_flat3d_afw.is_available()) {
        m_flat3d_afw_mode->draw("  AFW: warp mode (halo/noise A/B)");
        m_flat3d_afw_depth_dilation->draw("  AFW: depth edge dilation px (silhouette halo fix; 0 = off)");
        m_flat3d_afw_obj_motion->draw("  AFW: warp object motion (movers anti-stutter; 0 = off)");
        if (m_flat3d_afw_obj_motion->value() > 0.0f) {
            m_flat3d_afw_motion_thresh->draw("  AFW: motion threshold px (foliage gate for object motion)");
        }
        m_flat3d_afw_debug->draw("  AFW: debug logging (MV bursts, readbacks)");
        m_flat3d_afw_plugin_debug->draw("  AFW: plugin debug view (changes rendering)");
    }

    m_ui_distance_option->draw("GUI depth (m)");

    if (ImGui::TreeNode("Auto-Convergence & Crosshair")) {
        m_flat3d_auto_convergence->draw("Auto-Convergence");
        m_flat3d_max_popout->draw("Max Pop-out (% of width)");
        m_flat3d_autoconv_smoothing->draw("Auto-Convergence Smoothing");
        m_flat3d_dynamic_crosshair->draw("Dynamic Crosshair");
        m_flat3d_crosshair_depth->draw("Crosshair Fallback Depth (m)");
        m_flat3d_swap_shift_sign->draw("Swap Crosshair Shift Sign");

        const auto nearest = m_flat3d_depth_sampler.get_nearest_depth();
        const auto center = m_flat3d_depth_sampler.get_center_depth();

        if (nearest > 0.0f || center > 0.0f) {
            ImGui::Text("Depth samples: nearest %.2f m, center %.2f m", nearest, center);
            ImGui::Text("Applied: convergence %.3f m, separation %.4f m", m_flat3d->convergence, m_flat3d->separation_eff);
        } else if (m_flat3d_auto_convergence->value() || m_flat3d_dynamic_crosshair->value()) {
            ImGui::TextWrapped("No depth samples yet (depth buffer not located or readback pending).");
        }

        ImGui::TreePop();
    }

    ImGui::Separator();

    m_rendering_technique->draw("Rendering Technique");
    m_use_custom_view_distance->draw("Use Custom View Distance");
    m_view_distance->draw("View Distance/FarZ");

    ImGui::Separator();
    ImGui::Text("Graphical Options");

    m_force_fps_settings->draw("Force Uncap FPS");
    m_force_aa_settings->draw("Force Disable TAA");
    m_force_motionblur_settings->draw("Force Disable Motion Blur");
    m_force_vsync_settings->draw("Force Disable V-Sync");
    m_force_lensdistortion_settings->draw("Force Disable Lens Distortion");
    m_force_volumetrics_settings->draw("Force Disable Volumetrics");
    m_force_lensflares_settings->draw("Force Disable Lens Flares");
    m_force_dynamic_shadows_settings->draw("Force Enable Dynamic Shadows");
    m_enable_asynchronous_rendering->draw("Enable Asynchronous Rendering");

    ImGui::Separator();
    ImGui::Text("Debug info");
    m_camera_duplicator.on_draw_ui();

    m_flat3d_swap_shear_sign->draw("Swap Shear Sign (use if 3D is inverted)");
    ImGui::Checkbox("Disable Projection Matrix Override", &m_disable_projection_matrix_override);
    ImGui::Checkbox("Disable GUI Projection Matrix Override", &m_disable_gui_camera_projection_matrix_override);
    ImGui::Checkbox("Disable View Matrix Override", &m_disable_view_matrix_override);
    ImGui::Checkbox("Disable Temporal Fix", &m_disable_temporal_fix);
    ImGui::Checkbox("Disable Post Effect Fix", &m_disable_post_effect_fix);
}

void VR::on_device_reset() {
    std::scoped_lock _{m_openxr->sync_mtx};

    m_multipass.eye_textures[0] = nullptr;
    m_multipass.eye_textures[1] = nullptr;

    // Drop the flat3d redirect resources so they get re-cloned at the new
    // resolution/format (they're sized from the engine's real output target).
    m_multipass.flat3d_output_clones[0] = nullptr;
    m_multipass.flat3d_output_clones[1] = nullptr;
    m_multipass.flat3d_eye_rtvs[0] = nullptr;
    m_multipass.flat3d_eye_rtvs[1] = nullptr;
    m_multipass.flat3d_eye_textures[0] = nullptr;
    m_multipass.flat3d_eye_textures[1] = nullptr;

    m_flat3d_depth_sampler.reset();

    spdlog::info("VR: on_device_reset");
    m_backbuffer_inconsistency = false;
    if (g_framework->is_dx11()) {
        m_d3d11.on_reset(this);
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    }

    m_overlay_component.on_reset();

    // so i guess device resets can happen between begin and end rendering...
    if (m_in_render) {
        spdlog::info("VR: on_device_reset: in_render");

        // what the fuck
        if (m_needs_camera_restore) {
            spdlog::info("VR: on_device_reset: needs_camera_restore");
            restore_camera();
        }
    }

    if (inside_on_end) {
        spdlog::info("VR: on_device_reset: inside_on_end");
    }
}

void VR::on_config_load(const utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_load(cfg);
    }

    if (m_flat3d != nullptr) {
        m_flat3d->enabled = true; // stereo always active (the Active toggle is retired)
    }

    // Run the rest of OpenXR initialization code here that depends on config values
    if (get_runtime()->is_openxr() && get_runtime()->loaded) {
        spdlog::info("[VR] Finishing up OpenXR initialization");

        m_openxr->resolution_scale = m_resolution_scale->value();
        initialize_openxr_swapchains();
    }

    if (m_motion_controls_inactivity_timer->value() <= 10.0f) {
        m_motion_controls_inactivity_timer->value() = 30.0f;
    }
}

void VR::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

std::array<RECamera*, 2> VR::get_cameras() const {
    if (is_using_multipass()) {
        return m_multipass_cameras;
    }

    return std::array<RECamera*, 2>{ sdk::get_primary_camera(), nullptr };
}

Vector4f VR::get_position(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };
    std::shared_lock __{ get_runtime()->eyes_mtx };

    return get_position_unsafe(index);
}

Vector4f VR::get_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_velocity_unsafe(index);
}

Vector4f VR::get_angular_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_angular_velocity_unsafe(index);
}

Vector4f VR::get_position_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        auto result = glm::rowMajor4(matrix)[3];
        result.w = 1.0f;

        return result;
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // HMD position
        if (index == 0 && !m_openxr->stage_views.empty()) {
            return Vector4f{ *(Vector3f*)&m_openxr->view_space_location.pose.position, 1.0f };
        } else if (index > 0) {
            return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].location.pose.position, 1.0f };
        }

        return Vector4f{};
    } 

    return Vector4f{};
}

Vector4f VR::get_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& velocity = pose.vVelocity;

        return Vector4f{ velocity.v[0], velocity.v[1], velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }

        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].velocity.linearVelocity, 0.0f };
    }

    return Vector4f{};
}

Vector4f VR::get_angular_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& angular_velocity = pose.vAngularVelocity;

        return Vector4f{ angular_velocity.v[0], angular_velocity.v[1], angular_velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }
    
        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].velocity.angularVelocity, 0.0f };
    }

    return Vector4f{};
}

Matrix4x4f VR::get_rotation(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        return glm::extractMatrixRotation(glm::rowMajor4(matrix));
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };
        std::shared_lock __{ get_runtime()->eyes_mtx };

        // HMD rotation
        if (index == 0 && !m_openxr->stage_views.empty()) {
            return Matrix4x4f{*(glm::quat*)&m_openxr->view_space_location.pose.orientation};
            //return Matrix4x4f{*(glm::quat*)&m_openxr->stage_views[0].pose.orientation};
        } else if (index > 0) {
            if (index == VRRuntime::Hand::LEFT+1) {
                return Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.orientation};
            } else if (index == VRRuntime::Hand::RIGHT+1) {
                return Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.orientation};
            }
        }

        return glm::identity<Matrix4x4f>();
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        return glm::rowMajor4(matrix);
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };

        // HMD rotation
        if (index == 0 && !m_openxr->stage_views.empty()) {
            auto mat = Matrix4x4f{*(glm::quat*)&m_openxr->view_space_location.pose.orientation};
            mat[3] = Vector4f{*(Vector3f*)&m_openxr->view_space_location.pose.position, 1.0f};
            return mat;
        } else if (index > 0) {
            if (index == VRRuntime::Hand::LEFT+1) {
                auto mat = Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.orientation};
                mat[3] = Vector4f{*(Vector3f*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.position, 1.0f};
                return mat;
            } else if (index == VRRuntime::Hand::RIGHT+1) {
                auto mat = Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.orientation};
                mat[3] = Vector4f{*(Vector3f*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.position, 1.0f};
                return mat;
            }
        }
    }

    return glm::identity<Matrix4x4f>();
}

vr::HmdMatrix34_t VR::get_raw_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return vr::HmdMatrix34_t{};
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        return pose.mDeviceToAbsoluteTracking;
    } else {
        spdlog::error("VR: get_raw_transform: not implemented for {}", get_runtime()->name());
        return vr::HmdMatrix34_t{};
    }
}

bool VR::is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source) const {
    if (!get_runtime()->loaded) {
        return false;
    }
    
    bool active = false;

    if (get_runtime()->is_openvr()) {
        vr::InputDigitalActionData_t data{};
        vr::VRInput()->GetDigitalActionData(action, &data, sizeof(data), source);

        active = data.bActive && data.bState;
    } else if (get_runtime()->is_openxr()) {
        active = m_openxr->is_action_active((XrAction)action, (VRRuntime::Hand)source);
    }

    if (!active && action == m_action_minimap) {
        active = is_action_active(m_action_b_button, m_left_joystick) && is_hand_behind_head(VRRuntime::Hand::LEFT);
    }

    return active;
}

Vector2f VR::get_joystick_axis(vr::VRInputValueHandle_t handle) const {
    if (!get_runtime()->loaded) {
        return Vector2f{};
    }

    if (get_runtime()->is_openvr()) {
        vr::InputAnalogActionData_t data{};
        vr::VRInput()->GetAnalogActionData(m_action_joystick, &data, sizeof(data), handle);

        const auto deadzone = m_joystick_deadzone->value();
        const auto out = Vector2f{ data.x, data.y };

        return glm::length(out) > deadzone ? out : Vector2f{};
    } else if (get_runtime()->is_openxr()) {
        if (handle == (vr::VRInputValueHandle_t)VRRuntime::Hand::LEFT) {
            auto out = m_openxr->get_left_stick_axis();
            return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
        } else if (handle == (vr::VRInputValueHandle_t)VRRuntime::Hand::RIGHT) {
            auto out = m_openxr->get_right_stick_axis();
            return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
        }
    }

    return Vector2f{};
}

Vector2f VR::get_left_stick_axis() const {
    return get_joystick_axis(m_left_joystick);
}

Vector2f VR::get_right_stick_axis() const {
    return get_joystick_axis(m_right_joystick);
}

void VR::trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source) {
    if (!get_runtime()->loaded || !is_using_controllers()) {
        return;
    }

    if (get_runtime()->is_openvr()) {
        vr::VRInput()->TriggerHapticVibrationAction(m_action_haptic, seconds_from_now, duration, frequency, amplitude, source);
    } else if (get_runtime()->is_openxr()) {
        m_openxr->trigger_haptic_vibration(duration, frequency, amplitude, (VRRuntime::Hand)source);
    }
}
