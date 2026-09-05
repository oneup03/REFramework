#include <spdlog/spdlog.h>

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler") // D3DCOMPILER_47.dll is already delay-loaded project-wide

#include "REFramework.hpp"

#include "Flat3DCompose.hpp"

namespace vrmod {
// One fullscreen compose shader for every flatscreen 3D output mode. The
// interlaced/checkerboard patterns are selected from SV_Position (real output
// pixels) AFTER the per-eye sample, so they stay display-pixel-exact even when
// the eye textures are a different resolution than the backbuffer.
static const char g_flat3d_shader[] = R"(
cbuffer RepackParams : register(b0) {
    int2 out_size;
    int mode;
    int eye_swap;
    float scene_shift_uv;
    float scene_scale;
    float crop_origin_u;
    float crop_frac_u;
    int2 eye_size;
    int crosshair_enabled;
    int pad0;
    float crosshair_shift_uv;
    float crosshair_len_px;
    float crosshair_thick_px;
    float pad1;
    int gui_match_enabled;
    float gui_mask_threshold;
    int gui_match_debug;
    float gui_depth_shift; // per-eye disparity (eye-UV) applied oppositely to the matched GUI
    int ui_enabled;        // AFW extracted-UI composite: 0 off, 1 flat plane, 2 depth-adaptive (t3 = scene depth)
    float ui_shift_uv;     // per-eye UI disparity (eye-UV; 0 = screen depth); global offset in adaptive mode
    float ui_depth_scale;  // adaptive: shift = ui_depth_scale * device_depth + ui_disp_bias (+ ui_shift_uv)
    float ui_disp_bias;
    float content_frac_u;  // native-output override: engine content occupies this top-left fraction
    float content_frac_v;  // of the (forced-native) eye textures; (1,1) = full frame
    float ui_fit_scale;      // redirect-GUI horizontal fit-squish about center (<= 1); 1 = off
    float ghost_contrast;    // ghost/crosstalk reduction: 1 = off
    float ghost_black_floor; // ghost/crosstalk reduction: 0 = off
    float cpad1;
    float cpad2;
    float cpad3;
};

Texture2D eye_left : register(t0);
Texture2D eye_right : register(t1);
Texture2D eye_pre_left : register(t2);  // left eye, pre-overlay (scene only) - GUI isolation + depth base
Texture2D eye_pre_right : register(t3); // right eye, pre-overlay (scene only) - depth base
SamplerState samp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}

