#pragma once

#include "VRRuntime.hpp"

namespace runtimes {
// Flatscreen stereo 3D "runtime": no HMD, no tracking, no compositor.
// The game camera stays fully game-controlled (orientation, position, FoV);
// this runtime only supplies the per-eye lateral offset through eyes[].
// Convergence is applied as an off-axis shear to the game's own projection
// matrix inside VR::on_camera_get_projection_matrix, so projections[] here
// stays identity and is never consumed.
struct Flat3D final : public VRRuntime {
    Flat3D() {
        this->custom_stage = SynchronizeStage::EARLY;
    }

    virtual ~Flat3D() {
        this->destroy();
    }

    std::string_view name() const override {
        return "Flatscreen3D";
    }

    VRRuntime::Type type() const override {
        return VRRuntime::Type::FLAT3D;
    }

    bool ready() const override {
        return VRRuntime::ready() && this->enabled;
    }

    VRRuntime::Error update_poses() override {
        // There are no poses to wait on. Marking them valid makes
        // VR::update_hmd_state zero the standing origin (poses read as
        // zero/identity for a non-OpenVR/OpenXR runtime), which keeps
        // apply_hmd_transform a no-op.
        this->got_first_poses = true;
        this->got_first_valid_poses = true;
        this->needs_pose_update = false;
        return VRRuntime::Error::SUCCESS;
    }

    uint32_t get_width() const override;
    uint32_t get_height() const override;

    VRRuntime::Error update_matrices(float nearz, float farz) override;

    // Live stereo on/off (drives ready()); separate from loaded so the user
    // can toggle the effect without a restart.
    bool enabled{true};

    // Written by VR::update_flat3d_params() each frame before update_matrices.
    //
    // separation is the CLIP-SPACE stereo knob (dynamic3d 1.1): the projection shear is literally
    // dir * separation, with no convergence and no FoV term in it, and the value equals total
    // background disparity as a fraction of the screen width.
    //
    // eye_half_offset is DERIVED from it, not a setting: half of
    //   eye_baseline = 2 * separation * tan(half_hfov) * convergence
    // which is what keeps zero parallax pinned at `convergence` while separation is held fixed.
    // It therefore moves with both the game's live FoV and convergence - nothing may treat the
    // physical eye separation as a constant any more (dynamic3d 1.1 / pitfall 10).
    float separation{0.05f};       // clip-space; fraction of screen width at infinity
    float convergence{1.0f};       // meters
    float eye_half_offset{0.025f}; // meters; derived per frame, never a setting
    float shear_dir_left{1.0f};    // empirical shear sign for the LEFT eye (right eye = -this)

private:
    // Last-known-good swapchain dimensions (get_width/get_height are const).
    mutable uint32_t m_last_width{0};
    mutable uint32_t m_last_height{0};
};
}
