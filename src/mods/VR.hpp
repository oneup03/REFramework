#pragma once

#include <atomic>
#include <chrono>
#include <bitset>
#include <memory>
#include <shared_mutex>

#include <openvr.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>

#include "sdk/GameIdentity.hpp"
#include "utility/Patch.hpp"
#include "sdk/Math.hpp"
#include "sdk/helpers/NativeObject.hpp"
#include "sdk/Renderer.hpp"
#include "sdk/intrusive_ptr.hpp"
#include "vr/D3D11Component.hpp"
#include "vr/D3D12Component.hpp"
#include "vr/Flat3DDepth.hpp"
#include "vr/Flat3DAFW.hpp"
#include "vr/OverlayComponent.hpp"
#include "vr/runtimes/OpenXR.hpp"
#include "vr/runtimes/OpenVR.hpp"
#include "vr/runtimes/Flat3D.hpp"
#include "vr/CameraDuplicator.hpp"

#include "HookManager.hpp"

#include "Mod.hpp"

class REManagedObject;

class VR : public Mod {
public:
    enum RenderingTechnique {
        ALTERNATING, // AFR
        SEQUENTIAL_FRAME, // Two frames, synchronized
        MULTIPASS // Native stereo rendering, single frame
    };

    // Flatscreen 3D output formats. Combo order; translated to compose-shader
    // mode constants in Flat3DCompose.
    enum Flat3DOutputMode : int32_t {
        FLAT3D_SBS,
        FLAT3D_TAB,
        FLAT3D_ROW_INTERLACED,
        FLAT3D_COLUMN_INTERLACED,
        FLAT3D_CHECKERBOARD,
        FLAT3D_LEIA_SR,
        FLAT3D_ANAGLYPH_RC,
        FLAT3D_ANAGLYPH_RC_DUBOIS,
        FLAT3D_ANAGLYPH_RC_HALFCOLOR,
        FLAT3D_ANAGLYPH_GM,
        FLAT3D_ANAGLYPH_GM_DUBOIS,
        FLAT3D_ANAGLYPH_BLUE_AMBER,
        FLAT3D_DEBUG_LEFT_ONLY,
        FLAT3D_DEBUG_RIGHT_ONLY,
    };

public:
    static std::shared_ptr<VR>& get();

public:
    std::string_view get_name() const override { return "VR"; }

    // Called when the mod is initialized
    std::optional<std::string> on_initialize_d3d_thread() override;

    void on_lua_state_created(sol::state& lua) override;

    void on_pre_imgui_frame() override;
    void on_present() override;
    void on_post_present() override;
    void on_update_transform(RETransform* transform) override;
    void on_update_camera_controller(RopewayPlayerCameraController* controller) override;
    bool on_pre_gui_draw_element(REComponent* gui_element, void* primitive_context) override;
    void on_gui_draw_element(REComponent* gui_element, void* primitive_context) override;
    void on_pre_update_before_lock_scene(void* ctx) override;
    void on_pre_lightshaft_draw(void* shaft, void* render_context) override;
    void on_lightshaft_draw(void* shaft, void* render_context) override;

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    void on_draw_ui() override;
    void on_device_reset() override;

    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;

    // Application entries
    void on_pre_update_hid(void* entry);
    void on_update_hid(void* entry);
    void on_pre_begin_rendering(void* entry);
    void on_begin_rendering(void* entry);
    void on_pre_end_rendering(void* entry);
    void on_end_rendering(void* entry);
    void on_pre_wait_rendering(void* entry);
    void on_wait_rendering(void* entry);

    template<typename T = VRRuntime>
    T* get_runtime() const {
        return (T*)m_runtime.get();
    }

    auto get_hmd() const {
        return m_openvr->hmd;
    }

    auto& get_openvr_poses() const {
        return m_openvr->render_poses;
    }

    auto get_hmd_width() const {
        return get_runtime()->get_width();
    }

    auto get_hmd_height() const {
        return get_runtime()->get_height();
    }

    auto get_last_controller_update() const {
        return m_last_controller_update;
    }

    int32_t get_frame_count() const;
    int32_t get_game_frame_count() const;
    int32_t get_render_frame_count() const {
        return m_render_frame_count;
    }

    bool is_using_afr() const {
        return m_rendering_technique->value() == RenderingTechnique::ALTERNATING;
    }

    bool is_using_multipass() const {
        return m_rendering_technique->value() == RenderingTechnique::MULTIPASS;
    }

    bool is_using_sequential() const {
        return m_rendering_technique->value() == RenderingTechnique::SEQUENTIAL_FRAME;
    }

    // True-sequential harvest for flat3d: main render pass = left eye, engine re-run = right eye,
    // both at the same game tick (the re-run strips physics/anim/logic entries). Lets us reuse the
    // multipass clone/harvest/tile-map plumbing for the sequential path.
    bool is_using_flat3d_true_sequential() const {
        return false; // flat3d sequential removed (structural squish + <1/2 perf); AFR+AFW is the path
    }

    // AFW (alternate frame warp): AFR renders ONE eye per frame from the single real camera (full
    // post-process - no clone-camera color divergence), and the missing eye is synthesized by the
    // PDAFWPlugin from this frame's depth + motion vectors. Rides the AFR technique.
    bool is_using_flat3d_afw() const {
        return is_using_flat3d() && is_using_afr() && m_flat3d_afw_enabled->value() && m_flat3d_afw.is_available();
    }

    // Accessors for the NGX (DLSS) harvest hook (lives outside the class).
    vrmod::Flat3DAFW& get_flat3d_afw() { return m_flat3d_afw; }
    int32_t get_left_eye_interval() const { return m_left_eye_interval; }

    // Eye/pass index used by the projection/view hooks. Multipass keys off the render pass; flat3d
    // true-sequential keys off inside_on_end (main pass=left(0), engine re-run=right(1)); everything
    // else (AFR / VR sequential) keys off frame parity. Defined in VR.cpp (needs inside_on_end).
    uint32_t get_eye_pass_index() const;