// The ONE place eye selection happens: eye_swap, the symmetric-projection
// convergence fallback (scene_shift/scene_scale), the 16:9 crop, and the
// depth-aware crosshair overlay all apply here so every mode behaves uniformly.
float4 SampleEye(int half_idx, float u, float v) {
    int eye = (eye_swap != 0) ? (1 - half_idx) : half_idx;

    float dir = (eye == 0) ? 1.0 : -1.0;
    float2 uv = float2((u - 0.5 - dir * scene_shift_uv) / scene_scale + 0.5,
                       (v - 0.5) / scene_scale + 0.5);

    uv.x = crop_origin_u + uv.x * crop_frac_u;

    // The convergence shift reveals a strip with no source at one edge per eye - render it as a
    // clean black bar (clamp-sampling would smear the edge pixels instead).
    if (uv.x < 0.0 || uv.x > 1.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    // Native-output override: the engine rendered into the top-left believed sub-region of the
    // forced-native texture - map the logical [0,1] onto it (the upscale happens right here).
    float2 uv_logical = uv; // pre-content_frac coords: extraction textures are content-sized
    uv *= float2(content_frac_u, content_frac_v);

    float4 c = (eye == 0) ? eye_left.Sample(samp, uv) : eye_right.Sample(samp, uv);

    // Overlay-RT redirect GUI: t2/t3 = the engine's GUI captured PER EYE into our own
    // transparent-cleared targets (premultiplied by construction: blended onto (0,0,0,0)).
    // Each half samples ITS OWN eye's capture: flat HUD is identical in both (stable, lands at
    // the GUI plane via ui_shift), while world-anchored markers carry their anchor's baked
    // per-eye parallax (correct depth, no alternation jitter). Forward map:
    //   screen_x = 0.5 + (src_x - 0.5) * ui_fit_scale + dir * ui_shift_uv
    // ui_fit_scale = 1 - 2|shift| squishes the GUI horizontally about center so the shift can
    // never push its sides offscreen (dynamic3d "fit" strategy). The GUI textures are
    // content-sized (engine target dims, fully covered): raw per-eye coords, no scene
    // shift/zoom/crop, no content_frac.
    // ui_enabled == 2: alpha-channel debug view (engine UI blend coverage diagnosis).
    if (ui_enabled != 0) {
        float2 uiuv = float2((u - 0.5 - dir * ui_shift_uv) / ui_fit_scale + 0.5, v);
        if (uiuv.x >= 0.0 && uiuv.x <= 1.0) {
            float4 uic = (eye == 0) ? eye_pre_left.Sample(samp, uiuv)
                                    : eye_pre_right.Sample(samp, uiuv);
            if (ui_enabled == 2) {
                return float4(uic.a, uic.a, uic.a, 1.0);
            }
            c.rgb = uic.rgb + c.rgb * (1.0 - uic.a);
        }
    }

    // Depth-aware reticle: drawn per eye at screen center +/- the disparity of
    // whatever the player is aiming at (dynamic3d 4.1, own-reticle strategy).
    if (crosshair_enabled != 0) {
        float2 cpos = float2(0.5 + dir * crosshair_shift_uv, 0.5);
        float2 d = abs(float2(u, v) - cpos) * float2(eye_size.x, eye_size.y);

        if ((d.x <= crosshair_thick_px && d.y <= crosshair_len_px) ||
            (d.y <= crosshair_thick_px && d.x <= crosshair_len_px)) {
            c.rgb = float3(0.2, 1.0, 0.2) * 0.85 + c.rgb * 0.15;
        }
    }

    // Force OPAQUE: the harvested output target carries the engine's alpha (A=0 in HUD/menu
    // regions). Writing that to the backbuffer makes DWM composite those pixels against the
    // desktop (borderless), so a black menu background shows as the bright desktop ("white where
    // black expected") and old frames bleed through. The final image must be fully opaque.
    return float4(c.rgb, 1.0);
}

float4 Anaglyph(int m, float4 cA, float4 cB) {
    if (m == 9) { // simple red-cyan
        return float4(cA.r, cB.g, cB.b, 1.0);
    }

    if (m == 10) { // Dubois red-cyan
        float r = saturate( 0.437*cA.r + 0.449*cA.g + 0.164*cA.b - 0.011*cB.r - 0.032*cB.g - 0.007*cB.b);
        float g = saturate(-0.062*cA.r - 0.062*cA.g - 0.024*cA.b + 0.377*cB.r + 0.761*cB.g + 0.009*cB.b);
        float b = saturate(-0.048*cA.r - 0.050*cA.g - 0.017*cA.b - 0.026*cB.r - 0.093*cB.g + 1.234*cB.b);
        return float4(r, g, b, 1.0);
    }

    if (m == 12) { // half-color compromise red-cyan
        float r = dot(float3( 0.439,  0.447,  0.148), cA.rgb);
        float g = dot(float3( 0.095,  0.934, -0.005), cB.rgb);
        float b = dot(float3(-0.018, -0.028,  1.057), cB.rgb);
        return float4(saturate(float3(r, g, b)), 1.0);
    }

    if (m == 13) { // simple green-magenta (left = green lens)
        return float4(cB.r, cA.g, cB.b, 1.0);
    }

    if (m == 14) { // Dubois green-magenta
        float r = saturate(-0.062*cA.r - 0.158*cA.g - 0.039*cA.b + 0.529*cB.r + 0.705*cB.g + 0.024*cB.b);
        float g = saturate( 0.284*cA.r + 0.668*cA.g + 0.143*cA.b - 0.016*cB.r - 0.015*cB.g + 0.065*cB.b);
        float b = saturate(-0.015*cA.r - 0.027*cA.g + 0.021*cA.b + 0.009*cB.r + 0.075*cB.g + 0.937*cB.b);
        return float4(r, g, b, 1.0);
    }

    // 16: blue-amber (ColorCode-style): amber (left) carries color, blue
    // (right) carries a luminance-weighted channel.
    return float4(cA.r, cA.g, dot(cB.rgb, float3(0.15, 0.30, 0.55)), 1.0);
}

// Ghost/crosstalk reduction (output3d 3.4). Every stereo display leaks some of each eye into the
// other, and how visible that leak is depends on the brightness DIFFERENCE between the eyes - so
// compressing the signal range before it reaches the display reduces what you see. Displays that
// CANCEL crosstalk (autostereo panels, the LeiaSR weaver) additionally pre-subtract a fraction of
// the fellow eye, which drives values below zero where the render target clamps them; the clamped
// part is exactly what survives as a ghost, and raising the black floor gives that subtraction the
// foot-room it needs.
//
// Deliberately GLOBAL and FIXED. Localizing it cannot work (ghosting IS inter-eye difference, so a
// correction applied unevenly manufactures more of it), and adapting it per frame is visibly worse
// than a slightly-wrong constant - the whole image pumps as content changes.
//
// The remap runs in LINEAR light, pivoting on 0.5 via a plain 2.2 gamma, because that is the space
// a cancelling display's own correction runs in - the two have to agree or this makes things worse.
float3 GhostReduce(float3 c) {
    if (ghost_contrast >= 1.0 && ghost_black_floor <= 0.0) {
        return c; // exact no-op at the defaults; keep the untouched path bit-exact
    }

    float3 lin = pow(saturate(c), 2.2);
    lin = (lin - 0.5) * ghost_contrast + 0.5;                    // squeeze toward mid-grey
    lin = lin * (1.0 - ghost_black_floor) + ghost_black_floor;   // raise the black floor
    return pow(saturate(lin), 1.0 / 2.2);
}

// Applied at every one of ps_main's returns, so it lands on the FINAL composed pixel in every
// mode - last, after all other colour work, which is what output3d 3.4 requires. It sits here
// rather than around a single Repack() call because fxc's flow analysis emits a spurious
// "potentially uninitialized" X4000 for any non-entry function that returns from inside an if.
float4 GhostOut(float4 c) {
    return float4(GhostReduce(c.rgb), c.a);
}

float4 ps_main(VSOut input) : SV_Target {
    float2 uv = input.uv;
    float2 pix = input.pos.xy;

    if (mode == 100) { return GhostOut(SampleEye(0, uv.x, uv.y)); } // debug: left only
    if (mode == 101) { return GhostOut(SampleEye(1, uv.x, uv.y)); } // debug: right only

    if (mode == 0 || mode == 5) { // SbS (LeiaSR consumes an SbS compose)
        int half_idx = (uv.x < 0.5) ? 0 : 1;
        float u_half = (uv.x < 0.5) ? (uv.x * 2.0) : ((uv.x - 0.5) * 2.0);

        // GUI-match: isolate the GUI by diffing the LEFT eye's post-overlay vs its pre-overlay
        // (scene-only), then paint that single isolated GUI over EACH eye's own scene-only base with an
        // opposite per-eye horizontal shift. Result: identical GUI in both eyes at an adjustable depth
        // (gui_depth_shift; 0 = screen depth), stereo background preserved, no clone-eye HUD ghosting.
        // (eye_swap not handled here; GUI source = eye_left.)
        if (gui_match_enabled != 0) {
            // Opposite shift per eye. GUI sampled from (natural - half_shift) so it lands shifted.
            float half_shift = (half_idx == 0) ? (0.5 * gui_depth_shift) : (-0.5 * gui_depth_shift);

            float2 srcL = float2((u_half - half_shift - 0.5 - scene_shift_uv) / scene_scale + 0.5,
                                 (uv.y - 0.5) / scene_scale + 0.5);
            srcL.x = crop_origin_u + srcL.x * crop_frac_u;

            float3 gpost = eye_left.Sample(samp, srcL).rgb;
            float3 gpre  = eye_pre_left.Sample(samp, srcL).rgb;
            float dm = max(max(abs(gpost.r - gpre.r), abs(gpost.g - gpre.g)), abs(gpost.b - gpre.b));
            float m = (dm > gui_mask_threshold) ? 1.0 : 0.0;

            if (gui_match_debug != 0) { return float4(m, m, m, 1.0); }

            // Scene base = this eye's own scene-only pre-overlay at the NATURAL (unshifted) position.
            float2 baseUV = float2((u_half - 0.5 - scene_shift_uv) / scene_scale + 0.5,
                                   (uv.y - 0.5) / scene_scale + 0.5);
            baseUV.x = crop_origin_u + baseUV.x * crop_frac_u;
            float3 sceneBase = (half_idx == 0) ? eye_pre_left.Sample(samp, baseUV).rgb
                                               : eye_pre_right.Sample(samp, baseUV).rgb;

            return GhostOut(float4(lerp(sceneBase, gpost, m), 1.0));
        }

        return GhostOut(SampleEye(half_idx, u_half, uv.y));
    }

    if (mode == 1) { // TaB
        if (uv.y < 0.5) { return GhostOut(SampleEye(0, uv.x, uv.y * 2.0)); }
        return GhostOut(SampleEye(1, uv.x, (uv.y - 0.5) * 2.0));
    }

    // Pattern from the OUTPUT pixel coordinate, after the (possibly
    // upscaling) sample above - display-pixel-exact at any render resolution.
    if (mode == 2) { return GhostOut(SampleEye(((int)pix.y) & 1, uv.x, uv.y)); }              // row interlaced
    if (mode == 3) { return GhostOut(SampleEye(((int)pix.x) & 1, uv.x, uv.y)); }              // column interlaced
    if (mode == 4) { return GhostOut(SampleEye(((int)pix.x + (int)pix.y) & 1, uv.x, uv.y)); } // checkerboard

    // Anaglyph family: every variant samples both full eyes.
    return GhostOut(Anaglyph(mode, SampleEye(0, uv.x, uv.y), SampleEye(1, uv.x, uv.y)));
}
)";

