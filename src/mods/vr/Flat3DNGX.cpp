#include <map>

#include <d3dcompiler.h>
#include <wrl.h>

#include <spdlog/spdlog.h>
#include <safetyhook.hpp>

#include "../VR.hpp"

#include "d3d12/CommandContext.hpp"
#include "pdafw/NGXDecl.hpp"

// AFW depth/MV source: hook the game's OWN DLSS (nvngx) evaluate call and harvest the depth +
// motion vectors it feeds the upscaler - guaranteed real full-res per-frame data with exact MV
// scales, copied on the game's own command list in the state DLSS consumes them
// (NON_PIXEL_SHADER_RESOURCE). Ported from UEVR-3D's UpscaleHelper/VR.cpp NGX harvest.
// NOTE: requires the game's DLSS (or DLAA/Ray Reconstruction off-path) to be ENABLED in settings.

namespace flat3d_ngx {

static SafetyHookInline g_create_hook{};
static SafetyHookInline g_release_hook{};
static SafetyHookInline g_evaluate_hook{};
static bool g_installed = false;
static std::map<NVSDK_NGX_Handle*, NVSDK_NGX_Feature> g_non_dlss_handles{};

// Per-eye DLSS history ("ghosting fix", DLSS edition): the game's single DLSS feature accumulates
// ONE temporal history while AFR alternates eyes -> cross-eye contamination baked into every frame
// (the depth-squish / motion sickness). We create a SECOND feature with the same params and
// alternate handles per eye so each eye keeps its own history.
static NVSDK_NGX_Handle* g_second_handle = nullptr;
static NVSDK_NGX_Handle* g_primary_handle = nullptr;
static bool g_second_tried = false;

// Own MV compute pass, SYNTHETIC EYE-JUMP mode. Evidence (warp-separation-scale slider changes
// nothing) says the plugin's AlternateEyeWarping displacement comes from the MV FIELD itself, not
// CameraData - so during camera pans the raw MVs (eye jump + pan motion) contaminate the warp's
// disparity = the motion squish. Fix: generate a PURE per-pixel eye-jump field from depth + the two
// SAME-TICK cameras (no history, no raw-MV conventions): for each fresh-eye pixel, where does that
// surface point sit in the other eye's view this tick. Camera/object motion cannot pollute it.
static constexpr const char g_mv_correct_shader[] = R"(
cbuffer CorrectParams : register(b0) {
    float4x4 inv_vp_curr;   // clip (fresh eye, this tick) -> world
    float4x4 vp_target;     // world -> clip (target camera)
    float4x4 vp_prev;       // v2: world -> clip (frame N-1's actual camera - the prev textures' space)
    uint2 size;
    float2 inv_size;
    float2 delta_to_stored; // uv delta -> stored MV units (render_size / mv_scale)
    float obj_scale;        // object-motion term scale (0 = off)
    float pad_a;
    float pad_b;
    float pad_c;
};
Texture2D<float> depth_tex : register(t0);
Texture2D<float2> mv_prev_tex : register(t1);  // v2: LAST frame's raw MVs (camera + object)
Texture2D<float2> cam_prev_tex : register(t2); // v2: LAST frame's camera-only field
RWTexture2D<float2> out_tex : register(u0);

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) {
        return;
    }
    float2 uv = (float2(id.xy) + 0.5) * inv_size;
    float d = depth_tex[id.xy];
    // Homogeneous world point; no /w needed - the scale cancels in the projective division below.
    // Reversed-Z far (d=0) yields a point at infinity, where the eye jump naturally vanishes.
    float4 wp = mul(inv_vp_curr, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0));
    float4 pt = mul(vp_target, wp);
    float2 mv = float2(0.0, 0.0);
    if (abs(pt.w) > 1e-8) {
        float2 uvt = float2(pt.x / pt.w * 0.5 + 0.5, 0.5 - pt.y / pt.w * 0.5);
        mv = (uvt - uv) * delta_to_stored;
    }
    // v3 object-motion term (same-frame extraction - no history-texture lookup): THIS frame's raw
    // MV at this pixel minus the camera-only temporal flow (this surface projected into frame
    // N-1's ACTUAL camera) = the object's own 1-frame motion, ~0 on static pixels regardless of
    // camera pans. Added to the eye-jump so movers keep advancing in the warp's history layer
    // (fixes the half-rate character stutter) while the field stays free of camera-pan pollution
    // (the squish source). t1 = this frame's raw MVs; vp_prev = frame N-1's rendered camera.
    // Guard: implausibly large deltas (>~5% of the frame) are disocclusion/garbage - skip.
    if (obj_scale != 0.0) {
        float4 pp = mul(vp_prev, wp);
        if (abs(pp.w) > 1e-8) {
            float2 uvp = float2(pp.x / pp.w * 0.5 + 0.5, 0.5 - pp.y / pp.w * 0.5);
            float2 cam_flow = (uvp - uv) * delta_to_stored;
            float2 obj = mv_prev_tex[id.xy] - cam_flow;
            float2 obj_uv = obj / delta_to_stored;
            if (dot(obj_uv, obj_uv) < 0.0025) {
                mv += obj * obj_scale;
            }
        }
    }
    out_tex[id.xy] = mv;
}
)";