    RenderingTechnique get_rendering_technique() const {
        return (RenderingTechnique)m_rendering_technique->value();
    }

    // Functions that generally use a mutex or have more complex logic
    float get_standing_height();
    Vector4f get_standing_origin();
    void set_standing_origin(const Vector4f& origin);

    glm::quat get_rotation_offset();
    void set_rotation_offset(const glm::quat& offset);
    void recenter_view();

    glm::quat get_gui_rotation_offset();
    void set_gui_rotation_offset(const glm::quat& offset);
    void recenter_gui(const glm::quat& from);

    Vector4f get_current_offset();

    Matrix4x4f get_current_eye_transform(bool flip = false);
    Matrix4x4f get_current_projection_matrix(bool flip = false);
    Matrix4x4f get_projection_matrix(uint32_t pass);
    Matrix4x4f get_eye_transform(uint32_t pass);

    auto& get_controllers() const {
        return m_controllers;
    }

    bool is_using_controllers() const {
        return !m_controllers.empty() && (std::chrono::steady_clock::now() - m_last_controller_update) <= std::chrono::seconds((int32_t)m_motion_controls_inactivity_timer->value());
    }

    bool is_hmd_active() const {
        return get_runtime()->ready();
    }

    bool is_openvr_loaded() const {
        return m_openvr != nullptr && m_openvr->loaded;
    }

    bool is_openxr_loaded() const {
        return m_openxr != nullptr && m_openxr->loaded;
    }

    bool is_flat3d_loaded() const {
        return m_flat3d != nullptr && m_flat3d->loaded;
    }

    bool is_using_flat3d() const {
        return get_runtime()->is_flat3d();
    }


    bool is_using_hmd_oriented_audio() {
        return m_hmd_oriented_audio->value();
    }

    void toggle_hmd_oriented_audio() {
        m_hmd_oriented_audio->toggle();
    }

    const Matrix4x4f& get_last_render_matrix() {
        return m_render_camera_matrix;
    }

    Vector4f get_position(uint32_t index)  const;
    Vector4f get_velocity(uint32_t index)  const;
    Vector4f get_angular_velocity(uint32_t index)  const;
    Matrix4x4f get_rotation(uint32_t index)  const;
    Matrix4x4f get_transform(uint32_t index) const;
    vr::HmdMatrix34_t get_raw_transform(uint32_t index) const;

    const auto& get_eyes() const {
        return get_runtime()->eyes;
    }

    void apply_hmd_transform(glm::quat& rotation, Vector4f& position);
    void apply_hmd_transform(::REJoint* camera_joint);
    
    bool is_hand_behind_head(VRRuntime::Hand hand, float sensitivity = 0.2f) const;
    bool is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle) const;
    Vector2f get_joystick_axis(vr::VRInputValueHandle_t handle) const;

    Vector2f get_left_stick_axis() const;
    Vector2f get_right_stick_axis() const;

    void trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle);
    
    auto get_action_set() const { return m_action_set; }
    auto& get_active_action_set() const { return m_active_action_set; }
    auto get_action_trigger() const { return m_action_trigger; }
    auto get_action_grip() const { return m_action_grip; }
    auto get_action_joystick() const { return m_action_joystick; }
    auto get_action_joystick_click() const { return m_action_joystick_click; }
    auto get_action_a_button() const { return m_action_a_button; }
    auto get_action_b_button() const { return m_action_b_button; }
    auto get_action_weapon_dial() const { return m_action_weapon_dial; }
    auto get_action_minimap() const { return m_action_minimap; }
    auto get_action_block() const { return m_action_block; }
    auto get_action_dpad_up() const { return m_action_dpad_up; }
    auto get_action_dpad_down() const { return m_action_dpad_down; }
    auto get_action_dpad_left() const { return m_action_dpad_left; }
    auto get_action_dpad_right() const { return m_action_dpad_right; }
    auto get_action_heal() const { return m_action_heal; }
    auto get_left_joystick() const { return m_left_joystick; }
    auto get_right_joystick() const { return m_right_joystick; }

    const auto& get_action_handles() const { return m_action_handles;}

    auto get_ui_scale() const { return m_ui_scale_option->value(); }
    const auto& get_raw_projections() const { return get_runtime()->raw_projections; }

    void unhide_crosshair() {
        m_last_crosshair_hide = std::chrono::steady_clock::now();
    }
    
    void notify_camera_destroyed(RECamera* camera) {
        for (auto& existing_camera : m_multipass_cameras) {
            if (existing_camera == camera) {
                existing_camera = nullptr;
            }
        }
    }

    void set_multipass_camera(RECamera* camera, uint32_t index) {
        if (index >= 2) {
            return;
        }

        m_multipass_cameras[index] = camera;
    }

    std::array<RECamera*, 2> get_cameras() const;
    auto& get_camera_duplicator() {
        return m_camera_duplicator;
    }

private:
    Vector4f get_position_unsafe(uint32_t index) const;
    Vector4f get_velocity_unsafe(uint32_t index) const;
    Vector4f get_angular_velocity_unsafe(uint32_t index) const;