bool Flat3DCompose::setup(ID3D12Device* device, DXGI_FORMAT output_format) {
    spdlog::info("[VR] Setting up Flat3D compose (output format {})", (int)output_format);

    reset();

    if (device == nullptr) {
        spdlog::error("[VR] Flat3D compose: no device");
        return false;
    }

    m_output_format = output_format;

    // Shaders
    ComPtr<ID3DBlob> vs_blob{};
    ComPtr<ID3DBlob> ps_blob{};
    ComPtr<ID3DBlob> error_blob{};

    if (FAILED(D3DCompile(g_flat3d_shader, sizeof(g_flat3d_shader) - 1, nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs_blob, &error_blob))) {
        spdlog::error("[VR] Flat3D compose VS compile failed: {}", error_blob != nullptr ? (const char*)error_blob->GetBufferPointer() : "unknown error");
        return false;
    }

    error_blob.Reset();

    if (FAILED(D3DCompile(g_flat3d_shader, sizeof(g_flat3d_shader) - 1, nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps_blob, &error_blob))) {
        spdlog::error("[VR] Flat3D compose PS compile failed: {}", error_blob != nullptr ? (const char*)error_blob->GetBufferPointer() : "unknown error");
        return false;
    }

    // Root signature: [0] root constants b0, [1] SRV table t0-t1, static sampler s0
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 4;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER root_params[2]{};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_params[0].Constants.ShaderRegister = 0;
    root_params[0].Constants.RegisterSpace = 0;
    root_params[0].Constants.Num32BitValues = sizeof(RepackParams) / sizeof(uint32_t);
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[1].DescriptorTable.NumDescriptorRanges = 1;
    root_params[1].DescriptorTable.pDescriptorRanges = &srv_range;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = (UINT)std::size(root_params);
    rs_desc.pParameters = root_params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rs_blob{};
    ComPtr<ID3DBlob> rs_error{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_error))) {
        spdlog::error("[VR] Flat3D compose root signature serialize failed: {}", rs_error != nullptr ? (const char*)rs_error->GetBufferPointer() : "unknown error");
        return false;
    }

    if (FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_signature)))) {
        spdlog::error("[VR] Flat3D compose root signature creation failed");
        return false;
    }

    // PSO: vertexless fullscreen triangle, no depth, no blend
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_signature.Get();
    pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
    pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.InputLayout = {nullptr, 0};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = output_format;
    pso_desc.SampleDesc = {1, 0};

    if (FAILED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        spdlog::error("[VR] Flat3D compose PSO creation failed");
        return false;
    }

    // Descriptor heaps: eye SRVs must live contiguously in ONE shader-visible
    // heap (the per-texture heaps in TextureContext are separate), so we create
    // the SRVs directly into our own heap.
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.NumDescriptors = 4;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&m_srv_heap)))) {
        spdlog::error("[VR] Flat3D compose SRV heap creation failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = (UINT)m_last_rtv_textures.size();
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&m_rtv_heap)))) {
        spdlog::error("[VR] Flat3D compose RTV heap creation failed");
        return false;
    }

    m_srv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (auto& commands : m_commands) {
        if (!commands.setup(L"Flat3D Compose Commands")) {
            spdlog::error("[VR] Flat3D compose command context setup failed");
            return false;
        }
    }

    spdlog::info("[VR] Flat3D compose setup complete");
    return true;
}