struct MVCorrectConstants {
    glm::mat4 inv_vp_curr;
    glm::mat4 vp_target;
    glm::mat4 vp_prev{1.0f};
    uint32_t size[2];
    float inv_size[2];
    float delta_to_stored[2];
    float obj_scale{0.0f};
    float pad_a{0.0f};
    float pad_b{0.0f};
    float pad_c{0.0f};
};
static_assert(sizeof(MVCorrectConstants) == 58 * 4);

class MVCorrectPass {
public:
    bool setup(ID3D12Device* device) {
        if (m_pso != nullptr) {
            return true;
        }
        if (m_failed) {
            return false;
        }
        m_failed = true; // until proven otherwise

        Microsoft::WRL::ComPtr<ID3DBlob> cs_blob{};
        Microsoft::WRL::ComPtr<ID3DBlob> error_blob{};
        if (FAILED(D3DCompile(g_mv_correct_shader, sizeof(g_mv_correct_shader) - 1, nullptr, nullptr, nullptr,
                "cs_main", "cs_5_0", 0, 0, &cs_blob, &error_blob))) {
            spdlog::error("[Flat3D-NGX] MV-correct CS compile failed: {}",
                error_blob != nullptr ? (const char*)error_blob->GetBufferPointer() : "unknown");
            return false;
        }

        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 3;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 3;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = sizeof(MVCorrectConstants) / 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 2;
        rs_desc.pParameters = params;

        Microsoft::WRL::ComPtr<ID3DBlob> rs_blob{};
        Microsoft::WRL::ComPtr<ID3DBlob> rs_error{};
        if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_error))
                || FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
            spdlog::error("[Flat3D-NGX] MV-correct root signature failed: {}",
                rs_error != nullptr ? (const char*)rs_error->GetBufferPointer() : "unknown");
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = m_root_sig.Get();
        pso_desc.CS = {cs_blob->GetBufferPointer(), cs_blob->GetBufferSize()};
        if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
            spdlog::error("[Flat3D-NGX] MV-correct PSO creation failed");
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 16; // 2 sets (warp field, DLSS feed) x 2 eyes x (depth, mv_prev, cam_prev SRVs + out UAV)
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
            spdlog::error("[Flat3D-NGX] MV-correct descriptor heap creation failed");
            return false;
        }

        // SetDescriptorHeaps REPLACES the full heap set - binding only our SRV heap would leave the
        // command list with NO sampler heap at DLSS-evaluate entry, a state no real game produces;
        // the driver's NGX path derefs the bound sampler heap unchecked -> the null-read AV we
        // crash-dumped. Keep a valid (unused) sampler heap bound alongside.
        D3D12_DESCRIPTOR_HEAP_DESC samp_desc{};
        samp_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samp_desc.NumDescriptors = 1;
        samp_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&samp_desc, IID_PPV_ARGS(&m_sampler_heap)))) {
            spdlog::error("[Flat3D-NGX] MV-correct sampler heap creation failed");
            return false;
        }

        D3D12_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        device->CreateSampler(&samp, m_sampler_heap->GetCPUDescriptorHandleForHeapStart());

        m_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_failed = false;
        spdlog::info("[Flat3D-NGX] MV-correct compute pass ready");
        return true;
    }

    void update_views(ID3D12Device* device, uint32_t set, uint32_t eye, ID3D12Resource* depth, ID3D12Resource* mv,
        ID3D12Resource* cam_prev, ID3D12Resource* corr, DXGI_FORMAT mv_fmt)
    {
        const uint32_t slot = set * 2 + eye;
        if (m_bound_depth[slot] == depth && m_bound_mv[slot] == mv && m_bound_cam[slot] == cam_prev
                && m_bound_corr[slot] == corr) {
            return;
        }

        auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (size_t)slot * 4 * m_inc;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(depth, &srv, cpu);

        cpu.ptr += m_inc;
        srv.Format = mv_fmt;
        device->CreateShaderResourceView(mv, &srv, cpu);

        cpu.ptr += m_inc;
        device->CreateShaderResourceView(cam_prev, &srv, cpu);

        cpu.ptr += m_inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = mv_fmt;
        device->CreateUnorderedAccessView(corr, nullptr, &uav, cpu);

        m_bound_depth[slot] = depth;
        m_bound_mv[slot] = mv;
        m_bound_cam[slot] = cam_prev;
        m_bound_corr[slot] = corr;
    }

    void dispatch(ID3D12GraphicsCommandList* cmd, uint32_t set, uint32_t eye, const MVCorrectConstants& c,
        ID3D12Resource* corr, D3D12_RESOURCE_STATES corr_state)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = corr;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = corr_state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cmd->ResourceBarrier(1, &b);

        ID3D12DescriptorHeap* heaps[]{m_heap.Get(), m_sampler_heap.Get()};
        cmd->SetDescriptorHeaps(2, heaps);
        cmd->SetComputeRootSignature(m_root_sig.Get());
        cmd->SetPipelineState(m_pso.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(MVCorrectConstants) / 4, &c, 0);
        auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += (size_t)(set * 2 + eye) * 4 * m_inc;
        cmd->SetComputeRootDescriptorTable(1, gpu);
        cmd->Dispatch((c.size[0] + 7) / 8, (c.size[1] + 7) / 8, 1);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmd->ResourceBarrier(1, &b);
    }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root_sig{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_sampler_heap{};
    uint32_t m_inc{0};
    bool m_failed{false};
    ID3D12Resource* m_bound_depth[4]{};
    ID3D12Resource* m_bound_mv[4]{};
    ID3D12Resource* m_bound_cam[4]{};
    ID3D12Resource* m_bound_corr[4]{};
};