private:
    // Hooks
    void on_view_get_size(REManagedObject* scene_view, float* result) override;
    static void inputsystem_update_hook(void* ctx, REManagedObject* input_system);
    void on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) override;
    static Matrix4x4f* gui_camera_get_projection_matrix_hook(REManagedObject* camera, Matrix4x4f* result);
    void on_camera_get_view_matrix(REManagedObject* camera, Matrix4x4f* result) override;
    
    static HookManager::PreHookResult pre_set_hdr_mode(std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>& arg_tys, uintptr_t ret_addr);
    static void post_set_hdr_mode(uintptr_t& ret_val, sdk::RETypeDefinition* ret_ty, uintptr_t ret_addr) {}

    bool on_pre_overlay_layer_update(sdk::renderer::layer::Overlay* layer, void* render_context) override;
    bool on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override;
    void on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override; // POST-overlay: flat3d fallback harvest (HUD, but pre-final-encode)
    void on_output_layer_draw(sdk::renderer::layer::Output* layer, void* render_context) override; // POST-output: latest hook, final tonemapped+HUD image

    bool on_pre_post_effect_layer_update(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
    bool on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
    void on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
    uint32_t m_previous_distortion_type{};
    bool m_set_next_post_effect_distortion_type{false};

    bool on_pre_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
    void on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
    bool on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) override;

    bool on_pre_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) override;
    void on_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) override;

    struct SceneLayerData {
        SceneLayerData() = default;
        SceneLayerData(sdk::renderer::SceneInfo* info) {
            setup(info);
        }

        void setup(sdk::renderer::SceneInfo* info) {
            scene_info = info;
            if (scene_info != nullptr) {
                this->view_projection_matrix = scene_info->view_projection_matrix;
            }
        }

        void post_setup(int32_t index) {
            scene_info->old_view_projection_matrix = previous_view_projection_matrices[(index - 1) % 2];
            previous_view_projection_matrices[index % 2] = scene_info->view_projection_matrix;
        }

        sdk::renderer::SceneInfo* scene_info{};
        Matrix4x4f view_projection_matrix{};
        std::array<Matrix4x4f, 2> previous_view_projection_matrices{};
    };

    std::unordered_map<sdk::renderer::layer::Scene*, std::array<SceneLayerData, 5>> m_scene_layer_data {};

    static void wwise_listener_update_hook(void* listener);

    //static float get_sharpness_hook(void* tonemapping);

    // initialization functions
    std::optional<std::string> initialize_openvr();
    std::optional<std::string> initialize_openvr_input();
    std::optional<std::string> initialize_openxr();
    std::optional<std::string> initialize_openxr_input();
    std::optional<std::string> initialize_openxr_swapchains();
    std::optional<std::string> initialize_flat3d();
    std::optional<std::string> hijack_resolution();
    std::optional<std::string> hijack_input();
    std::optional<std::string> hijack_camera();
    std::optional<std::string> hijack_wwise_listeners(); // audio hook

    std::optional<std::string> reinitialize_openvr() {
        spdlog::info("Reinitializing OpenVR");
        std::scoped_lock _{m_openvr_mtx};

        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        m_openvr.reset();

        // Reinitialize openvr input, hopefully this fixes the issue
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openvr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenVR: {}", *e);
        }

        return e;
    }

    std::optional<std::string> reinitialize_openxr() {
        spdlog::info("Reinitializing OpenXR");
        std::scoped_lock _{m_openvr_mtx};

        if (m_is_d3d12) {
            m_d3d12.openxr().destroy_swapchains();
        } else {
            m_d3d11.openxr().destroy_swapchains();
        }

        m_openxr.reset();
        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openxr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenXR: {}", *e);
        }

        return e;
    }

    bool detect_controllers();
    bool is_any_action_down();
    void update_hmd_state();
    void update_flat3d_params(); // separation/convergence/FoV auto-scale, before update_matrices
    void draw_flat3d_ui(); // Flatscreen 3D section of on_draw_ui
    void update_action_states();
    void update_camera(); // if not in firstperson mode
    void update_camera_origin(); // every frame
    void update_audio_camera();
    void update_render_matrix();
    void restore_audio_camera(); // after wwise listener update
    void restore_camera(); // After rendering
    void set_lens_distortion(bool value);
    void disable_bad_effects();
    void fix_temporal_effects();

    // input functions
    // Purpose: "Emulate" OpenVR input to the game
    // By setting things like input flags based on controller state
    void openvr_input_to_re2_re3(REManagedObject* input_system);
    void openvr_input_to_re_engine(); // generic, can be used on any game

    // Sets overlay layer to return instantly
    // causes world-space gui elements to render properly
    Patch::Ptr m_overlay_draw_patch{};
    
    mutable std::recursive_mutex m_openvr_mtx{};
    mutable std::recursive_mutex m_wwise_mtx{};
    mutable std::recursive_mutex m_scene_update_mtx{};
    mutable std::shared_mutex m_gui_mtx{};
    mutable std::shared_mutex m_rotation_mtx{};

    vr::VRTextureBounds_t m_right_bounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    vr::VRTextureBounds_t m_left_bounds{ 0.0f, 0.0f, 1.0f, 1.0f };

    glm::vec3 m_overlay_rotation{-1.550f, 0.0f, -1.330f};
    glm::vec4 m_overlay_position{0.0f, 0.06f, -0.07f, 1.0f};

    float m_nearz{ 0.1f };
    float m_farz{ 3000.0f };

    std::array<RECamera*, 2> m_multipass_cameras{};

    std::shared_ptr<VRRuntime> m_runtime{std::make_shared<VRRuntime>()}; // will point to the real runtime if it exists
    std::shared_ptr<runtimes::OpenVR> m_openvr{std::make_shared<runtimes::OpenVR>()};
    std::shared_ptr<runtimes::OpenXR> m_openxr{std::make_shared<runtimes::OpenXR>()};
    std::shared_ptr<runtimes::Flat3D> m_flat3d{std::make_shared<runtimes::Flat3D>()};

    Vector4f m_standing_origin{ 0.0f, 1.5f, 0.0f, 0.0f };
    glm::quat m_rotation_offset{ glm::identity<glm::quat>() };
    glm::quat m_gui_rotation_offset{ glm::identity<glm::quat>() };

    std::vector<int32_t> m_controllers{};
    std::unordered_set<int32_t> m_controllers_set{};

    // Action set handles
    vr::VRActionSetHandle_t m_action_set{};
    vr::VRActiveActionSet_t m_active_action_set{};

    // Action handles
    vr::VRActionHandle_t m_action_trigger{ };
    vr::VRActionHandle_t m_action_grip{ };
    vr::VRActionHandle_t m_action_joystick{};
    vr::VRActionHandle_t m_action_joystick_click{};
    vr::VRActionHandle_t m_action_a_button{};
    vr::VRActionHandle_t m_action_b_button{};
    vr::VRActionHandle_t m_action_dpad_up{};
    vr::VRActionHandle_t m_action_dpad_right{};
    vr::VRActionHandle_t m_action_dpad_down{};
    vr::VRActionHandle_t m_action_dpad_left{};
    vr::VRActionHandle_t m_action_system_button{};
    vr::VRActionHandle_t m_action_weapon_dial{};
    vr::VRActionHandle_t m_action_re3_dodge{};
    vr::VRActionHandle_t m_action_re2_quickturn{};
    vr::VRActionHandle_t m_action_re2_firstperson_toggle{};
    vr::VRActionHandle_t m_action_re2_reset_view{};
    vr::VRActionHandle_t m_action_re2_change_ammo{};
    vr::VRActionHandle_t m_action_re2_toggle_flashlight{};
    vr::VRActionHandle_t m_action_minimap{};
    vr::VRActionHandle_t m_action_block{};
    vr::VRActionHandle_t m_action_haptic{};
    vr::VRActionHandle_t m_action_heal{};

    bool m_was_firstperson_toggle_down{false};
    bool m_was_flashlight_toggle_down{false};
    
    
    std::unordered_map<std::string, std::reference_wrapper<vr::VRActionHandle_t>> m_action_handles {
        { "/actions/default/in/Trigger", m_action_trigger },
        { "/actions/default/in/Grip", m_action_grip },
        { "/actions/default/in/Joystick", m_action_joystick },
        { "/actions/default/in/JoystickClick", m_action_joystick_click },
        { "/actions/default/in/AButton", m_action_a_button },
        { "/actions/default/in/BButton", m_action_b_button },
        { "/actions/default/in/DPad_Up", m_action_dpad_up },
        { "/actions/default/in/DPad_Right", m_action_dpad_right },
        { "/actions/default/in/DPad_Down", m_action_dpad_down },
        { "/actions/default/in/DPad_Left", m_action_dpad_left },
        { "/actions/default/in/SystemButton", m_action_system_button },
        { "/actions/default/in/WeaponDial_Start", m_action_weapon_dial },
        { "/actions/default/in/RE3_Dodge", m_action_re3_dodge },
        { "/actions/default/in/RE2_Quickturn", m_action_re2_quickturn },
        { "/actions/default/in/RE2_FirstPerson_Toggle", m_action_re2_firstperson_toggle },
        { "/actions/default/in/RE2_Reset_View", m_action_re2_reset_view },
        { "/actions/default/in/RE2_Change_Ammo", m_action_re2_change_ammo },
        { "/actions/default/in/RE2_Toggle_Flashlight", m_action_re2_toggle_flashlight },
        { "/actions/default/in/MiniMap", m_action_minimap },
        { "/actions/default/in/Block", m_action_block },
        { "/actions/default/in/Heal", m_action_heal },

        // Out
        { "/actions/default/out/Haptic", m_action_haptic },
    };

    // Input sources
    vr::VRInputValueHandle_t m_left_joystick{};
    vr::VRInputValueHandle_t m_right_joystick{};

    // Input system history
    std::bitset<64> m_button_states_down{};
    std::bitset<64> m_button_states_on{};
    std::bitset<64> m_button_states_up{};
    std::chrono::steady_clock::time_point m_last_controller_update{};
    std::chrono::steady_clock::time_point m_last_interaction_display{};
    std::chrono::steady_clock::time_point m_last_crosshair_hide{};
    uint32_t m_backbuffer_inconsistency_start{};
    std::chrono::nanoseconds m_last_input_delay{};
    std::chrono::nanoseconds m_avg_input_delay{};

    HANDLE m_present_finished_event{CreateEvent(nullptr, TRUE, FALSE, nullptr)};

    Vector4f m_raw_projections[2]{};

    vrmod::D3D11Component m_d3d11{};
    vrmod::D3D12Component m_d3d12{};
    vrmod::OverlayComponent m_overlay_component{};
    vrmod::CameraDuplicator m_camera_duplicator{};
    vrmod::Flat3DDepth m_flat3d_depth_sampler{};
    // AFW (alternate frame warp): PDAFWPlugin binding + plugin-side per-eye buffers. Init'd from
    // D3D12Component once the device/queue exist; unavailable (with status text) if the real dll
    // isn't beside the game exe.
    vrmod::Flat3DAFW m_flat3d_afw{};

    // AFW per-frame camera matrices, recorded by the view/proj hooks each render pass - BOTH eyes,
    // same-tick (the pass's own eye + the other computed from the same base). Present-time code
    // indexes by ITS OWN fill parity, avoiding the hooks' one-frame-ahead parity skew.
    struct AfwFrameData {
        Matrix4x4f view[2]{glm::identity<Matrix4x4f>(), glm::identity<Matrix4x4f>()};
        Matrix4x4f proj[2]{glm::identity<Matrix4x4f>(), glm::identity<Matrix4x4f>()};
        int32_t rfc{-1}; // render frame the matrices were recorded on
        bool valid{false};
    } m_afw_frame{};
    // Frame-stamped ring (UEVR-3D keeps a 3-deep history + offset guard for the same reason): the
    // game thread may already have recorded frame N+1's matrices by the time frame N's warp runs.
    // Consumers look up their own frame; fall back to the live struct on a miss.
    AfwFrameData m_afw_frame_ring[4]{};