void Flat3DCompose::reset() {
    for (auto& commands : m_commands) {
        commands.reset();
    }

    m_pso.Reset();
    m_root_signature.Reset();
    m_srv_heap.Reset();
    m_rtv_heap.Reset();

    m_leia_intermediate.reset();
    m_leia.destroy_weaver(); // the SRContext itself survives for the process lifetime

    m_last_eye_textures.fill(nullptr);
    m_last_rtv_textures.fill(nullptr);
    m_output_format = DXGI_FORMAT_UNKNOWN;
}

// Resting state for the LeiaSR SbS intermediate; the weaver samples it as a view texture.
static constexpr D3D12_RESOURCE_STATES LEIA_INTERMEDIATE_STATE =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// sbs_width is the FULL combined side-by-side width (2x the per-eye/backbuffer width).
bool Flat3DCompose::ensure_leia_intermediate(ID3D12Device* device, uint32_t sbs_width, uint32_t height) {
    if (m_leia_intermediate.texture != nullptr) {
        const auto desc = m_leia_intermediate.texture->GetDesc();

        if (desc.Width == sbs_width && desc.Height == height) {
            return true;
        }

        m_leia_intermediate.reset();
        m_leia.destroy_weaver(); // weaver holds a reference to the old input
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = sbs_width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ComPtr<ID3D12Resource> texture{};

    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, LEIA_INTERMEDIATE_STATE, nullptr,
            IID_PPV_ARGS(&texture)))) {
        spdlog::error("[Flat3D] Failed to create LeiaSR SbS intermediate");
        return false;
    }

    texture->SetName(L"Flat3D LeiaSR SbS Intermediate");

    if (!m_leia_intermediate.setup(device, texture.Get(), std::nullopt, std::nullopt, L"Flat3D LeiaSR Intermediate")) {
        spdlog::error("[Flat3D] Failed to set up LeiaSR intermediate RTV/SRV");
        m_leia_intermediate.reset();
        return false;
    }

    return true;
}

