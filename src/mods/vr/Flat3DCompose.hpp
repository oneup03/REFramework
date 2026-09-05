#pragma once

#include <array>

#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>

#include "d3d12/CommandContext.hpp"
#include "d3d12/TextureContext.hpp"
#include "Flat3DLeiaSR.hpp"

namespace vrmod {
// Fullscreen compose pass that packs the two eye textures into the real
// backbuffer for flatscreen stereo 3D output (SbS/TaB/interlaced/checkerboard/
// anaglyph). One shader, mode-keyed. Runs at the end of D3D12Component::on_frame,
// before REFramework draws its menu, so the menu stays readable on top.
class Flat3DCompose {
public:
    // Shader mode constants (output3d numbering; 6-8, 11, 15 reserved for
    // DualDisplay/Katanga/deghosted variants).
    enum ShaderMode : int32_t {
        MODE_SBS = 0,
        MODE_TAB = 1,
        MODE_ROW_INTERLACED = 2,
        MODE_COL_INTERLACED = 3,
        MODE_CHECKERBOARD = 4,
        MODE_LEIA_SR = 5, // composes SbS; the weaver consumes it (M3)
        MODE_ANA_RC = 9,
        MODE_ANA_RC_DUBOIS = 10,
        MODE_ANA_RC_COMPROMISE = 12,
        MODE_ANA_GM = 13,
        MODE_ANA_GM_DUBOIS = 14,
        MODE_ANA_BLUE_AMBER = 16,
        MODE_DEBUG_LEFT = 100,
        MODE_DEBUG_RIGHT = 101,
    };

    // Root-constant block; must match the cbuffer in the embedded shader.
    struct RepackParams {
        int32_t out_size[2]{};
        int32_t mode{MODE_SBS};
        int32_t eye_swap{0};
        float scene_shift_uv{0.0f}; // symmetric-projection convergence fallback (0 = off)
        float scene_scale{1.0f};    // cover-zoom paired with scene_shift_uv (1 = off)
        float crop_origin_u{0.0f};  // centered 16:9 per-eye crop; (0, 1) = off
        float crop_frac_u{1.0f};
        int32_t eye_size[2]{};          // per-eye texture pixel dimensions
        int32_t crosshair_enabled{0};
        int32_t pad0{0};
        float crosshair_shift_uv{0.0f}; // per-eye disparity shift, eye-UV units (left eye = +this)
        float crosshair_len_px{12.0f};
        float crosshair_thick_px{2.0f};
        float pad1{0.0f};
        // GUI-match: paint the primary eye's GUI (isolated by diffing post vs pre-overlay scene) onto
        // the clone eye, so both eyes show the same GUI at screen depth while backgrounds stay stereo.
        int32_t gui_match_enabled{0};
        float gui_mask_threshold{0.02f};
        int32_t gui_match_debug{0}; // 1 = output the GUI mask (white) for tuning
        float gui_depth_shift{0.0f}; // per-eye disparity (eye-UV) applied oppositely to the matched GUI
        // AFW extracted-UI composite: t2 = plugin-extracted UI+alpha (premultiplied); both eyes
        // composite the SAME texture at native size. Mode 1 = flat plane at ui_shift_uv (base =
        // the scene's symmetric-projection shift + HUD-depth slider). Mode 2 = depth-adaptive:
        // t3 = scene depth (reversed-Z); per-pixel shift = ui_depth_scale * d + ui_disp_bias
        // (+ ui_shift_uv as global offset) - world-anchored icons inherit their anchor's depth.
        int32_t ui_enabled{0};
        float ui_shift_uv{0.0f};
        float ui_depth_scale{0.0f};
        float ui_disp_bias{0.0f};
        // Native-output override: engine content occupies this top-left fraction of the eye
        // textures; the compose upscales it to the full (display-native) output. (1,1) = off.
        float content_frac_u{1.0f};
        float content_frac_v{1.0f};
        // Redirect-GUI horizontal fit-squish about center (<= 1): shrinks the GUI so the
        // per-eye plane shift can't clip its sides (dynamic3d "fit" strategy). 1 = off.
        float ui_fit_scale{1.0f};
        // Ghost/crosstalk reduction (output3d 3.4), applied last in the shader. Exact no-ops at
        // these defaults, and the shader branches on them so the untouched path stays bit-exact.
        float ghost_contrast{1.0f};    // < 1 squeezes toward mid-grey, shrinking |L-R|
        float ghost_black_floor{0.0f}; // > 0 raises the black floor ("foot-room" for cancellation)
        float cpad1{0.0f};
        float cpad2{0.0f};
        float cpad3{0.0f};
    };

    // Root constants are written as a raw dword blob, so the C++ struct and the HLSL cbuffer must
    // agree field-for-field. HLSL rounds a cbuffer up to a 16-byte multiple - keep the explicit
    // tail padding above in step with that so no field silently shifts register.
    static_assert(sizeof(RepackParams) == 128, "RepackParams must stay a 16-byte multiple and match the shader cbuffer");

    bool setup(ID3D12Device* device, DXGI_FORMAT output_format);
    void reset();

    bool is_initialized() const { return m_pso != nullptr; }
    DXGI_FORMAT get_output_format() const { return m_output_format; }
    Flat3DLeiaSR& leia() { return m_leia; }

    // left/right must be SRV-capable textures in PIXEL_SHADER_RESOURCE state
    // (the persistent eye caches). backbuffer is expected in PRESENT state.
    // window is the game window (used for the LeiaSR weaver only).
    // ui_tex (optional): plugin-extracted UI+alpha texture; takes the t2 slot (pre_left).
    // ui_depth_tex (optional): scene depth for the depth-adaptive UI mode; takes t3 (pre_right).
    void render(d3d12::TextureContext& left, d3d12::TextureContext& right, d3d12::TextureContext& pre_left,
        d3d12::TextureContext& pre_right, ID3D12Resource* backbuffer, uint32_t backbuffer_index,
        const RepackParams& params, HWND window, ID3D12Resource* ui_tex = nullptr, ID3D12Resource* ui_depth_tex = nullptr);

private:
    void update_srvs(ID3D12Device* device, ID3D12Resource* left, ID3D12Resource* right, ID3D12Resource* pre_left, ID3D12Resource* pre_right);
    D3D12_CPU_DESCRIPTOR_HANDLE update_rtv(ID3D12Device* device, ID3D12Resource* backbuffer, uint32_t index);
    bool ensure_leia_intermediate(ID3D12Device* device, uint32_t sbs_width, uint32_t height);
    void record_compose(ID3D12GraphicsCommandList* cmd_list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height, const RepackParams& params);

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D12RootSignature> m_root_signature{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_srv_heap{}; // 4 slots, shader-visible (left/right/pre-left/pre-right)
    ComPtr<ID3D12DescriptorHeap> m_rtv_heap{}; // one slot per swapchain buffer

    std::array<ID3D12Resource*, 4> m_last_eye_textures{}; // change tracking only, not owning
    std::array<ID3D12Resource*, 8> m_last_rtv_textures{}; // change tracking only, not owning

    std::array<d3d12::CommandContext, 3> m_commands{};

    // LeiaSR: full-SbS (2W x H) intermediate the weaver consumes, plus the weaver itself.
    d3d12::TextureContext m_leia_intermediate{};
    Flat3DLeiaSR m_leia{};

    DXGI_FORMAT m_output_format{DXGI_FORMAT_UNKNOWN};
    uint32_t m_srv_descriptor_size{0};
    uint32_t m_rtv_descriptor_size{0};
};
} // namespace vrmod