public:
    const AfwFrameData& get_afw_frame() const { return m_afw_frame; }
    const AfwFrameData& get_afw_frame_for(int32_t rfc) const {
        const auto& e = m_afw_frame_ring[rfc & 3];
        if (e.valid && e.rfc == rfc) {
            return e;
        }
        // The hooks read the frame counter one ahead of present for the same logical frame
        // (observed: exact-stamp lookups always miss by +1).
        const auto& e1 = m_afw_frame_ring[(rfc + 1) & 3];
        return (e1.valid && e1.rfc == rfc + 1) ? e1 : m_afw_frame;
    }
    bool afw_per_eye_dlss_enabled() const { return true; } // settled always-on
    bool afw_dlss_mv_feed_enabled() const { return true; } // settled always-on
    float afw_obj_motion_scale() const { return m_flat3d_afw_obj_motion->value(); }


    // AFW depth/motion-vector sources: the LIVE engine depth + VelocityTarget natives (captured at
    // the overlay hook; copied at present time by run_flat3d_afw with our own command list - the
    // engine copy path crashes on Wilds for depth).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_afw_depth_tex{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_afw_mv_tex{};

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct MultiPass {
        std::array<d3d12::TextureContext, 2> eye_contexts{}; // For the SRV
        std::array<ComPtr<ID3D12Resource>, 2> eye_textures{};
        std::array<sdk::intrusive_ptr<sdk::renderer::Texture>, 2> native_res_copies{}; // used with TemporalUpscaler disabled
        // Flat3D: per-eye clone of the PrepareOutput's TargetState. We set_output_state
        // to redirect each eye's prepared color INTO its clone; because the engine
        // renders into it, the clone is a real, state-tracked render target we can read
        // safely (unlike a bare create_texture clone, which the command executor can't
        // resolve). See VR::on_pre_prepare_output_layer_draw.
        std::array<sdk::intrusive_ptr<sdk::renderer::TargetState>, 2> flat3d_output_clones{};
        // Flat3D RTV-swap redirect: a per-eye clone of the output RTV. We set_rtv it into
        // the eye's output TargetState so the engine renders that eye into OUR private
        // texture (which it then tracks). create_render_target_view resolves on MHWilds
        // (create_target_state does not, so we swap the RTV instead of cloning the whole
        // TargetState). Read at present time - never mid-frame - so no GPU race.
        std::array<sdk::intrusive_ptr<sdk::renderer::RenderTargetView>, 2> flat3d_eye_rtvs{};
        // The texture backing each eye's clone RTV, kept DIRECTLY from the manual clone -
        // the clone RTV's get_texture_d3d12() reads null (the worker doesn't populate that
        // member for externally-created RTVs), so we track the texture ourselves.
        std::array<sdk::intrusive_ptr<sdk::renderer::Texture>, 2> flat3d_eye_textures{};
        std::array<uint32_t, 2> allocated_size{};
        uint32_t pass{0};
        // Flat3D GUI-match: a clone of the PRIMARY eye's target harvested at the PRE-overlay hook
        // (scene only, no GUI). The compose diffs the post-overlay primary eye against this to build a
        // GUI mask, then paints the primary eye's GUI onto the clone eye - so both eyes show the same
        // GUI at screen depth while the backgrounds stay stereo. Allocated alongside native_res_copies.
        sdk::intrusive_ptr<sdk::renderer::Texture> pre_left_copy{};
        ComPtr<ID3D12Resource> pre_left_texture{};
        // Same for the RIGHT eye - needed to depth-SHIFT the matched GUI without ghosting the clone
        // eye's own baked HUD (we repaint the isolated GUI over this scene-only base).
        sdk::intrusive_ptr<sdk::renderer::Texture> pre_right_copy{};
        ComPtr<ID3D12Resource> pre_right_texture{};
    } m_multipass{};
    

    Vector4f m_original_camera_position{ 0.0f, 0.0f, 0.0f, 0.0f };
    glm::quat m_original_camera_rotation{ glm::identity<glm::quat>() };

    Matrix4x4f m_original_camera_matrix{ glm::identity<Matrix4x4f>() };

    Vector4f m_original_audio_camera_position{ 0.0f, 0.0f, 0.0f, 0.0f };
    glm::quat m_original_audio_camera_rotation{ glm::identity<glm::quat>() };

    Matrix4x4f m_render_camera_matrix{ glm::identity<Matrix4x4f>() };

    sdk::helpers::NativeObject m_via_hid_gamepad{ "via.hid.GamePad" };

    // options
    int m_frame_count{};
    int m_render_frame_count{};
    int m_last_frame_count{-1};
    int m_left_eye_frame_count{0};
    int m_right_eye_frame_count{0};

    bool m_submitted{false};
    //bool m_disable_sharpening{true};

    bool m_needs_camera_restore{false};
    bool m_needs_audio_restore{false};
    bool m_in_render{false};
    bool m_in_lightshaft{false};
    bool m_positional_tracking{true};
    bool m_is_d3d12{false};
    bool m_backbuffer_inconsistency{false};
    bool m_init_finished{false};
    bool m_has_hw_scheduling{false}; // hardware accelerated GPU scheduling

    // on the backburner
    bool m_depth_aided_reprojection{false};

    // == 1 or == 0
    uint8_t m_left_eye_interval{0};
    uint8_t m_right_eye_interval{1};

    static std::string actions_json;
    static std::string binding_rift_json;
    static std::string bindings_oculus_touch_json;
    static std::string binding_vive;
    static std::string bindings_vive_controller;
    static std::string bindings_knuckles;

    const std::unordered_map<std::string, std::string> m_binding_files {
        { "actions.json", actions_json },
        { "binding_rift.json", binding_rift_json },
        { "bindings_oculus_touch.json", bindings_oculus_touch_json },
        { "binding_vive.json", binding_vive },
        { "bindings_vive_controller.json", bindings_vive_controller },
        { "bindings_knuckles.json", bindings_knuckles }
    };

    const ModKey::Ptr m_set_standing_key{ ModKey::create(generate_name("SetStandingOriginKey")) };
    const ModKey::Ptr m_recenter_view_key{ ModKey::create(generate_name("RecenterViewKey")) };
    const ModToggle::Ptr m_decoupled_pitch{ ModToggle::create(generate_name("DecoupledPitch"), false) };
    const ModCombo::Ptr m_rendering_technique{ 
        ModCombo::create(generate_name("RenderingTechnique_V2"),
        {
            "Alternating/AFR", 
            "Two Frame Sequential", 
            "Single Frame Multipass"
        }, 
#if TDB_VER < 69
        1 // Previous rendering technique
#else
        2 // New rendering technique
#endif
        ) 
    };
    const ModToggle::Ptr m_use_custom_view_distance{ ModToggle::create(generate_name("UseCustomViewDistance"), false) };
    const ModToggle::Ptr m_hmd_oriented_audio{ ModToggle::create(generate_name("HMDOrientedAudio"), true) };
    const ModSlider::Ptr m_view_distance{ ModSlider::create(generate_name("CustomViewDistance"), 10.0f, 3000.0f, 500.0f) };
    const ModSlider::Ptr m_motion_controls_inactivity_timer{ ModSlider::create(generate_name("MotionControlsInactivityTimer"), 30.0f, 100.0f, 30.0f) };
    const ModSlider::Ptr m_joystick_deadzone{ ModSlider::create(generate_name("JoystickDeadzone"), 0.01f, 0.9f, 0.15f) };
    const ModSlider::Ptr m_ui_scale_option{ ModSlider::create(generate_name("2DUIScale"), 1.0f, 100.0f, 12.0f) };
    const ModSlider::Ptr m_ui_distance_option{ ModSlider::create(generate_name("2DUIDistance"), 0.01f, 8.0f, 1.0f) };
    const ModSlider::Ptr m_world_ui_scale_option{ ModSlider::create(generate_name("WorldSpaceUIScale"), 1.0f, 100.0f, 15.0f) };
    const ModSlider::Ptr m_resolution_scale{ ModSlider::create(generate_name("OpenXRResolutionScale"), 0.1f, 5.0f, 1.0f) };

    const ModToggle::Ptr m_force_fps_settings{ ModToggle::create(generate_name("ForceFPS"), true) };