void Flat3DCompose::record_compose(ID3D12GraphicsCommandList* cmd_list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height, const RepackParams& params) {
    cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)width;
    viewport.Height = (float)height;
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;

    D3D12_RECT scissor{};
    scissor.right = (LONG)width;
    scissor.bottom = (LONG)height;

    cmd_list->RSSetViewports(1, &viewport);
    cmd_list->RSSetScissorRects(1, &scissor);

    cmd_list->SetGraphicsRootSignature(m_root_signature.Get());
    cmd_list->SetPipelineState(m_pso.Get());

    ID3D12DescriptorHeap* heaps[] = {m_srv_heap.Get()};
    cmd_list->SetDescriptorHeaps(1, heaps);
    cmd_list->SetGraphicsRoot32BitConstants(0, sizeof(RepackParams) / sizeof(uint32_t), &params, 0);
    cmd_list->SetGraphicsRootDescriptorTable(1, m_srv_heap->GetGPUDescriptorHandleForHeapStart());

    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_list->DrawInstanced(3, 1, 0, 0);
}

void Flat3DCompose::update_srvs(ID3D12Device* device, ID3D12Resource* left, ID3D12Resource* right, ID3D12Resource* pre_left, ID3D12Resource* pre_right) {
    const std::array<ID3D12Resource*, 4> textures{left, right, pre_left, pre_right};

    for (size_t i = 0; i < textures.size(); ++i) {
        if (textures[i] == nullptr || m_last_eye_textures[i] == textures[i]) {
            continue;
        }

        auto handle = m_srv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += i * m_srv_descriptor_size;

        device->CreateShaderResourceView(textures[i], nullptr, handle);
        m_last_eye_textures[i] = textures[i];
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE Flat3DCompose::update_rtv(ID3D12Device* device, ID3D12Resource* backbuffer, uint32_t index) {
    const auto slot = index % m_last_rtv_textures.size();

    auto handle = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += slot * m_rtv_descriptor_size;

    if (m_last_rtv_textures[slot] != backbuffer) {
        device->CreateRenderTargetView(backbuffer, nullptr, handle);
        m_last_rtv_textures[slot] = backbuffer;
    }

    return handle;
}

void Flat3DCompose::render(d3d12::TextureContext& left, d3d12::TextureContext& right, d3d12::TextureContext& pre_left,
    d3d12::TextureContext& pre_right, ID3D12Resource* backbuffer, uint32_t backbuffer_index,
    const RepackParams& params, HWND window, ID3D12Resource* ui_tex, ID3D12Resource* ui_depth_tex)
{
    if (!is_initialized() || backbuffer == nullptr || left.texture == nullptr || right.texture == nullptr) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();

    if (device == nullptr) {
        return;
    }

    // Fall back the pre-overlay slots to the eye textures when the caches aren't ready (never sampled
    // unless gui_match_enabled/ui_enabled), so the descriptor slots always hold valid SRVs.
    // The AFW extracted-UI texture takes the t2 slot when provided.
    auto* pre_left_res = ui_tex != nullptr ? ui_tex
        : (pre_left.texture != nullptr ? pre_left.texture.Get() : left.texture.Get());
    auto* pre_right_res = ui_depth_tex != nullptr ? ui_depth_tex
        : (pre_right.texture != nullptr ? pre_right.texture.Get() : right.texture.Get());
    update_srvs(device, left.texture.Get(), right.texture.Get(), pre_left_res, pre_right_res);
    const auto rtv = update_rtv(device, backbuffer, backbuffer_index);

    const auto bb_desc = backbuffer->GetDesc();
    const auto width = (uint32_t)bb_desc.Width;
    const auto height = (uint32_t)bb_desc.Height;

    // LeiaSR: compose SbS into an intermediate, then let the weaver interleave
    // it into the backbuffer using live eye tracking. Falls back to plain SbS
    // on the backbuffer whenever the weaver is unavailable.
    //
    // The intermediate is FULL side-by-side (2W x H), so each half is a
    // backbuffer-width eye view and the weaver samples it 1:1 instead of
    // horizontally upscaling a half-SbS source before the lenticular interleave.
    const auto want_leia = params.mode == MODE_LEIA_SR;
    const auto sbs_width = width * 2;
    auto leia_active = false;

    if (want_leia && ensure_leia_intermediate(device, sbs_width, height)) {
        leia_active = m_leia.init(device, window);
    }

    auto& commands = m_commands[backbuffer_index % m_commands.size()];
    commands.wait(INFINITE);

    std::scoped_lock _{commands.mtx};
    auto cmd_list = commands.cmd_list.Get();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (leia_active) {
        // 1. SbS into the intermediate
        barrier.Transition.pResource = m_leia_intermediate.texture.Get();
        barrier.Transition.StateBefore = LEIA_INTERMEDIATE_STATE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd_list->ResourceBarrier(1, &barrier);

        record_compose(cmd_list, m_leia_intermediate.get_rtv(), sbs_width, height, params);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = LEIA_INTERMEDIATE_STATE;
        cmd_list->ResourceBarrier(1, &barrier);

        // 2. Weave intermediate -> backbuffer. SR-lib reads the dimensions and
        // format off the resource desc, so there's nothing to describe here.
        m_leia.set_input(m_leia_intermediate.texture.Get());

        barrier.Transition.pResource = backbuffer;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd_list->ResourceBarrier(1, &barrier);

        cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.Width = (float)width;
        viewport.Height = (float)height;
        viewport.MinDepth = D3D12_MIN_DEPTH;
        viewport.MaxDepth = D3D12_MAX_DEPTH;

        D3D12_RECT scissor{};
        scissor.right = (LONG)width;
        scissor.bottom = (LONG)height;

        // D3D12 rasterizes against whatever RSSetViewports last set ON THE
        // COMMAND LIST, not against what the weaver is told. The compose above
        // left it at the 2W-wide intermediate; without this reset the weave
        // rasterizes at that width and the backbuffer shows only its left half.
        cmd_list->RSSetViewports(1, &viewport);
        cmd_list->RSSetScissorRects(1, &scissor);

        if (!m_leia.weave(cmd_list, viewport, scissor, m_output_format)) {
            // Weaver died mid-frame: draw plain SbS so the frame isn't lost.
            record_compose(cmd_list, rtv, width, height, params);
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd_list->ResourceBarrier(1, &barrier);
    } else {
        barrier.Transition.pResource = backbuffer;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd_list->ResourceBarrier(1, &barrier);

        record_compose(cmd_list, rtv, width, height, params);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd_list->ResourceBarrier(1, &barrier);
    }

    commands.has_commands = true;
    commands.execute();
}
} // namespace vrmod