static MVCorrectPass g_mv_correct{};

// Dedicated command context for the same-eye DLSS MV feed: recorded and submitted at
// evaluate-hook time (RHI thread only), queue-ordered before the game's DLSS list.
static d3d12::CommandContext g_dlss_mv_ctx{};
static bool g_dlss_mv_ctx_ready = false;

static NVSDK_NGX_Result hk_create(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out_handle)
{
    const auto result = g_create_hook.call<NVSDK_NGX_Result>(cmd, feature, params, out_handle);

    spdlog::info("[Flat3D-NGX] CreateFeature feature={} result={:x}", (int)feature, (uint32_t)result);

    if (out_handle != nullptr && *out_handle != nullptr
            && feature != NVSDK_NGX_Feature_SuperSampling && feature != NVSDK_NGX_Feature_RayReconstruction) {
        g_non_dlss_handles[*out_handle] = feature;
    }

    return result;
}

static NVSDK_NGX_Result hk_release(NVSDK_NGX_Handle* handle) {
    // If the game tears down the feature we cloned, release our clone too.
    if (handle == g_primary_handle && g_second_handle != nullptr) {
        g_release_hook.call<NVSDK_NGX_Result>(g_second_handle);
        g_second_handle = nullptr;
        g_primary_handle = nullptr;
        g_second_tried = false;
        spdlog::info("[Flat3D-NGX] released per-eye second DLSS feature");
    }

    const auto result = g_release_hook.call<NVSDK_NGX_Result>(handle);
    g_non_dlss_handles.erase(handle);
    return result;
}