#if TDB_VER < 69
    const ModToggle::Ptr m_force_aa_settings{ ModToggle::create(generate_name("ForceAntiAliasing"), true) };
#else
    // On new versions, since we're using the new rendering technique, we don't need to turn AA off
    const ModToggle::Ptr m_force_aa_settings{ ModToggle::create(generate_name("ForceAntiAliasing_V2"), false) };
#endif

    const ModToggle::Ptr m_force_motionblur_settings{ ModToggle::create(generate_name("ForceMotionBlur"), true) };
    const ModToggle::Ptr m_force_vsync_settings{ ModToggle::create(generate_name("ForceVSync"), true) };
    const ModToggle::Ptr m_force_lensdistortion_settings{ ModToggle::create(generate_name("ForceLensDistortion"), true) };
    const ModToggle::Ptr m_force_volumetrics_settings{ ModToggle::create(generate_name("ForceVolumetrics"), true) };
    const ModToggle::Ptr m_force_lensflares_settings{ ModToggle::create(generate_name("ForceLensFlares"), true) };
    const ModToggle::Ptr m_force_dynamic_shadows_settings{ ModToggle::create(generate_name("ForceDynamicShadows"), true) };

#ifdef REFRAMEWORK_UNIVERSAL
    const ModToggle::Ptr m_allow_engine_overlays{ ModToggle::create(generate_name("AllowEngineOverlays_V2"), sdk::GameIdentity::get().tdb_ver() < 73) };