static NVSDK_NGX_Result hk_evaluate(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* params, void* callback)
{
    auto& vr = VR::get();

    // Original MV resource to restore after the evaluate when the same-eye feed substituted it -
    // the game reuses its parameter object and must not inherit our texture.
    ID3D12Resource* restore_mv = nullptr;

    if (params != nullptr && !g_non_dlss_handles.contains((NVSDK_NGX_Handle*)handle)
            && vr->is_using_flat3d_afw()) {
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* mv = nullptr;
        ID3D12Resource* output = nullptr;
        float mv_scale[2]{1.0f, 1.0f};
        params->Get(NVSDK_NGX_Parameter_Depth, &depth);
        params->Get(NVSDK_NGX_Parameter_MotionVectors, &mv);
        params->Get(NVSDK_NGX_Parameter_Output, &output);
        params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mv_scale[0]);
        params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mv_scale[1]);

        auto& afw = vr->get_flat3d_afw();
        auto* renderer = afw.renderer();

        if (depth != nullptr && mv != nullptr && renderer != nullptr) {
            const auto dd = depth->GetDesc();
            const auto md = mv->GetDesc();

            static bool s_logged = false;
            if (!s_logged) {
                s_logged = true;
                D3D12_RESOURCE_DESC od{};
                if (output != nullptr) od = output->GetDesc();
                spdlog::info("[Flat3D-NGX] harvest: depth {}x{} fmt={} | mv {}x{} fmt={} | out {}x{} | mv_scale=({:.3f},{:.3f})",
                    (uint32_t)dd.Width, dd.Height, (uint32_t)dd.Format,
                    (uint32_t)md.Width, md.Height, (uint32_t)md.Format,
                    (uint32_t)od.Width, od.Height, mv_scale[0], mv_scale[1]);
            }

            // io depth is R32_FLOAT (typeless D32S8 can't be sampled by the warp); we do the planar
            // plane-0 copy OURSELVES on the game's command list with explicit barriers.
            if (afw.ensure_io_textures((uint32_t)dd.Width, dd.Height, DXGI_FORMAT_R32_FLOAT, md.Format)) {
                // Eye slot by render-frame parity (parity-locked to the eye on both render and
                // present sides, so slot mapping stays eye-consistent).
                const uint32_t eye = ((uint32_t)vr->get_render_frame_count() % 2 == (uint32_t)vr->get_left_eye_interval()) ? 0u : 1u;

                constexpr auto k_io_state = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                auto raw_copy = [&](ID3D12Resource* src_res, ID3D12Resource* dst_res) {
                    D3D12_RESOURCE_BARRIER b[2]{};
                    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b[0].Transition.pResource = src_res;
                    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    b[1].Transition.pResource = dst_res;
                    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b[1].Transition.StateBefore = k_io_state;
                    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    cmd->ResourceBarrier(2, b);

                    D3D12_TEXTURE_COPY_LOCATION s{};
                    s.pResource = src_res;
                    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    s.SubresourceIndex = 0;
                    D3D12_TEXTURE_COPY_LOCATION d{};
                    d.pResource = dst_res;
                    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    d.SubresourceIndex = 0;
                    cmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);

                    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
                    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
                    cmd->ResourceBarrier(2, b);
                };

                raw_copy(depth, afw.io_depth[eye].pTexture);
                raw_copy(mv, afw.io_mv[eye].pTexture);

                // NOTE: NO compute work on this (the game's DLSS) command list. Recording our
                // MV-correction dispatch here - or the plugin's - trips a deterministic null-deref
                // in the NV driver's deferred binding resolve (nvwgf2umx+0x6b9755, crash-dumped
                // twice, sampler heap bound or not). The correction runs at present time on the
                // plugin's own command list instead (flat3d_ngx_mv_correct_dispatch).
                afw.ngx_mv_scale_raw[0] = mv_scale[0];
                afw.ngx_mv_scale_raw[1] = mv_scale[1];

                // SAME-EYE MV FEED (the per-eye-history noise fix): each per-eye DLSS feature's
                // history is its own eye TWO frames back, but the game's MVs reference ONE frame
                // back (other eye) - the mismatch makes DLSS under-accumulate (confirmed: single
                // history = no noise). Substitute a same-eye N->N-2 field computed on OUR OWN
                // command list, submitted to the queue NOW - queue order places it before the
                // game's still-recording DLSS list, and it touches ONLY plugin-owned io textures
                // (no commands, no state on the game's list - the driver crash class can't occur).
                // Depth input = LAST frame's harvest (this frame's copy above executes later, with
                // the game's list); exact for static pixels, where the noise lives.
                if (vr->afw_per_eye_dlss_enabled() && vr->afw_dlss_mv_feed_enabled()
                        && afw.prev_frames >= 2 && vr->get_afw_frame().valid
                        && afw.io_mv_dlss[eye].pTexture != nullptr
                        && afw.io_depth[eye ^ 1].pTexture != nullptr
                        && md.Format == DXGI_FORMAT_R16G16_FLOAT) {
                    Microsoft::WRL::ComPtr<ID3D12Device> device{};
                    cmd->GetDevice(IID_PPV_ARGS(&device));

                    if (device != nullptr && g_mv_correct.setup(device.Get())) {
                        const auto& fr = vr->get_afw_frame_for((int32_t)vr->get_render_frame_count());

                        MVCorrectConstants c{};
                        c.inv_vp_curr = glm::inverse(vrmod::afw_to_reverse_z(fr.proj[eye]) * fr.view[eye]);
                        c.vp_target = vrmod::afw_to_reverse_z(afw.prev2_proj[eye]) * afw.prev2_view[eye]; // same eye, N-2
                        // Frame N-1's actual camera (other eye) - the space the v2 prev textures live in.
                        c.vp_prev = vrmod::afw_to_reverse_z(afw.prev_proj[eye ^ 1]) * afw.prev_view[eye ^ 1];
                        c.size[0] = afw.io_w;
                        c.size[1] = afw.io_h;
                        c.inv_size[0] = 1.0f / (float)afw.io_w;
                        c.inv_size[1] = 1.0f / (float)afw.io_h;
                        c.delta_to_stored[0] = (float)afw.io_w / (std::abs(mv_scale[0]) > 1e-6f ? mv_scale[0] : 1.0f);
                        c.delta_to_stored[1] = (float)afw.io_h / (std::abs(mv_scale[1]) > 1e-6f ? mv_scale[1] : 1.0f);

                        constexpr auto k_state = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                        if (!g_dlss_mv_ctx_ready) {
                            g_dlss_mv_ctx_ready = g_dlss_mv_ctx.setup(L"Flat3D DLSS same-eye MV feed");
                        }

                        // (v2 object-motion term retired: camera-only field alone tested more
                        // stable - no ghosting without it. obj_scale 0 disables the term.)
                        c.obj_scale = 0.0f;

                        if (g_dlss_mv_ctx_ready) {
                            g_dlss_mv_ctx.wait(INFINITE);
                            {
                                std::scoped_lock _{g_dlss_mv_ctx.mtx};
                                g_mv_correct.update_views(device.Get(), 1, eye, afw.io_depth[eye ^ 1].pTexture,
                                    afw.io_mv[eye ^ 1].pTexture,
                                    afw.io_mv[eye ^ 1].pTexture,
                                    afw.io_mv_dlss[eye].pTexture, md.Format);
                                g_mv_correct.dispatch(g_dlss_mv_ctx.cmd_list.Get(), 1, eye, c,
                                    afw.io_mv_dlss[eye].pTexture, k_state);
                                g_dlss_mv_ctx.has_commands = true;
                            }
                            g_dlss_mv_ctx.execute();

                            params->Set(NVSDK_NGX_Parameter_MotionVectors, afw.io_mv_dlss[eye].pTexture);
                            restore_mv = mv;

                            static bool s_feed_logged = false;
                            if (!s_feed_logged) {
                                s_feed_logged = true;
                                spdlog::info("[Flat3D-NGX] same-eye DLSS MV feed active (own queue-ordered list)");
                            }
                        }
                    }
                }

                // Scale to output-resolution pixel space like UEVR does.
                if (output != nullptr) {
                    const auto od = output->GetDesc();
                    afw.ngx_mv_scale[0] = mv_scale[0] * (float)od.Width / (float)md.Width;
                    afw.ngx_mv_scale[1] = mv_scale[1] * (float)od.Height / (float)md.Height;
                } else {
                    afw.ngx_mv_scale[0] = mv_scale[0];
                    afw.ngx_mv_scale[1] = mv_scale[1];
                }
                afw.ngx_last_frame = vr->get_render_frame_count();
            }
        }
    }

    // Per-eye DLSS history: create a second feature (same params) on first sight, then route the
    // odd-parity (right-eye) evaluates through it. Each eye accumulates its OWN temporal history -
    // removes the cross-eye contamination that reads as depth squish under AFR alternation.
    const NVSDK_NGX_Handle* use_handle = handle;

    if (params != nullptr && !g_non_dlss_handles.contains((NVSDK_NGX_Handle*)handle)
            && vr->is_using_flat3d_afw() && vr->afw_per_eye_dlss_enabled()) {
        if (!g_second_tried) {
            g_second_tried = true;
            NVSDK_NGX_Handle* h = nullptr;
            const auto cr = g_create_hook.call<NVSDK_NGX_Result>(cmd, NVSDK_NGX_Feature_SuperSampling, params, &h);
            if (h != nullptr) {
                g_second_handle = h;
                g_primary_handle = (NVSDK_NGX_Handle*)handle;
                spdlog::info("[Flat3D-NGX] created per-eye second DLSS feature (result={:x})", (uint32_t)cr);
            } else {
                spdlog::warn("[Flat3D-NGX] second DLSS feature creation failed (result={:x}) - single history", (uint32_t)cr);
            }
        }

        if (g_second_handle != nullptr && handle == g_primary_handle) {
            const uint32_t eye = ((uint32_t)vr->get_render_frame_count() % 2 == (uint32_t)vr->get_left_eye_interval()) ? 0u : 1u;
            if (eye == 1) {
                use_handle = g_second_handle;
            }
        }
    }

    const auto result = g_evaluate_hook.call<NVSDK_NGX_Result>(cmd, use_handle, params, callback);

    if (restore_mv != nullptr) {
        params->Set(NVSDK_NGX_Parameter_MotionVectors, restore_mv);
    }

    return result;
}