#else
#if TDB_VER < 73
    const ModToggle::Ptr m_allow_engine_overlays{ ModToggle::create(generate_name("AllowEngineOverlays_V2"), true) };
#else
    const ModToggle::Ptr m_allow_engine_overlays{ ModToggle::create(generate_name("AllowEngineOverlays_V2"), false) };
#endif
#endif

    const ModToggle::Ptr m_desktop_fix{ ModToggle::create(generate_name("DesktopRecordingFix"), true) };
    const ModToggle::Ptr m_desktop_fix_skip_present{ ModToggle::create(generate_name("DesktopRecordingFixSkipPresent"), true) };

#ifdef REFRAMEWORK_UNIVERSAL
    const ModToggle::Ptr m_enable_asynchronous_rendering{ ModToggle::create(generate_name("AsyncRendering_V3"), sdk::GameIdentity::get().tdb_ver() < 73) };
#else
#if TDB_VER >= 73
    const ModToggle::Ptr m_enable_asynchronous_rendering{ ModToggle::create(generate_name("AsyncRendering_V3"), false) };
#else
    const ModToggle::Ptr m_enable_asynchronous_rendering{ ModToggle::create(generate_name("AsyncRendering_V3"), true) };
#endif
#endif

    // Flatscreen 3D output settings (always enabled/active when no HMD runtime is present;
    // the Enabled/Active toggles are retired)
    const ModCombo::Ptr m_flat3d_output_mode{
        ModCombo::create(generate_name("Flat3D_OutputMode"),
        {
            "Side-by-Side",
            "Top-and-Bottom",
            "Row Interlaced",
            "Column Interlaced",
            "Checkerboard",
            "LeiaSR (Autostereo)",
            "Anaglyph Red-Cyan",
            "Anaglyph Red-Cyan (Dubois)",
            "Anaglyph Red-Cyan (Half-Color)",
            "Anaglyph Green-Magenta",
            "Anaglyph Green-Magenta (Dubois)",
            "Anaglyph Blue-Amber",
            "Debug: Left Eye Only",
            "Debug: Right Eye Only"
        }, FLAT3D_SBS)
    };
    const ModToggle::Ptr m_flat3d_eye_swap{ ModToggle::create(generate_name("Flat3D_EyeSwap"), false) };
    const ModSlider::Ptr m_flat3d_depth{ ModSlider::create(generate_name("Flat3D_Depth"), 0.0f, 3.0f, 0.4f) };
    const ModSlider::Ptr m_flat3d_convergence{ ModSlider::create(generate_name("Flat3D_Convergence"), 0.01f, 4.0f, 2.5f) };
    const ModSlider::Ptr m_flat3d_reference_fov{ ModSlider::create(generate_name("Flat3D_ReferenceFOV"), 10.0f, 120.0f, 70.0f) };
    const ModToggle::Ptr m_flat3d_crop_eyes_169{ ModToggle::create(generate_name("Flat3D_CropEyesTo169"), false) };
    const ModToggle::Ptr m_flat3d_swap_shear_sign{ ModToggle::create(generate_name("Flat3D_SwapShearSign"), false) };
    const ModToggle::Ptr m_flat3d_auto_convergence{ ModToggle::create(generate_name("Flat3D_AutoConvergence"), false) };
    const ModSlider::Ptr m_flat3d_max_popout{ ModSlider::create(generate_name("Flat3D_MaxPopoutPercent"), 0.1f, 5.0f, 1.5f) };
    const ModSlider::Ptr m_flat3d_autoconv_smoothing{ ModSlider::create(generate_name("Flat3D_AutoConvSmoothing"), 0.01f, 0.25f, 0.08f) };
    const ModToggle::Ptr m_flat3d_dynamic_crosshair{ ModToggle::create(generate_name("Flat3D_DynamicCrosshair"), false) };
    const ModSlider::Ptr m_flat3d_crosshair_depth{ ModSlider::create(generate_name("Flat3D_CrosshairFallbackDepth"), 0.1f, 100.0f, 10.0f) };
    const ModToggle::Ptr m_flat3d_swap_shift_sign{ ModToggle::create(generate_name("Flat3D_SwapShiftSign"), false) };
    // World-space GUI (multipass): map screen GUI onto a camera-facing plane at this depth (world
    // units / metres). Both eyes render it -> fixes the per-eye divergence AND gives adjustable
    // GUI depth. World-anchored markers keep their own depth (they have a mesh -> skipped).
    // Live tuning for the world-GUI plane (camera handedness/scale unknown until tested in real
    // gameplay): flip the camera forward, and a size multiplier if the plane is too big/small.
    // Menus/title: the clone eye renders the GUI at a divergent state. Mirror the primary eye into
    // both clones so the GUI matches exactly (flattens stereo - fine on menus, turn off in gameplay).
    // Stereo-preserving GUI match: diff the primary eye's post-overlay vs pre-overlay (scene-only) to
    // isolate the GUI, then paint it onto the clone eye. Both eyes show identical GUI at screen depth
    // while backgrounds stay stereo. Preferred over MirrorEyes (which flattens everything).
    // Debug: output the isolated GUI mask (white=GUI) instead of the composed image, to tune threshold.
    // Matched-GUI depth: per-eye horizontal disparity (eye-UV units) applied oppositely to the isolated
    // GUI. 0 = screen depth; +/- pushes the whole HUD behind/in front of the screen. UEVR-style depth
    // without world-space conversion (which is invisible on Wilds).
    // (Overlay-RT redirect / GUI separation is settled ALWAYS ON: the engine's GUI draw is
    // redirected into our own transparent-cleared per-eye textures (Flat3DGuiRedirect) and
    // composited per-eye at the GUI plane's disparity - warp inputs stay naturally hudless.)
    // (GUI capture eye phase is hardcoded swapped: RE computes world-anchored GUI positions
    // during the game UPDATE phase, before the render-frame counter increments, so a frame's GUI
    // content carries the PREVIOUS frame's eye projection and belongs in the opposite slot.
    // User-validated twice in gameplay 2026-07-31.)
    // Set by the present-time display-native check; drives the pixel-exactness warning for
    // interlaced/checkerboard/LeiaSR modes.
    std::atomic<bool> m_flat3d_native_mismatch{false};
    // (Native-output override is always on: ResizeBuffers substitutes the display's physical
    // resolution; the compose samples the engine-believed sub-region.)
    // AFW (alternate frame warp) settings. Enabled = use the PDAFWPlugin to warp the missing AFR eye
    // (only takes effect when the AFR technique is selected and the real plugin dll initialized).
    const ModToggle::Ptr m_flat3d_afw_enabled{ ModToggle::create(generate_name("Flat3D_AFW"), true) };
    // Warp mode A/B for the disocclusion halo / noise around foreground objects:
    // Combined (default) = other-eye depth reprojection blended with same-eye MV history;
    // Other-eye only isolates the depth-reprojection layer (halo = edge stretch);
    // Previous-frame only isolates the history layer. (Plugin FrameWarpMode.)
    const ModCombo::Ptr m_flat3d_afw_mode{
        ModCombo::create(generate_name("Flat3D_AFW_Mode"),
        {
            "Combined (other-eye + history)",
            "Other-eye only (depth reprojection)",
            "Previous-frame only (MV history)",
        }, 0)
    };
    // (ClearBeforeWarping is settled OFF: the user A/B showed clearing makes the shadow-halo
    // flicker WORSE - stale-pixel fill partially hides the disocclusion holes.)
    // Foreground depth-edge dilation radius (px) fed to the warp: reversed-Z max filter widens
    // each silhouette so edge pixels reproject WITH the object instead of flickering between
    // foreground/background (the depth-reprojection halo the user isolated). 0 = off.
    const ModSlider::Ptr m_flat3d_afw_depth_dilation{ ModSlider::create(generate_name("Flat3D_AFW_DepthDilation"), 0.0f, 4.0f, 1.0f) };
    // v3 warp object motion: add this frame's per-object motion (raw MV minus camera temporal
    // flow, same-frame extraction) to the camera-only eye-jump field, so movers keep advancing in
    // the warp's history layer (fixes half-rate character stutter). 0 = off (pure camera field).
    // Foliage self-motion is the historical flicker risk - gated by the motion threshold below.
    const ModSlider::Ptr m_flat3d_afw_obj_motion{ ModSlider::create(generate_name("Flat3D_AFW_ObjMotion"), 0.0f, 8.0f, 3.0f) };
    // Plugin-side per-object motion gate (pixels): with object motion in the field, small motions
    // (wind-blown foliage) below this are ignored by the warp while large mover motion passes.
    // (Inert while the field was camera-only - there was nothing to gate.)
    const ModSlider::Ptr m_flat3d_afw_motion_thresh{ ModSlider::create(generate_name("Flat3D_AFW_MotionThreshold"), 0.0f, 60.0f, 25.0f) };
    const ModToggle::Ptr m_flat3d_afw_debug{ ModToggle::create(generate_name("Flat3D_AFW_Debug"), false) }; // log readbacks/diagnostics only
    const ModToggle::Ptr m_flat3d_afw_plugin_debug{ ModToggle::create(generate_name("Flat3D_AFW_PluginDebug"), false) }; // plugin's own debug view (changes rendering)
    // (Synthetic camera-only MV field + CombinedWarping are now ALWAYS ON - validated by burst
    // readbacks 2026-07-30: field matches raw MVs to the 3rd decimal on static geometry, and
    // camera-only displacement keeps wind-blown foliage from warping by its own animation.
    // The sep-scale depth probe, matrix-transpose experiment, and the v2 object-motion feed term
    // are settled and removed: scale 1, no transpose, camera-only field.)
    // (Per-eye DLSS histories + same-eye MV feed are settled ALWAYS ON: per-eye histories fix
    // the cross-eye TAA ghosting, and the same-eye N->N-2 feed fixes the under-accumulation
    // noise those histories would otherwise cause. Both user-validated.)

    // Flatscreen 3D transient state: game-projection terms recorded in the
    // projection hook (render thread) and consumed by update_flat3d_params.
    std::atomic<float> m_flat3d_game_p00{1.0f};
    std::atomic<float> m_flat3d_game_p11{1.0f};
    float m_flat3d_fov_scale_ema{1.0f};

    // Flat3D UI extraction (multipass): redirect the PRIMARY eye's Overlay draw into a private
    // transparent target so we capture the UI ONLY (icons/highlights/text), then composite it
    // onto BOTH eyes with a per-eye depth shift + fit-scale (UEVR-style). Increment 1 just
    // validates the capture. Toggle for A/B; targets are engine clones kept across frames.
    // WIP UI extraction. Redirect-render approach is BLOCKED (engine can't render into an
    // unregistered create_texture clone as RENDER_TARGET - not implicitly promotable, no barrier
    // injection available). Off by default; next approach = read the engine's OWN GUI target.
    bool m_flat3d_extract_ui{false};    // BASELINE: extraction off - confirm heartbeat grows + no rehook loop
    bool m_flat3d_ui_diag{true};
    bool m_flat3d_ui_debug_show{false};
    // MHWilds can't clone a whole TargetState (create_target_state doesn't resolve), so we clone
    // just the RTV (create_render_target_view DOES resolve) and set_rtv it into the existing
    // overlay target state, restoring the original RTV after the draw.
    sdk::intrusive_ptr<sdk::renderer::Texture> m_flat3d_ui_tex{};        // direct texture clone (native resolves)
    sdk::intrusive_ptr<sdk::renderer::RenderTargetView> m_flat3d_ui_rtv{};       // RTV pointing at m_flat3d_ui_tex
    sdk::intrusive_ptr<sdk::renderer::RenderTargetView> m_flat3d_ui_saved_rtv{}; // engine's real RTV, restored post-draw
    bool m_flat3d_ui_backed{false};
    bool m_flat3d_ui_redirect_ready{false}; // defer redirect until tile-mapping completed a prior frame
    // Physical D3D12 state of the UI clone - WE cycle it with explicit barriers (the engine can't,
    // it's unregistered): COMMON -> RENDER_TARGET (engine draws) -> COMMON (next frame).
    D3D12_RESOURCE_STATES m_flat3d_ui_state{D3D12_RESOURCE_STATE_COMMON};
    ID3D12Resource* m_flat3d_ui_native{nullptr}; // cached native of m_flat3d_ui_tex (for barriers)

    // Auto-convergence state (only touched from update_flat3d_params)
    float m_flat3d_znear_ema{-1.0f};
    float m_flat3d_inv_conv_ema{-1.0f};
    std::array<float, 5> m_flat3d_znear_history{};
    uint32_t m_flat3d_znear_history_count{0};
    uint32_t m_flat3d_znear_history_idx{0};

    bool m_disable_projection_matrix_override{ false };
    bool m_disable_gui_camera_projection_matrix_override{ false };
    bool m_disable_view_matrix_override{false};
    bool m_disable_backbuffer_size_override{false};
    bool m_disable_temporal_fix{false};
    bool m_disable_post_effect_fix{false};

    ValueList m_options{
        *m_set_standing_key,
        *m_recenter_view_key,
        *m_decoupled_pitch,
        *m_rendering_technique,
        *m_use_custom_view_distance,
        *m_hmd_oriented_audio,
        *m_view_distance,
        *m_motion_controls_inactivity_timer,
        *m_joystick_deadzone,
        *m_force_fps_settings,
        *m_force_aa_settings,
        *m_force_motionblur_settings,
        *m_force_vsync_settings,
        *m_force_lensdistortion_settings,
        *m_force_volumetrics_settings,
        *m_force_lensflares_settings,
        *m_force_dynamic_shadows_settings,
        *m_ui_scale_option,
        *m_ui_distance_option,
        *m_world_ui_scale_option,
        *m_allow_engine_overlays,
        *m_resolution_scale,
        *m_desktop_fix,
        *m_desktop_fix_skip_present,
        *m_enable_asynchronous_rendering,
        *m_flat3d_output_mode,
        *m_flat3d_eye_swap,
        *m_flat3d_depth,
        *m_flat3d_convergence,
        *m_flat3d_reference_fov,
        *m_flat3d_crop_eyes_169,
        *m_flat3d_swap_shear_sign,
        *m_flat3d_auto_convergence,
        *m_flat3d_max_popout,
        *m_flat3d_autoconv_smoothing,
        *m_flat3d_dynamic_crosshair,
        *m_flat3d_crosshair_depth,
        *m_flat3d_swap_shift_sign,
        *m_flat3d_afw_mode,
        *m_flat3d_afw_depth_dilation,
        *m_flat3d_afw_obj_motion,
        *m_flat3d_afw_motion_thresh,
        *m_flat3d_afw_debug,        // persisted so headless tests can drive the debug views via config
        *m_flat3d_afw_plugin_debug,
    };

    bool m_use_rotation{true};

    friend class vrmod::D3D11Component;
    friend class vrmod::D3D12Component;
    friend class vrmod::OverlayComponent;
};