bool install_hooks() {
    if (g_installed) {
        return true;
    }

    auto ngx = GetModuleHandleW(L"_nvngx.dll");
    if (ngx == nullptr) {
        ngx = GetModuleHandleW(L"nvngx.dll");
    }
    if (ngx == nullptr) {
        return false; // DLSS not initialized by the game (yet, or disabled in settings)
    }

    const auto p_create = GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature");
    const auto p_release = GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature");
    const auto p_evaluate = GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature");

    if (p_create == nullptr || p_release == nullptr || p_evaluate == nullptr) {
        spdlog::error("[Flat3D-NGX] nvngx exports missing");
        g_installed = true; // don't retry forever
        return false;
    }

    auto r1 = safetyhook::InlineHook::create(p_create, (void*)hk_create);
    auto r2 = safetyhook::InlineHook::create(p_release, (void*)hk_release);
    auto r3 = safetyhook::InlineHook::create(p_evaluate, (void*)hk_evaluate);

    if (!r1 || !r2 || !r3) {
        spdlog::error("[Flat3D-NGX] hook creation failed");
        g_installed = true;
        return false;
    }

    g_create_hook = std::move(r1.value());
    g_release_hook = std::move(r2.value());
    g_evaluate_hook = std::move(r3.value());
    g_installed = true;
    spdlog::info("[Flat3D-NGX] DLSS harvest hooks installed");
    return true;
}

} // namespace flat3d_ngx

// Simple linkage shim for the call site in D3D12Component.
bool flat3d_ngx_install_hooks_shim() {
    return flat3d_ngx::install_hooks();
}

// Present-time synthetic eye-jump MV field: records the compute dispatch on the caller's
// (plugin/present-side) command list, never the game's DLSS list - see the note in hk_evaluate.
// Needs only this tick's depth + both same-tick cameras (no history). Returns true when
// io_mv_corr[eye] will hold the synthetic field this frame.
bool flat3d_ngx_mv_correct_dispatch(ID3D12GraphicsCommandList* cmd, uint32_t eye) {
    auto& vr = VR::get();
    auto& afw = vr->get_flat3d_afw();

    constexpr auto k_io_state = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (cmd == nullptr || eye > 1 || !afw.io_ready || !vr->get_afw_frame().valid
            || afw.io_mv_corr[eye].pTexture == nullptr || afw.io_depth[eye].pTexture == nullptr
            || afw.io_mv[eye].pTexture == nullptr || afw.io_mv_fmt != DXGI_FORMAT_R16G16_FLOAT
            || afw.io_w == 0 || afw.io_h == 0) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    cmd->GetDevice(IID_PPV_ARGS(&device));

    if (device == nullptr || !flat3d_ngx::g_mv_correct.setup(device.Get())) {
        return false;
    }

    const uint32_t other = eye ^ 1;
    const auto& fr = vr->get_afw_frame_for((int32_t)vr->get_render_frame_count());

    flat3d_ngx::MVCorrectConstants c{};
    // to_reverseZ: recorded projections are forward-Z, the depth we sample is reversed-Z.
    c.inv_vp_curr = glm::inverse(vrmod::afw_to_reverse_z(fr.proj[eye]) * fr.view[eye]);
    c.vp_target = vrmod::afw_to_reverse_z(fr.proj[other]) * fr.view[other]; // other eye, SAME tick

    // v3 object motion (movers anti-stutter): extract this frame's per-object motion (raw minus
    // camera temporal flow vs frame N-1's rendered camera = prev[other], recorded pre-roll) and
    // add it to the eye-jump. The plugin's IgnoreMotionThreshold gates small (foliage) motions.
    const float obj_scale = vr->afw_obj_motion_scale();
    if (obj_scale > 0.0f && afw.prev_frames >= 1) {
        c.vp_prev = vrmod::afw_to_reverse_z(afw.prev_proj[other]) * afw.prev_view[other];
        c.obj_scale = obj_scale;
    }

    c.size[0] = afw.io_w;
    c.size[1] = afw.io_h;
    c.inv_size[0] = 1.0f / (float)afw.io_w;
    c.inv_size[1] = 1.0f / (float)afw.io_h;
    c.delta_to_stored[0] = (float)afw.io_w / (std::abs(afw.ngx_mv_scale_raw[0]) > 1e-6f ? afw.ngx_mv_scale_raw[0] : 1.0f);
    c.delta_to_stored[1] = (float)afw.io_h / (std::abs(afw.ngx_mv_scale_raw[1]) > 1e-6f ? afw.ngx_mv_scale_raw[1] : 1.0f);

    flat3d_ngx::g_mv_correct.update_views(device.Get(), 0, eye, afw.io_depth[eye].pTexture,
        afw.io_mv[eye].pTexture, afw.io_mv[eye].pTexture /* cam_prev unused (obj_scale=0) */,
        afw.io_mv_corr[eye].pTexture, afw.io_mv_fmt);
    flat3d_ngx::g_mv_correct.dispatch(cmd, 0, eye, c, afw.io_mv_corr[eye].pTexture, k_io_state);

    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        spdlog::info("[Flat3D-NGX] synthetic eye-jump MV field active (present-time compute)");
    }

    return true;
}
