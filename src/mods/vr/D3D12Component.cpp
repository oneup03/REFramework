#include <openvr.h>
#include <utility/ScopeGuard.hpp>
#include <utility/Profiler.hpp>

#include "../VR.hpp"
#include "../TemporalUpscaler.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>

#include <d3dcompiler.h>

#include "d3d12/DirectXTK.hpp"

#include "Flat3DGuiRedirect.hpp"
#include "D3D12Component.hpp"

namespace vrmod {
vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    REF_PROFILE_FUNCTION();

    if (m_openvr.left_eye_tex[0].texture == nullptr || m_force_reset) {
        setup();
    }

    auto& hook = g_framework->get_d3d12_hook();
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};

    const auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer");
        return vr::VRCompositorError_None;
    }

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    // TODO: Correct this for the upscaler...?
    // Flat3D composes from the harvested per-eye CLONES (fill_flat3d_eye_converting), not from
    // this converted backbuffer copy - once the clones exist this block is pure redundant work
    // (a full backbuffer copy + shader conversion every frame) AND its commands.wait(INFINITE)
    // contends with the engine's now-synchronous rendering (delay-render off in true-sequential),
    // intermittently hard-freezing the present thread. Skip it whenever flat3d has both clones;
    // only the pre-clone fallback (which reads m_converted_eye_tex via eye_texture) still needs it.
    const bool flat3d_clones_ready = vr->is_using_flat3d()
        && vr->m_multipass.eye_textures[0] != nullptr && vr->m_multipass.eye_textures[1] != nullptr;

    if (!m_backbuffer_is_8bit && !flat3d_clones_ready && (!vr->is_using_multipass() || (vr->m_multipass.eye_textures[0] == nullptr || vr->m_multipass.eye_textures[1] == nullptr))) {
        auto& commands = m_backbuffer_copy_commands[backbuffer_index % m_backbuffer_copy_commands.size()];
        auto command_list = commands.cmd_list.Get();
        commands.wait(INFINITE);

        // Copy current backbuffer into our copy so we can use it as an SRV.
        commands.copy(backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);

        float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
        commands.clear_rtv(m_converted_eye_tex, clear_color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Convert the backbuffer to 8-bit.
        render_srv_to_rtv(command_list, m_backbuffer_copy, m_converted_eye_tex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        commands.execute();
    }

    auto eye_texture = m_backbuffer_is_8bit ? backbuffer : m_converted_eye_tex.texture;

    auto runtime = vr->get_runtime();

    // Sometimes this can happen if pipeline execution does not go exactly as planned
    // so we need to resynchronized or begin the frame again.
    if (runtime->ready()) {
        runtime->fix_frame();
    }

    const auto frame_count = vr->m_render_frame_count;
    const auto is_multipass = vr->is_using_multipass();


    // Flat3D is excluded: its eye_textures are the HDR scene-color clones, which
    // intentionally differ from the 8-bit per-eye caches (fill_flat3d_eye_converting
    // does the conversion). Matching the cache format to the clones here would trip
    // on_reset()+setup() every frame - a reset loop.
    if (is_multipass && !runtime->is_flat3d() && (vr->m_multipass.eye_textures[0] != nullptr && vr->m_multipass.eye_textures[1] != nullptr)) {
        const auto eye_desc = vr->m_multipass.eye_textures[0]->GetDesc();

        if (runtime->is_openxr()) {
            if (eye_desc.Format != m_openxr.last_format) {
                spdlog::info("[VR] OpenXR format changed from {} to {}", m_openxr.last_format, eye_desc.Format);
                m_openxr.create_swapchains();
            }
        } else {
            if (eye_desc.Format != m_openvr.last_format) {
                spdlog::info("[VR] OpenVR format changed from {} to {}", m_openvr.last_format, eye_desc.Format);
                on_reset(vr);
                setup();
            }
        }
    }

    // If m_frame_count is even, we're rendering the left eye.
    if (frame_count % 2 == vr->m_left_eye_interval && !is_multipass) {
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            m_openxr.copy(0, eye_texture.Get(), nullptr, D3D12_RESOURCE_STATE_PRESENT);
        }

        // Flat3D texture (persistent left eye cache; composed at the end of this function)
        if (runtime->is_flat3d()) {
            m_openvr.copy_left(eye_texture.Get());

            static uint32_t s_fl = 0;
            ++s_fl;
            if (vr->m_flat3d_afw_debug->value() && s_fl % 120 == 0) {
                spdlog::info("[Flat3D-AFW] fill LEFT rfc={} (L={} R={} so far)", frame_count, s_fl, 0);
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture (m_left_eye_tex0 holds the intermediate frame).
        if (runtime->is_openvr()) {
            m_openvr.copy_left(eye_texture.Get());

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::Texture_t left_eye{(void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &vr->m_left_bounds);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
        }
    } else {
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (is_multipass) {
                /*D3D12_BOX src_box{};
                src_box.back = 1;
                src_box.right = vr->get_hmd_width();
                src_box.bottom = vr->get_hmd_height();
                m_openxr.copy(0, (ID3D12Resource*)vr->m_multipass.eye_textures[0], &src_box, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                src_box.left = src_box.right;
                src_box.right *= 2;
                m_openxr.copy(1, (ID3D12Resource*)vr->m_multipass.eye_textures[0], &src_box, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);*/

                if (vr->m_multipass.eye_textures[0].Get() != nullptr && vr->m_multipass.eye_textures[1].Get() != nullptr) {
                    auto& ctx0 = vr->m_multipass.eye_contexts[0];
                    auto& ctx1 = vr->m_multipass.eye_contexts[1];

                    if (ctx0.texture.Get() != vr->m_multipass.eye_textures[0].Get()) {
                        ctx0.reset();
                        const auto desc = vr->m_multipass.eye_textures[0]->GetDesc();
                        ctx0.setup(device, vr->m_multipass.eye_textures[0].Get(), desc.Format, desc.Format);
                    }

                    if (ctx1.texture.Get() != vr->m_multipass.eye_textures[1].Get()) {
                        ctx1.reset();
                        const auto desc = vr->m_multipass.eye_textures[1]->GetDesc();
                        ctx1.setup(device, vr->m_multipass.eye_textures[1].Get(), desc.Format, desc.Format);
                    }

                    if (m_backbuffer_is_8bit) {
                        if (!TemporalUpscaler::get()->ready()) {
                            m_openxr.copy(0, vr->m_multipass.eye_textures[0].Get(), nullptr, D3D12_RESOURCE_STATE_COPY_DEST);
                            m_openxr.copy(1, vr->m_multipass.eye_textures[1].Get(), nullptr, D3D12_RESOURCE_STATE_COPY_DEST);
                        } else {
                            m_openxr.copy(0, vr->m_multipass.eye_textures[0].Get(), nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            m_openxr.copy(1, vr->m_multipass.eye_textures[1].Get(), nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        }
                    } else {
                        auto copy0fn = [&](d3d12::CommandContext& ctx, d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
                            const float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
                            ctx.clear_rtv(dst, clear_color, dst_state);
                            render_srv_to_rtv(ctx.cmd_list.Get(), vr->m_multipass.eye_contexts[0], dst, src_state, dst_state);
                        };

                        auto copy1fn = [&](d3d12::CommandContext& ctx, d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
                            const float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
                            ctx.clear_rtv(dst, clear_color, dst_state);
                            render_srv_to_rtv(ctx.cmd_list.Get(), vr->m_multipass.eye_contexts[1], dst, src_state, dst_state);
                        };

                        if (!TemporalUpscaler::get()->ready()) {
                            m_openxr.copy(0, ctx0.texture.Get(), nullptr, D3D12_RESOURCE_STATE_COPY_DEST, copy0fn);
                            m_openxr.copy(1, ctx1.texture.Get(), nullptr, D3D12_RESOURCE_STATE_COPY_DEST, copy1fn);
                        } else {
                            m_openxr.copy(0, ctx0.texture.Get(), nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, copy0fn);
                            m_openxr.copy(1, ctx1.texture.Get(), nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, copy1fn);
                        }
                    }
                } else {
                    // just copy the backbuffer to both eyes as a fallback
                    m_openxr.copy(0, eye_texture.Get(), nullptr, D3D12_RESOURCE_STATE_PRESENT);
                    m_openxr.copy(1, eye_texture.Get(), nullptr, D3D12_RESOURCE_STATE_PRESENT);
                }

                vr->m_multipass.eye_textures[0].Reset();
                vr->m_multipass.eye_textures[1].Reset();
            } else {
                m_openxr.copy(1, eye_texture.Get(), nullptr, D3D12_RESOURCE_STATE_PRESENT);
            }
        }

        // Flat3D (AFR): fill this parity's persistent eye cache from the backbuffer. The other
        // eye is synthesized by AFW (run_flat3d_afw) before the compose. texture_counter is never
        // advanced in flat3d - single-slot semantics keep the pair coherent.
        if (runtime->is_flat3d()) {
            m_openvr.copy_right(eye_texture.Get());
        }

        // OpenVR texture
        // Copy the back buffer to the right eye texture.
        if (runtime->is_openvr()) {
            if (is_multipass) {
                if (vr->m_multipass.eye_textures[0].Get() != nullptr && vr->m_multipass.eye_textures[1].Get() != nullptr) {
                    if (!TemporalUpscaler::get()->ready()) {
                        m_openvr.copy_left(vr->m_multipass.eye_textures[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                        m_openvr.copy_right(vr->m_multipass.eye_textures[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                    } else {
                        m_openvr.copy_left(vr->m_multipass.eye_textures[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        m_openvr.copy_right(vr->m_multipass.eye_textures[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    }
                } else {
                    // just copy the backbuffer to both eyes as a fallback
                    m_openvr.copy_left(eye_texture.Get());
                    m_openvr.copy_right(eye_texture.Get());
                }
            } else {
                m_openvr.copy_right(eye_texture.Get());
            }

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::Texture_t right_eye{(void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

            if (is_multipass) {
                vr::D3D12TextureData_t left {
                    m_openvr.get_left().texture.Get(),
                    command_queue,
                    0
                };

                vr::Texture_t left_eye{
                    (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto
                };

                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &vr->m_left_bounds);
                runtime->frame_synced = false;

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                    return e;
                }
            }

            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &vr->m_right_bounds);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }

            ++m_openvr.texture_counter;
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (frame_count % 2 == vr->m_right_eye_interval || is_multipass) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->ready() && runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE) {
            runtime->synchronize_frame();

            if (!runtime->got_first_poses) {
                runtime->update_poses();
            }
        }

        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame();
            }

            auto result = vr->m_openxr->end_frame();

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame();
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        // Never in flat3d - it would overwrite the stereo compose below with a stale frame.
        if (vr->m_desktop_fix->value() && !runtime->is_flat3d()) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                auto& copier = m_generic_copiers[frame_count % m_generic_copiers.size()];
                copier.wait(INFINITE);
                copier.copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                copier.execute();
            }
        }
    }

    // Flatscreen 3D: pack the freshest eye pair into the real backbuffer.
    // Runs on EVERY present - in AFR/sequential one eye is one present stale,
    // which is the standard AFR contract (never a blank eye).
    if (runtime->is_flat3d() && runtime->ready()) {
        // AFW P0: bind + init the PDAFWPlugin once the device/queue exist (idempotent, one attempt).
        vr->m_flat3d_afw.init(device, hook->get_command_queue());

        // NGX (DLSS) depth/MV harvest hooks - retried until the game loads nvngx (DLSS enabled).
        if (vr->m_flat3d_afw.is_available()) {
            extern bool flat3d_ngx_install_hooks_shim();
            flat3d_ngx_install_hooks_shim();
        }

        if (vr->m_flat3d_auto_convergence->value() || vr->m_flat3d_dynamic_crosshair->value()) {
            // Real depth for auto-convergence / adaptive crosshair: the DLSS-harvested io depth
            // (the engine-reflection clone reads zero on Wilds). This is what lights up the whole
            // dynamic3d layer - the control law has been implemented and starved all along.
            {
                auto& afw = vr->m_flat3d_afw;
                const uint32_t fresh = ((uint32_t)vr->m_render_frame_count % 2 == (uint32_t)vr->m_left_eye_interval) ? 0u : 1u;
                const bool ngx_ok = afw.io_ready
                    && (vr->m_render_frame_count - afw.ngx_last_frame) < 10
                    && afw.io_depth[fresh].pTexture != nullptr;
                vr->m_flat3d_depth_sampler.set_external_source(ngx_ok ? afw.io_depth[fresh].pTexture : nullptr);
            }

            vr->m_flat3d_depth_sampler.on_frame(vr->m_nearz);
        }

        DXGI_SWAP_CHAIN_DESC swap_desc{};
        swapchain->GetDesc(&swap_desc);

        // AFW: warp the missing AFR eye from this frame's fresh render + harvested depth/MV, into the
        // stale eye's cache, BEFORE the compose reads the pair.
        run_flat3d_afw(vr, backbuffer_index);

        const auto bb_desc = backbuffer->GetDesc();

        // Display-native check (output3d 3.1): interlaced/checkerboard/LeiaSR patterns must map
        // 1:1 to physical panel pixels - any DWM scaling AFTER present breaks them, and no shader
        // work can compensate. DXGI desktop coordinates are physical pixels (DPI-aware by
        // construction). The in-shader upscale-then-pattern path already handles render-res <
        // output-res correctly; this guards the present-to-panel step.
        if ((vr->m_render_frame_count % 300) == 1) {
            ComPtr<IDXGIOutput> dxgi_output{};
            if (SUCCEEDED(swapchain->GetContainingOutput(&dxgi_output)) && dxgi_output != nullptr) {
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(dxgi_output->GetDesc(&od))) {
                    const auto ow = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
                    const auto oh = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
                    const bool mismatch = (ow != (LONG)bb_desc.Width || oh != (LONG)bb_desc.Height);

                    if (mismatch != vr->m_flat3d_native_mismatch.load()) {
                        vr->m_flat3d_native_mismatch.store(mismatch);
                        if (mismatch) {
                            spdlog::warn("[Flat3D] Backbuffer {}x{} != display-native {}x{} - interlaced/"
                                "checkerboard/LeiaSR patterns will be rescaled by the compositor. Set the "
                                "game resolution to the desktop resolution (use DLSS render scaling for perf).",
                                bb_desc.Width, bb_desc.Height, ow, oh);
                        } else {
                            spdlog::info("[Flat3D] Backbuffer matches display-native {}x{}", ow, oh);
                        }
                    }
                }
            }
        }

        const auto params = build_flat3d_params(vr, (uint32_t)bb_desc.Width, (uint32_t)bb_desc.Height);
        m_flat3d_compose.render(m_openvr.get_left(), m_openvr.get_right(), m_flat3d_pre_left_cache, m_flat3d_pre_right_cache, backbuffer.Get(), backbuffer_index, params, swap_desc.OutputWindow, m_flat3d_afw_ui_tex, m_flat3d_afw_ui_depth_tex);

        // Consumed this frame's draw-time fills; next frame must re-fill.
        m_flat3d_eye_filled = {};
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

void D3D12Component::on_post_present(VR* vr) {
}

void D3D12Component::flat3d_ui_barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (res == nullptr || from == to) {
        return;
    }

    m_flat3d_ui_barrier_cmd.wait(INFINITE); // reset the list to recording

    if (m_flat3d_ui_barrier_cmd.cmd_list == nullptr) {
        return;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    m_flat3d_ui_barrier_cmd.cmd_list->ResourceBarrier(1, &b);

    m_flat3d_ui_barrier_cmd.has_commands = true;
    m_flat3d_ui_barrier_cmd.execute();
}

void D3D12Component::back_reserved_texture(ID3D12Resource* native) {
    if (native == nullptr || m_backed_reserved_natives.count(native) != 0) {
        return;
    }

    // Only RESERVED (tiled) resources need tile mapping. Committed/placed resources report layout
    // UNKNOWN/ROW_MAJOR; reserved report 64KB_UNDEFINED/STANDARD_SWIZZLE (what MHWilds create_texture
    // hands back). Mark others done so we don't re-inspect every frame.
    const auto desc = native->GetDesc();
    if (desc.Layout != D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE &&
        desc.Layout != D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE) {
        m_backed_reserved_natives.insert(native);
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto queue = hook->get_command_queue();
    if (device == nullptr || queue == nullptr) {
        return;
    }

    UINT num_tiles = 0;
    D3D12_PACKED_MIP_INFO packed{};
    D3D12_TILE_SHAPE tile_shape{};
    UINT num_subres = 1;
    D3D12_SUBRESOURCE_TILING subres_tiling{};
    device->GetResourceTiling(native, &num_tiles, &packed, &tile_shape, &num_subres, 0, &subres_tiling);

    if (num_tiles == 0) {
        m_backed_reserved_natives.insert(native);
        return;
    }

    // Dedicated heap sized to the whole resource's tiles (64KB each). RT/DS-texture heap flag matches
    // our clones (ALLOW_RENDER_TARGET). Kept alive in m_reserved_tile_heaps for the mapping's lifetime.
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = (UINT64)num_tiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = 0;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;

    ComPtr<ID3D12Heap> heap;
    if (FAILED(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)))) {
        spdlog::error("[Flat3D] back_reserved_texture: CreateHeap failed for native={:x} ({} tiles)",
            (uintptr_t)native, num_tiles);
        return;
    }

    // Map ALL of subresource 0's tiles linearly onto the heap.
    D3D12_TILED_RESOURCE_COORDINATE start{}; // (0,0,0), subresource 0
    D3D12_TILE_REGION_SIZE region{};
    region.NumTiles = num_tiles;
    region.UseBox = FALSE;
    const D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
    const UINT heap_offset = 0;
    const UINT range_tile_count = num_tiles;

    queue->UpdateTileMappings(native, 1, &start, &region, heap.Get(), 1, &range_flags,
        &heap_offset, &range_tile_count, D3D12_TILE_MAPPING_FLAG_NONE);

    m_reserved_tile_heaps.push_back(heap);
    m_backed_reserved_natives.insert(native);

    spdlog::info("[Flat3D] back_reserved_texture: mapped {} tiles ({} MB) for native={:x} (fmt={} {}x{})",
        num_tiles, hd.SizeInBytes / (1024 * 1024), (uintptr_t)native, (uint32_t)desc.Format,
        (uint32_t)desc.Width, desc.Height);
}

void D3D12Component::fill_flat3d_eye_converting(ID3D12Resource* src, uint32_t eye, D3D12_RESOURCE_STATES src_state) {
    if (src == nullptr || eye > 1) {
        return;
    }


    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();

    if (device == nullptr) {
        return;
    }

    // Wrap the engine's raw scene-color resource in an SRV/RTV view (no copy - we
    // read it directly). Re-wrap only when the resource pointer changes.
    auto& srcctx = m_flat3d_src[eye];
    if (srcctx.texture.Get() != src) {
        srcctx.reset();
        const auto desc = src->GetDesc();
        const bool ok = srcctx.setup(device, src, desc.Format, desc.Format);

        static std::array<bool, 2> s_logged{};
        if (!s_logged[eye]) {
            s_logged[eye] = true;
            spdlog::info("[Flat3D-Blit] eye={} src={:x} {}x{} fmt={} dim={} flags={:x} srv_setup={} rtv={:x}",
                eye, (uintptr_t)src, (uint32_t)desc.Width, desc.Height, (uint32_t)desc.Format,
                (uint32_t)desc.Dimension, (uint32_t)desc.Flags, ok, (uintptr_t)(ok ? srcctx.get_rtv().ptr : 0));
        }

        if (!ok) {
            return;
        }
    }

    auto& dst = eye == 0 ? m_openvr.acquire_left() : m_openvr.acquire_right();
    auto* command_list = dst.commands.cmd_list.Get();

    // In multipass the source is the engine's per-eye scene-color CLONE, filled via
    // RenderContext::copy_texture (VR::on_prepare_output_layer_draw). That copy is a
    // real engine command with proper state tracking, so the clone is a safe, stable
    // resource - unlike the raw UAV scene color, which crashed when read externally.
    // It rests in src_state (COPY_DEST after copy_texture, UNORDERED_ACCESS under the
    // upscaler); render_srv_to_rtv needs it shader-readable, so transition it here and
    // restore afterwards to keep the engine's next-frame copy_texture consistent.
    // (src_state == PIXEL_SHADER_RESOURCE means "already readable" - the AFR/backbuffer
    // fallback path, no barrier needed.)
    const bool needs_transition = src_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    if (needs_transition) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = src;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = src_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        command_list->ResourceBarrier(1, &barrier);
    }

    // Blit the (now shader-readable) source into the cache, format-converting
    // (e.g. R11G11B10_FLOAT -> 8-bit).
    render_srv_to_rtv(command_list, srcctx, dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (needs_transition) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = src;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = src_state;
        command_list->ResourceBarrier(1, &barrier);
    }

    dst.commands.has_commands = true;
    dst.commands.execute();

    m_flat3d_eye_filled[eye] = true;
}

void D3D12Component::fill_flat3d_pre(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state, bool is_left) {
    auto& srcctx = is_left ? m_flat3d_pre_src : m_flat3d_pre_right_src;
    auto& dst = is_left ? m_flat3d_pre_left_cache : m_flat3d_pre_right_cache;

    if (src == nullptr || dst.texture == nullptr) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    if (device == nullptr) {
        return;
    }

    // Wrap the pre-overlay scene-only clone in an SRV/RTV view; re-wrap on pointer change.
    if (srcctx.texture.Get() != src) {
        srcctx.reset();
        const auto desc = src->GetDesc();
        if (!srcctx.setup(device, src, desc.Format, desc.Format)) {
            return;
        }
    }
    dst.commands.wait(INFINITE);
    auto* command_list = dst.commands.cmd_list.Get();

    const bool needs_transition = src_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    if (needs_transition) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = src;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = src_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        command_list->ResourceBarrier(1, &barrier);
    }

    render_srv_to_rtv(command_list, srcctx, dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (needs_transition) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = src;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = src_state;
        command_list->ResourceBarrier(1, &barrier);
    }

    dst.commands.has_commands = true;
    dst.commands.execute();
}

void D3D12Component::afw_snapshot_depth_mv(VR* vr, ID3D12Resource* live_depth, ID3D12Resource* live_mv) {
    if (live_depth == nullptr || live_mv == nullptr) {
        return;
    }

    // NGX (DLSS) harvest supersedes this fallback entirely (the engine-reflection depth is a decoy
    // on Wilds anyway) - skip the work when NGX inputs are fresh.
    auto& afw = vr->m_flat3d_afw;
    if (afw.io_ready && (vr->m_render_frame_count - afw.ngx_last_frame) < 10) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook != nullptr ? hook->get_device() : nullptr;

    if (device == nullptr) {
        return;
    }

    const auto ld = live_depth->GetDesc();
    const auto lm = live_mv->GetDesc();

    // Same steady-state gate as the warp (menus/loading = tiny or absent depth) - RELATIVE to the
    // engine output size (the absolute 1280 silently starved AFW at sub-desktop resolutions).
    {
        auto& hookp = g_framework->get_d3d12_hook();
        uint32_t out_w = hookp->get_engine_believed_width();

        if (out_w == 0) {
            out_w = hookp->get_display_width();
        }

        const uint32_t min_w = std::max(out_w / 2u, 320u);

        if (ld.Width < min_w || lm.Width != ld.Width || lm.Height != ld.Height) {
            return;
        }
    }

    // R32_FLOAT depth snapshot target (planar CopyTextureRegion from D32S8 plane 0 is legal).
    if (m_afw_depth_local == nullptr
            || m_afw_depth_local->GetDesc().Width != ld.Width
            || m_afw_depth_local->GetDesc().Height != ld.Height) {
        m_afw_depth_local.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto rdesc = ld;
        rdesc.Format = DXGI_FORMAT_R32_FLOAT;
        rdesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        rdesc.MipLevels = 1;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(m_afw_depth_local.GetAddressOf())))) {
            spdlog::error("[Flat3D-AFW] R32 depth snapshot creation failed");
            return;
        }
        m_afw_depth_local->SetName(L"Flat3D AFW Depth R32");
    }

    if (m_afw_mv_local == nullptr
            || m_afw_mv_local->GetDesc().Width != lm.Width
            || m_afw_mv_local->GetDesc().Height != lm.Height
            || m_afw_mv_local->GetDesc().Format != lm.Format) {
        m_afw_mv_local.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto mdesc = lm;
        mdesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        mdesc.MipLevels = 1;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &mdesc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(m_afw_mv_local.GetAddressOf())))) {
            spdlog::error("[Flat3D-AFW] MV snapshot creation failed");
            return;
        }
        m_afw_mv_local->SetName(L"Flat3D AFW MV Local");
    }

    // Copies execute immediately on the engine queue: at hook time this lands right after the
    // PREVIOUS frame's GPU work. (NOTE: this whole fallback is superseded by the NGX harvest and
    // skipped while NGX inputs are fresh.)
    auto& copier = m_generic_copiers[1 % m_generic_copiers.size()];
    copier.wait(INFINITE);
    copier.copy_region(live_depth, m_afw_depth_local.Get(), nullptr,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON);
    copier.execute();
    auto& copier2 = m_generic_copiers[2 % m_generic_copiers.size()];
    copier2.wait(INFINITE);
    copier2.copy(live_mv, m_afw_mv_local.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    copier2.execute();
}

namespace {
// Foreground depth dilation for the AFW warp (the silhouette disocclusion-halo fix): a reversed-Z
// MAX filter widens every foreground silhouette by `radius` pixels, so edge pixels reproject
// coherently WITH the object instead of flickering between foreground/background assignment
// (standard timewarp/spacewarp mitigation). Runs as compute on the PLUGIN's present-time command
// list - the crash-safe location (never the game's DLSS list) - and binds the plugin-created
// textures via their handles in the plugin's own shader-visible view heap.
struct Flat3DDepthDilatePass {
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D12RootSignature> root{};
    ComPtr<ID3D12PipelineState> pso{};
    bool failed{false};

    bool ensure(ID3D12Device* device) {
        if (pso != nullptr) {
            return true;
        }
        if (failed || device == nullptr) {
            return false;
        }

        static const char k_src[] = R"(
cbuffer C : register(b0) { int radius; int width; int height; int pad; }
Texture2D<float> src_depth : register(t0);
RWTexture2D<float> dst_depth : register(u0);
[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)width || id.y >= (uint)height) { return; }
    float m = 0.0;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            int2 p = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(width - 1, height - 1));
            m = max(m, src_depth[p]); // reversed-Z: max = nearest = foreground dilate
        }
    }
    dst_depth[id.xy] = m;
}
)";

        ComPtr<ID3DBlob> cs{};
        ComPtr<ID3DBlob> err{};
        if (FAILED(D3DCompile(k_src, sizeof(k_src) - 1, nullptr, nullptr, nullptr, "cs_main", "cs_5_0", 0, 0, &cs, &err))) {
            spdlog::error("[Flat3D-AFW] depth-dilate CS compile failed: {}",
                err != nullptr ? (const char*)err->GetBufferPointer() : "(no log)");
            failed = true;
            return false;
        }

        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1;
        srv_range.BaseShaderRegister = 0;

        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors = 1;
        uav_range.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 4;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srv_range;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uav_range;

        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 3;
        rs.pParameters = params;

        ComPtr<ID3DBlob> rs_blob{};
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err))
            || FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root)))) {
            spdlog::error("[Flat3D-AFW] depth-dilate root signature creation failed");
            failed = true;
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pd_{};
        pd_.pRootSignature = root.Get();
        pd_.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};

        if (FAILED(device->CreateComputePipelineState(&pd_, IID_PPV_ARGS(&pso)))) {
            spdlog::error("[Flat3D-AFW] depth-dilate PSO creation failed");
            failed = true;
            return false;
        }

        spdlog::info("[Flat3D-AFW] depth-dilate pass ready");
        return true;
    }

    void dispatch(ID3D12GraphicsCommandList* cmd, pd::D3D12RendererAPI* renderer,
        const pd::TextureDesc& src, const pd::TextureDesc& dst, uint32_t w, uint32_t h, int radius) {
        constexpr auto k_rest = (D3D12_RESOURCE_STATES)(
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst.pTexture;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = k_rest;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cmd->ResourceBarrier(1, &b);

        ID3D12DescriptorHeap* heaps[]{renderer->GetViewHeap()};
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetPipelineState(pso.Get());

        const int32_t consts[4]{radius, (int32_t)w, (int32_t)h, 0};
        cmd->SetComputeRoot32BitConstants(0, 4, consts, 0);
        cmd->SetComputeRootDescriptorTable(1, src.shaderResourceViewHandle);
        cmd->SetComputeRootDescriptorTable(2, dst.unorderedAccessViewHandle);
        cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = k_rest;
        cmd->ResourceBarrier(1, &b);
    }
};
} // namespace

void D3D12Component::run_flat3d_afw(VR* vr, uint32_t backbuffer_index) {
    // Cleared up front so any early-out below leaves no stale/dangling UI pointers for the compose.
    m_flat3d_afw_ui_tex = nullptr;
    m_flat3d_afw_ui_depth_tex = nullptr;

    if (!vr->is_using_flat3d_afw() || vr->is_using_multipass()) {
        return;
    }

    auto& afw = vr->m_flat3d_afw;
    auto* renderer = afw.renderer();

    if (renderer == nullptr || !vr->m_afw_frame.valid
            || vr->m_afw_depth_tex.Get() == nullptr || vr->m_afw_mv_tex.Get() == nullptr) {
        return;
    }

    static int s_steplog = 0;
    const bool sl = s_steplog < 2;
    if (sl) { ++s_steplog; }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();

    if (device == nullptr) {
        return;
    }

    // Snapshot the LIVE engine depth/MV into our own committed copies, with our own command list +
    // explicit barriers (frame is complete at present; the engine's copy path crashes on Wilds).
    // Assumed resting states: depth = DEPTH_WRITE, MV = RENDER_TARGET (transitioned there and back).
    const auto live_depth = vr->m_afw_depth_tex.Get();
    const auto live_mv = vr->m_afw_mv_tex.Get();

    // Steady-state gate: only warp with a plausible gameplay depth (menus/loading render tiny or
    // no depth - warping those produced garbage). Threshold is RELATIVE to the engine's output
    // size - the old absolute 1280 was tuned for 4K output and silently disabled AFW whenever the
    // game resolution (and with it the DLSS render res) dropped below it.
    {
        const auto ld = live_depth->GetDesc();
        const auto lm = live_mv->GetDesc();

        auto& hookp = g_framework->get_d3d12_hook();
        uint32_t out_w = hookp->get_engine_believed_width();

        if (out_w == 0) {
            out_w = hookp->get_display_width();
        }

        const uint32_t min_w = std::max(out_w / 2u, 320u);

        if (ld.Width < min_w || lm.Width != ld.Width || lm.Height != ld.Height) {
            return;
        }
    }

    // Locals are filled by afw_snapshot_depth_mv at the overlay hook (mid-frame, before the engine
    // clears depth); nothing to snapshot here.
    if (m_afw_depth_local == nullptr || m_afw_mv_local == nullptr) {
        return;
    }

    // (Depth/MV snapshot happens in afw_snapshot_depth_mv at the overlay hook.)

    // Index by the SAME parity the AFR fill above used (frame_count % 2 == left_interval -> left).
    // The hooks run a frame ahead ((count+1)%2) - using their parity targeted the WRONG cache
    // (warp overwrote the fresh eye; both halves showed one eye).
    const uint32_t fresh = ((uint32_t)vr->m_render_frame_count % 2 == (uint32_t)vr->m_left_eye_interval) ? 0u : 1u;
    const uint32_t other = fresh ^ 1;

    auto& fresh_cache = (fresh == 0) ? m_openvr.get_left() : m_openvr.get_right();
    auto& other_cache = (other == 0) ? m_openvr.get_left() : m_openvr.get_right();

    if (fresh_cache.texture == nullptr || other_cache.texture == nullptr) {
        return;
    }

    const auto cache_desc = fresh_cache.texture->GetDesc();

    // Native-output override: the engine renders into the top-left BELIEVED sub-region of the
    // forced-native buffers - run the whole warp at the believed size (region copies in/out, so
    // the warp always operates on real content); the compose upscales afterward.
    uint32_t content_w = (uint32_t)cache_desc.Width;
    uint32_t content_h = cache_desc.Height;
    {
        auto& hookp = g_framework->get_d3d12_hook();
        const auto bw = hookp->get_engine_believed_width();
        const auto bh = hookp->get_engine_believed_height();

        if (bw != 0 && bh != 0 && bw <= content_w && bh <= content_h) {
            content_w = bw;
            content_h = bh;
        }
    }

    if (sl) spdlog::info("[Flat3D-AFW] warp step: ensure_eye_buffers...");
    if (!afw.ensure_eye_buffers(content_w, content_h, cache_desc.Format, cache_desc.Format)) {
        return;
    }

    auto& eye_fb = afw.eye_buffers().eyeFrameBuffers[fresh];
    auto& other_fb = afw.eye_buffers().eyeFrameBuffers[other];

    if (eye_fb.color.pTexture == nullptr || other_fb.color.pTexture == nullptr) {
        return;
    }

    // Wrap our resources as plugin TextureDescs (SRV setup done by the plugin renderer); re-wrap on
    // resource change. initialState tells the plugin what to transition from/to.
    static std::array<pd::TextureDesc, 2> s_color_desc{}; // per eye slot
    static pd::TextureDesc s_depth_desc{};
    static pd::TextureDesc s_mv_desc{};
    // Declared here (not at their use sites) so the epoch check below can invalidate every one of
    // them before anything reads them this frame.
    static pd::TextureDesc s_pdepth{};
    static pd::TextureDesc s_pmv{};
    static uint64_t s_dims = 0;
    static pd::TextureDesc s_hudless_src{};
    static pd::TextureDesc s_post_src{};

    // A device reset destroys everything these caches point at, and none of their own guards can
    // tell: `wrap` compares pointer identity, the plugin depth/MV block compares s_dims plus a
    // non-null check, and at an unchanged resolution all of those still pass while the resources
    // are dead. Flat3DAFW bumps reset_epoch on every reset - drop the caches when it moves so they
    // are rebuilt instead of copied into. (Without this, Flat3DAFW::on_device_reset would leave the
    // warp in the WORST state: freshly rebuilt class-owned buffers feeding stale plugin textures.)
    static uint32_t s_epoch = 0;

    if (s_epoch != afw.reset_epoch) {
        s_epoch = afw.reset_epoch;
        s_color_desc = {};
        s_depth_desc = pd::TextureDesc{};
        s_mv_desc = pd::TextureDesc{};
        s_pdepth = pd::TextureDesc{};
        s_pmv = pd::TextureDesc{};
        s_dims = 0;
        s_hudless_src = pd::TextureDesc{};
        s_post_src = pd::TextureDesc{};
        spdlog::info("[Flat3D-AFW] device reset - dropped cached plugin texture descs");
    }

    auto wrap = [&](pd::TextureDesc& desc, ID3D12Resource* res, pd::ImageType type, D3D12_RESOURCE_STATES state) {
        if (desc.pTexture != res) {
            desc = pd::TextureDesc{};
            desc.type = type;
            desc.pTexture = res;
            desc.initialState = state;
            renderer->SetupTextureDesc(desc);
        }
    };

    wrap(s_color_desc[fresh], fresh_cache.texture.Get(), pd::Image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    wrap(s_depth_desc, m_afw_depth_local.Get(), pd::Image, D3D12_RESOURCE_STATE_COMMON); // Image, not Depth: matches UEVR-3D
    wrap(s_mv_desc, m_afw_mv_local.Get(), pd::Image, D3D12_RESOURCE_STATE_COMMON);

    // SAMPLED inputs must be PLUGIN-CREATED textures (UEVR only ever samples plugin-owned textures;
    // externally-wrapped SetupTextureDesc resources are used as copy sources only - handing them to
    // the warp made it sample null descriptors = zero depth = mono copy). Create plugin-side
    // depth/MV and copy our snapshots into them each frame.
    {
        const auto dd = m_afw_depth_local->GetDesc();
        const auto md = m_afw_mv_local->GetDesc();
        const uint64_t dims = dd.Width ^ ((uint64_t)dd.Height << 20) ^ ((uint64_t)md.Format << 44);
        if (dims != s_dims || s_pdepth.pTexture == nullptr || s_pmv.pTexture == nullptr) {
            s_dims = dims;
            s_pdepth = pd::TextureDesc{}; // type stays Image (matches UEVR-3D's depthDesc)
            if (!renderer->CreateTexture((int)dd.Width, (int)dd.Height, DXGI_FORMAT_R32_FLOAT,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    s_pdepth, true)) {
                spdlog::error("[Flat3D-AFW] plugin depth CreateTexture failed");
                return;
            }
            s_pmv = pd::TextureDesc{};
            if (!renderer->CreateTexture((int)md.Width, (int)md.Height, md.Format,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    s_pmv, true)) {
                spdlog::error("[Flat3D-AFW] plugin MV CreateTexture failed");
                return;
            }
            spdlog::info("[Flat3D-AFW] plugin-owned depth/MV created ({}x{})", (uint32_t)dd.Width, dd.Height);
        }
    }

    // Same-stage GUI extraction: the PRE-overlay (hudless) and POST-overlay (with GUI) clones
    // bracket the GUI draw on the SAME target at the SAME pipeline stage, so the ExtractUI diff is
    // exact - the old backbuffer-vs-hudless diff spanned post-GUI passes and produced the alpha/
    // color artifacts + flicker. The clean UI is composited by OUR compose identically into both
    // eyes (the only flicker-free GUI treatment under AFR alternation).
    bool ui_ok = false;
    if (vr->m_multipass.pre_left_texture.Get() != nullptr && vr->m_multipass.pre_right_texture.Get() != nullptr) {
        back_reserved_texture(vr->m_multipass.pre_left_texture.Get());
        back_reserved_texture(vr->m_multipass.pre_right_texture.Get());
        fill_flat3d_pre(vr->m_multipass.pre_left_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, true);
        fill_flat3d_pre(vr->m_multipass.pre_right_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, false);
        ui_ok = m_flat3d_pre_left_cache.texture != nullptr && m_flat3d_pre_right_cache.texture != nullptr
            && afw.ensure_ui_textures(content_w, content_h, cache_desc.Format);
    }
    if (ui_ok && s_hudless_src.pTexture != m_flat3d_pre_left_cache.texture.Get()) {
        s_hudless_src = pd::TextureDesc{};
        s_hudless_src.pTexture = m_flat3d_pre_left_cache.texture.Get();
        s_hudless_src.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        renderer->SetupTextureDesc(s_hudless_src);
    }
    if (ui_ok && s_post_src.pTexture != m_flat3d_pre_right_cache.texture.Get()) {
        s_post_src = pd::TextureDesc{};
        s_post_src.pTexture = m_flat3d_pre_right_cache.texture.Get();
        s_post_src.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        renderer->SetupTextureDesc(s_post_src);
    }

    if (sl) spdlog::info("[Flat3D-AFW] warp step: BeginCommandList...");
    auto* cmd_list = renderer->BeginCommandList((int)(backbuffer_index % 3));

    // Fresh eye color -> plugin's current-eye buffer (the warp samples its own buffers).
    // Region copy: under the native-output override the cache holds content in its top-left
    // believed sub-region; the eye buffer is exactly that size.
    if (sl) spdlog::info("[Flat3D-AFW] warp step: plugin Copy color...");
    D3D12_BOX content_box{0, 0, 0, content_w, content_h, 1};
    renderer->Copy(cmd_list, eye_fb.color, s_color_desc[fresh], content_box);
    // Our depth/MV snapshots -> plugin-owned sampled textures.
    renderer->Copy(cmd_list, s_pdepth, s_depth_desc);
    renderer->Copy(cmd_list, s_pmv, s_mv_desc);
    // GUI handling. BAKED fallback (redirect off): displayed eyes keep the game's GUI; the plugin
    // re-composites the extracted UI unwarped into the warped eye.
    // Overlay-RT redirect active: the GUI never reaches the engine frames (naturally hudless
    // warp inputs) and lives in our own PER-EYE textures instead - no extraction, no plugin
    // recomposite. Each compose half samples its own eye's capture (world-anchored GUI elements
    // are projected through the alternating AFR camera, so per-eye sourcing gives markers their
    // anchor's true parallax and kills the alternation jitter); until both eyes have captured
    // once, the available one serves both halves.
    auto* gui_tex_l = Flat3DGuiRedirect::get().gui_texture(0);
    auto* gui_tex_r = Flat3DGuiRedirect::get().gui_texture(1);
    const bool gui_redirect = gui_tex_l != nullptr || gui_tex_r != nullptr; // always on once capturing
    if (gui_tex_l == nullptr) {
        gui_tex_l = gui_tex_r;
    }
    if (gui_tex_r == nullptr) {
        gui_tex_r = gui_tex_l;
    }

    if (ui_ok && !gui_redirect) {
        renderer->Copy(cmd_list, afw.io_hudless, s_hudless_src);
        renderer->Copy(cmd_list, afw.io_final, s_post_src);
        renderer->ExtractUI(cmd_list, afw.io_ui[fresh], afw.io_hudless, afw.io_final);
    }
    m_flat3d_afw_ui_tex = gui_redirect ? gui_tex_l : nullptr;       // compose t2 = left eye's GUI
    m_flat3d_afw_ui_depth_tex = gui_redirect ? gui_tex_r : nullptr; // compose t3 = right eye's GUI

    // MV-DECODE BURST ("AFW: debug view"): every ~600 frames, capture 8 CONSECUTIVE frames of
    // depth + raw MV + synthetic MV at 4 screen points (center, near85, left-quarter, top) and log
    // them as one block. Consecutive frames separate the per-frame eye alternation from object
    // motion (foliage!), and the 4 depths expose the MV field's depth dependence - the data needed
    // to decode Wilds' MV units/content. Aim at RIGID geometry while capturing.
    if (vr->m_flat3d_afw_debug->value() && afw.io_ready
            && afw.io_depth[fresh].pTexture != nullptr && afw.io_mv[fresh].pTexture != nullptr) {
        static uint32_t s_burst_left = 0;
        static uint32_t s_next_burst = 0;
        static int32_t s_meta_rfc[8]{};
        static uint32_t s_meta_eye[8]{};

        const auto rfc_now = (uint32_t)vr->m_render_frame_count;

        if (s_burst_left == 0 && rfc_now >= s_next_burst) {
            s_burst_left = 8;
            s_next_burst = rfc_now + 600;
        }

        if (s_burst_left > 0) {
            const uint32_t f = 8 - s_burst_left;
            --s_burst_left;

            if (m_afw_depth_readback != nullptr && m_afw_depth_readback->GetDesc().Width < 16384) {
                m_afw_depth_readback.Reset();
            }
            if (m_afw_depth_readback == nullptr) {
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = 16384; // 8 frames x 2048 (depth @+0, raw MV @+512, synth MV @+1024)
                bd.Height = 1;
                bd.DepthOrArraySize = 1;
                bd.MipLevels = 1;
                bd.SampleDesc = {1, 0};
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(m_afw_depth_readback.GetAddressOf()));
            }

            if (m_afw_depth_readback != nullptr) {
                constexpr auto k_io_state2 = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                const uint64_t base = 2048ull * f;
                const uint32_t w = afw.io_w;
                const uint32_t h = afw.io_h;
                // Sample points: center, near85 (player), left-quarter, top (sky/far).
                const uint32_t px[4]{w / 2, w / 2, w / 4, w / 2};
                const uint32_t py[4]{h / 2, h * 85 / 100, h / 2, h / 8};

                auto& c = m_generic_copiers[backbuffer_index % m_generic_copiers.size()];
                c.wait(INFINITE);
                {
                    std::scoped_lock _{c.mtx};

                    ID3D12Resource* srcs[3]{afw.io_depth[fresh].pTexture, afw.io_mv[fresh].pTexture,
                        afw.io_mv_corr[fresh].pTexture};
                    const DXGI_FORMAT fmts[3]{DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT};

                    for (int si = 0; si < 3; ++si) {
                        if (srcs[si] == nullptr) {
                            continue;
                        }

                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = srcs[si];
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        b.Transition.StateBefore = k_io_state2;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        c.cmd_list->ResourceBarrier(1, &b);

                        D3D12_TEXTURE_COPY_LOCATION src{};
                        src.pResource = srcs[si];
                        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        src.SubresourceIndex = 0;
                        D3D12_TEXTURE_COPY_LOCATION dst{};
                        dst.pResource = m_afw_depth_readback.Get();
                        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        dst.PlacedFootprint.Offset = base + 512ull * si;
                        dst.PlacedFootprint.Footprint.Format = fmts[si];
                        dst.PlacedFootprint.Footprint.Width = 4;
                        dst.PlacedFootprint.Footprint.Height = 1;
                        dst.PlacedFootprint.Footprint.Depth = 1;
                        dst.PlacedFootprint.Footprint.RowPitch = 256;

                        for (uint32_t pi = 0; pi < 4; ++pi) {
                            D3D12_BOX box{px[pi], py[pi], 0, px[pi] + 1, py[pi] + 1, 1};
                            c.cmd_list->CopyTextureRegion(&dst, pi, 0, 0, &src, &box);
                        }

                        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                        c.cmd_list->ResourceBarrier(1, &b);
                    }

                    c.has_commands = true;
                }
                c.execute();
                c.wait(INFINITE);

                s_meta_rfc[f] = (int32_t)rfc_now;
                s_meta_eye[f] = fresh;

                if (f == 7) {
                    auto h2f = [](uint16_t hbits) -> float {
                        uint32_t s = (uint32_t)(hbits & 0x8000u) << 16;
                        uint32_t e = (hbits >> 10) & 0x1fu;
                        uint32_t m = hbits & 0x3ffu;
                        uint32_t fb;
                        if (e == 0) {
                            if (m == 0) {
                                fb = s;
                            } else {
                                e = 113;
                                while ((m & 0x400u) == 0) { m <<= 1; --e; }
                                m &= 0x3ffu;
                                fb = s | (e << 23) | (m << 13);
                            }
                        } else if (e == 31) {
                            fb = s | 0x7f800000u | (m << 13);
                        } else {
                            fb = s | ((e + 112) << 23) | (m << 13);
                        }
                        float out;
                        memcpy(&out, &fb, 4);
                        return out;
                    };

                    D3D12_RANGE rr{0, 16384};
                    void* mapped = nullptr;
                    if (SUCCEEDED(m_afw_depth_readback->Map(0, &rr, &mapped)) && mapped != nullptr) {
                        spdlog::info("[Flat3D-AFW] MV burst: pts=(c,n85,q,t) dims={}x{} mv_scale_raw=({:.1f},{:.1f}) sep={:.4f} conv={:.3f} p00={:.4f}",
                            w, h, afw.ngx_mv_scale_raw[0], afw.ngx_mv_scale_raw[1],
                            vr->m_flat3d->separation_eff, vr->m_flat3d->convergence, vr->m_flat3d_game_p00.load());

                        for (uint32_t bf = 0; bf < 8; ++bf) {
                            const auto* bytes = (const uint8_t*)mapped + 2048ull * bf;
                            float d[4]{};
                            memcpy(d, bytes, 16);
                            uint16_t mraw[8]{};
                            uint16_t msyn[8]{};
                            memcpy(mraw, bytes + 512, 16);
                            memcpy(msyn, bytes + 1024, 16);

                            spdlog::info("[Flat3D-AFW] MVB f={} rfc={} eye={} d=({:.3e},{:.3e},{:.3e},{:.3e}) "
                                "raw=({:.4f},{:.4f})({:.4f},{:.4f})({:.4f},{:.4f})({:.4f},{:.4f}) "
                                "syn=({:.4f},{:.4f})({:.4f},{:.4f})({:.4f},{:.4f})({:.4f},{:.4f})",
                                bf, s_meta_rfc[bf], s_meta_eye[bf], d[0], d[1], d[2], d[3],
                                h2f(mraw[0]), h2f(mraw[1]), h2f(mraw[2]), h2f(mraw[3]),
                                h2f(mraw[4]), h2f(mraw[5]), h2f(mraw[6]), h2f(mraw[7]),
                                h2f(msyn[0]), h2f(msyn[1]), h2f(msyn[2]), h2f(msyn[3]),
                                h2f(msyn[4]), h2f(msyn[5]), h2f(msyn[6]), h2f(msyn[7]));
                        }

                        D3D12_RANGE wr{0, 0};
                        m_afw_depth_readback->Unmap(0, &wr);
                    }
                }
            }
        }
    }

    // Camera matrices, all same-tick (recorded by the view/proj hooks this frame). glm
    // column-major matches the plugin convention (the transpose experiment is settled: off).
    auto fix = [&](const Matrix4x4f& m) { return m; };

    // Frame-aligned matrices: the game thread may have recorded the NEXT frame's already; the ring
    // returns this frame's own (falls back to live on a miss).
    const auto& afr_frame = vr->get_afw_frame_for((int32_t)vr->m_render_frame_count);

    const auto& view_src = afr_frame.view[fresh];
    const auto& proj_src = afr_frame.proj[fresh];

    const Matrix4x4f& view_dst = afr_frame.view[other];
    const Matrix4x4f& proj_dst = afr_frame.proj[other];

    // to_reverseZ: the recorded projections are forward-Z but the depth buffer is reversed-Z -
    // the plugin needs clip matrices matching the depth it samples (same as UEVR-3D).
    const auto proj_src_rz = afw_to_reverse_z(proj_src);
    const auto proj_dst_rz = afw_to_reverse_z(proj_dst);

    pd::CameraData cd{};
    cd.srcWorldToViewMatrix = fix(view_src);
    cd.srcViewToWorldMatrix = fix(glm::inverse(view_src));
    cd.srcViewToClipMatrix = fix(proj_src_rz);
    cd.srcClipToViewMatrix = fix(glm::inverse(proj_src_rz));
    cd.destWorldToViewMatrix = fix(view_dst);
    cd.destViewToWorldMatrix = fix(glm::inverse(view_dst));
    cd.destViewToClipMatrix = fix(proj_dst_rz);
    cd.destClipToViewMatrix = fix(glm::inverse(proj_dst_rz));

    // Prefer the NGX (DLSS) harvested inputs - real full-res depth/MV in DLSS's own state, copied on
    // the game's command list. The engine-reflection snapshot path stays as fallback (its
    // "DepthStencilTex" reads zero on Wilds - likely a decoy).
    const bool ngx_fresh = afw.io_ready
        && (vr->m_render_frame_count - afw.ngx_last_frame) < 10
        && afw.io_depth[fresh].pTexture != nullptr;

    // Synthetic eye-jump MV field (pure same-tick parallax, no camera/object motion pollution):
    // computed HERE, on this present-time command list - recording compute on the game's DLSS
    // list trips a deterministic null-deref in the NV driver's deferred binding resolve.
    extern bool flat3d_ngx_mv_correct_dispatch(ID3D12GraphicsCommandList* cmd, uint32_t eye);
    const bool mv_corrected = ngx_fresh && flat3d_ngx_mv_correct_dispatch(cmd_list, fresh);

    // Foreground depth dilation (silhouette halo fix) - see Flat3DDepthDilatePass. NGX path only
    // (the active source on Wilds); radius 0 = off.
    static Flat3DDepthDilatePass s_depth_dilate{};
    const int dilate_r = (int)std::lround(vr->m_flat3d_afw_depth_dilation->value());
    bool depth_dilated = false;

    if (ngx_fresh && dilate_r > 0 && afw.io_depth_dil.pTexture != nullptr && s_depth_dilate.ensure(device)) {
        s_depth_dilate.dispatch(cmd_list, renderer, afw.io_depth[fresh], afw.io_depth_dil, afw.io_w, afw.io_h, dilate_r);
        depth_dilated = true;
    }

    pd::FrameBufferDesc in{};
    in.color = eye_fb.color;
    in.depth = depth_dilated ? afw.io_depth_dil : (ngx_fresh ? afw.io_depth[fresh] : s_pdepth);
    in.motionVectors = mv_corrected ? afw.io_mv_corr[fresh] : (ngx_fresh ? afw.io_mv[fresh] : s_pmv);

    static bool s_src_logged = false;
    if (!s_src_logged) {
        s_src_logged = true;
        spdlog::info("[Flat3D-AFW] warp inputs: {}", ngx_fresh ? "NGX (DLSS) depth/MV" : "engine-reflection snapshot (fallback)");
    }

    pd::FrameWarpEvaluateParams p{};
    p.InCmdList = cmd_list;
    p.InEyeFrameBuffer = &in;
    // Baked fallback: plugin re-composites the extracted UI unwarped into the warped eye.
    // GUI-redirect mode: the frames are naturally hudless - warp everything, compose adds GUI.
    p.InUIColorAlpha = (ui_ok && !gui_redirect) ? &afw.io_ui[fresh] : nullptr;
    p.IsHudlessColor = !ui_ok || gui_redirect;
    // Synthetic field = displacement from fresh-eye pixels to the other eye (same tick); raw MVs
    // reference the other eye's previous frame. Both match FromOtherEye semantics.
    // Settled OBJECT-ONLY: the field carries per-object motion only and the plugin reprojects the
    // same-tick eye parallax itself from CameraData + depth (the decomposition PureDark's own RE9
    // build uses). User-validated as better than the field-driven eye-jump.
    p.MotionVectorsType = pd::ObjectOnly;
    // NGX gives the exact MV scale (output-pixel space); the user sliders act as multipliers on top.
    // Exact NGX scale - the burst-validated synthetic field needs no user multiplier (the old
    // scale/threshold sliders were raw-MV-era tuning; threshold acts on per-object motion, which
    // the camera-only field has none of).
    p.InMotionScale[0] = ngx_fresh ? afw.ngx_mv_scale[0] : 1.0f;
    p.InMotionScale[1] = ngx_fresh ? afw.ngx_mv_scale[1] : 1.0f;
    // Warp-mode A/B (default Combined - with the camera-only field it strongly reduces foliage
    // flicker; the other modes isolate which layer causes the foreground disocclusion halo).
    switch (vr->m_flat3d_afw_mode->value()) {
    case 1: p.Mode = pd::AlternateEyeWarping; break;
    case 2: p.Mode = pd::PreviousFrameWarping; break;
    default: p.Mode = pd::CombinedWarping; break;
    }
    p.EyeIndex = (pd::EyeIndex)fresh;
    p.CameraData = &cd;
    p.IgnoreMotionThreshold = vr->m_flat3d_afw_motion_thresh->value(); // foliage gate for the object-motion term
    p.Debug = vr->m_flat3d_afw_plugin_debug->value(); // separate from the LOGGING toggle - this changes what the plugin draws

    if (sl) spdlog::info("[Flat3D-AFW] warp step: EvaluateFrameWarp...");
    afw.evaluate(p);

    if (sl) spdlog::info("[Flat3D-AFW] warp step: EndCommandList...");
    renderer->EndCommandList((int)(backbuffer_index % 3));

    // Roll the both-eye matrix history (prev2 <- prev <- this frame); the DLSS same-eye feed
    // needs N-2.
    if (afr_frame.valid) {
        for (int i = 0; i < 2; ++i) {
            afw.prev2_view[i] = afw.prev_view[i];
            afw.prev2_proj[i] = afw.prev_proj[i];
            afw.prev_view[i] = afr_frame.view[i];
            afw.prev_proj[i] = afr_frame.proj[i];
        }
        if (afw.prev_frames < 2) {
            ++afw.prev_frames;
        }
    }

    // Warped result -> the stale eye's cache, so the compose shows a same-tick coherent pair.
    // Plugin leaves its output in ALL shader-resource state. (Baked-GUI mode: the fresh cache
    // keeps the game's own GUI.)
    auto& copier = m_generic_copiers[backbuffer_index % m_generic_copiers.size()];
    copier.wait(INFINITE);
    // (Fresh cache keeps its pristine baked world-GUI - only the warped half is composited.)
    if (content_w != (uint32_t)cache_desc.Width || content_h != cache_desc.Height) {
        // Believed-size warp output -> the cache's top-left content region.
        D3D12_BOX out_box{0, 0, 0, content_w, content_h, 1};
        copier.copy_region(other_fb.color.pTexture, other_cache.texture.Get(), &out_box,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else {
        copier.copy(other_fb.color.pTexture, other_cache.texture.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    copier.execute();

    static uint32_t s_wlog = 0;
    ++s_wlog;
    if (vr->m_flat3d_afw_debug->value() && (s_wlog % 240) == 1) {
        // Full state dump: parity, per-eye view translations (are the two views actually +/-sep/2
        // apart?), per-eye shear terms, and the runtime eye transforms themselves.
        const auto& e0 = vr->get_runtime()->eyes[0];
        const auto& e1 = vr->get_runtime()->eyes[1];
        spdlog::info("[Flat3D-AFW] state: rfc={} fc={} fresh={} | view0.t=({:.4f},{:.4f},{:.4f}) view1.t=({:.4f},{:.4f},{:.4f}) | "
            "shear0={:.5f} shear1={:.5f} | eyes0.x={:.4f} eyes1.x={:.4f} | sep_eff={:.4f} conv={:.3f}",
            vr->m_render_frame_count, vr->m_frame_count, fresh,
            view_src[3].x, view_src[3].y, view_src[3].z,
            view_dst[3].x, view_dst[3].y, view_dst[3].z,
            proj_src[2][0], proj_dst[2][0],
            e0[3].x, e1[3].x,
            vr->m_flat3d->separation_eff, vr->m_flat3d->convergence);
        // Projection convention check vs the plugin's expectation (UEVR feeds to_reverseZ'd UE
        // projections: reversed-Z, RH -Z-forward => P22=0-ish, P23=near-ish, P32=-1).
        spdlog::info("[Flat3D-AFW] proj conv: P00={:.4f} P11={:.4f} P22={:.6f} P23={:.6f} P32={:.4f} P33={:.4f} | ring_d={}",
            proj_src[0][0], proj_src[1][1], proj_src[2][2], proj_src[3][2], proj_src[2][3], proj_src[3][3],
            afr_frame.rfc - (int32_t)vr->m_render_frame_count);
    }
}

void D3D12Component::on_reset(VR* vr) {
    REF_PROFILE_FUNCTION();

    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& copier : m_generic_copiers) {
        copier.reset();
    }

    for (auto& commands : m_backbuffer_copy_commands) {
        commands.reset();
    }
    
    m_prev_backbuffer.Reset();
    m_backbuffer_copy.reset();
    m_converted_eye_tex.reset();
    m_flat3d_compose.reset();
    Flat3DGuiRedirect::get().on_reset();

    for (auto& ctx : m_flat3d_src) {
        ctx.reset();
    }
    m_flat3d_pre_src.reset();
    m_flat3d_pre_left_cache.reset();
    m_flat3d_pre_right_src.reset();
    m_flat3d_pre_right_cache.reset();
    m_afw_depth_local.Reset();
    m_afw_mv_local.Reset();
    m_flat3d_eye_filled = {};

    if (runtime->is_openxr() && runtime->loaded) {
        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height()) {
            m_openxr.create_swapchains();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_openvr.texture_counter = 0;
}

void D3D12Component::setup() {
    REF_PROFILE_FUNCTION();

    if (VR::get()->is_hmd_active()) {
        spdlog::info("[VR] Setting up d3d12 textures...");
    }
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    ComPtr<ID3D12Resource> real_backbuffer{};

    const auto& vr = VR::get();
    const auto is_multipass = vr->is_using_multipass();
    
    if (is_multipass && vr->m_multipass.eye_textures[0].Get() != nullptr && vr->m_multipass.eye_textures[1].Get() != nullptr) {
        backbuffer = vr->m_multipass.eye_textures[0];
    } else if (is_multipass) {
        spdlog::warn("[VR] Multipass textures are not setup correctly.");
    }

    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return;
    }

    if (backbuffer == nullptr) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return;
    }

    auto backbuffer_desc = backbuffer->GetDesc();
    const auto real_backbuffer_desc = real_backbuffer->GetDesc();

    if (is_multipass) {
        backbuffer_desc.Width = vr->get_hmd_width();
        backbuffer_desc.Height = vr->get_hmd_height();

        if (backbuffer.Get() == real_backbuffer.Get()) {
            // Flatscreen 3D: keep the REAL backbuffer format so the 8-bit check
            // below stays honest. On a 10-bit swapchain the caches must be fed
            // through the conversion path - a raw CopyResource from a 10-bit
            // backbuffer into 8-bit caches is invalid and removes the device.
            if (!vr->get_runtime()->is_flat3d()) {
                backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            }
            spdlog::warn("[VR] Multipass textures are not setup correctly: Re-using backbuffer.");
        } else {
            spdlog::info("[VR] Multipass textures are setup correctly.");
        }
    } else {
        backbuffer_desc.Width = real_backbuffer_desc.Width;
        backbuffer_desc.Height = real_backbuffer_desc.Height;
    }

    m_openvr.last_format = backbuffer_desc.Format;

    spdlog::info("[VR] D3D12 Backbuffer width: {}, height: {}, format: {}", backbuffer_desc.Width, backbuffer_desc.Height, backbuffer_desc.Format);
    spdlog::info("[VR] D3D12 Real Backbuffer width: {}, height: {}, format: {}", real_backbuffer_desc.Width, real_backbuffer_desc.Height, real_backbuffer_desc.Format);

    m_backbuffer_is_8bit = backbuffer_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || backbuffer_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;

    auto backbuffer_srv_desc = backbuffer_desc;
    backbuffer_srv_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_srv_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    // Create copy of backbuffer to use as SRV to convert from HDR to 8bit
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> backbuffer_copy{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_srv_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                IID_PPV_ARGS(backbuffer_copy.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up backbuffer copy texture RTV/SRV.");
        }
    }

    // Create copy of backbuffer to use as SRV to convert from HDR to 8bit
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> backbuffer_copy{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_srv_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                IID_PPV_ARGS(backbuffer_copy.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up backbuffer copy texture RTV/SRV.");
        }
    }

    auto rt_desc = backbuffer_desc;

    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    switch (backbuffer_desc.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;

        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        
        default:
            spdlog::error("[OpenVR] Possibly unsupported backbuffer format: {}", backbuffer_desc.Format);
            break;
    };

    // Create converted eye texture
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create converted eye texture.");
            return;
        }

        if (!m_converted_eye_tex.setup(device, eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up converted eye texture RTV/SRV.");
        }
    }

    for (auto& ctx : m_openvr.left_eye_tex) {
        ComPtr<ID3D12Resource> left_eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(left_eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create left eye texture.");
            return;
        }

        left_eye_tex->SetName(L"OpenVR Left Eye Texture");
        if (!ctx.setup(device, left_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up left eye texture RTV/SRV.");
        }
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ComPtr<ID3D12Resource> right_eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(right_eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create right eye texture.");
            return;
        }

        right_eye_tex->SetName(L"OpenVR Right Eye Texture");
        if (!ctx.setup(device, right_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up right eye texture RTV/SRV.");
        }
    }

    // Flat3D GUI-match: 8-bit caches for the pre-overlay (scene-only) eyes (left = diff source + depth
    // base; right = depth base for the shifted GUI).
    {
        ComPtr<ID3D12Resource> pre_left_tex{};
        if (SUCCEEDED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(pre_left_tex.GetAddressOf())))) {
            pre_left_tex->SetName(L"Flat3D Pre-Overlay Left Cache");
            if (!m_flat3d_pre_left_cache.setup(device, pre_left_tex.Get(), std::nullopt, std::nullopt)) {
                spdlog::error("[VR] Error setting up Flat3D pre-overlay left cache RTV/SRV.");
            }
        } else {
            spdlog::error("[VR] Failed to create Flat3D pre-overlay left cache.");
        }

        ComPtr<ID3D12Resource> pre_right_tex{};
        if (SUCCEEDED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(pre_right_tex.GetAddressOf())))) {
            pre_right_tex->SetName(L"Flat3D Pre-Overlay Right Cache");
            if (!m_flat3d_pre_right_cache.setup(device, pre_right_tex.Get(), std::nullopt, std::nullopt)) {
                spdlog::error("[VR] Error setting up Flat3D pre-overlay right cache RTV/SRV.");
            }
        } else {
            spdlog::error("[VR] Failed to create Flat3D pre-overlay right cache.");
        }
    }

    for (auto& copier : m_generic_copiers) {
        copier.setup(L"Generic Copier");
    }
    
    for (auto& commands : m_backbuffer_copy_commands) {
        commands.setup(L"Backbuffer Copy Commands");
    }

    m_flat3d_ui_barrier_cmd.setup(L"Flat3D UI Barrier");

    setup_sprite_batch_pso(rt_desc.Format);

    m_backbuffer_size[0] = real_backbuffer_desc.Width;
    m_backbuffer_size[1] = real_backbuffer_desc.Height;

    if (vr->get_runtime()->is_flat3d()) {
        m_flat3d_compose.setup(device, real_backbuffer_desc.Format);
        Flat3DGuiRedirect::get().init(device); // installs the global D3D12 detours once
    }

    spdlog::info("[VR] d3d12 textures have been setup");
    m_force_reset = false;
}

void D3D12Component::setup_sprite_batch_pso(DXGI_FORMAT output_format) {
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    DirectX::RenderTargetState output_state{output_format, DXGI_FORMAT_UNKNOWN};
    DirectX::SpriteBatchPipelineStateDescription pd{output_state};

    // OPAQUE blit (override the SpriteBatch default of premultiplied AlphaBlend). We use this batch
    // to COPY whole harvested images into the eye caches. The harvested output target has A=0 in
    // HUD/menu-transparent regions; an alpha blend skips those pixels and leaves the un-cleared
    // cache's stale content showing through - so a black menu background renders as leftover white
    // and old frames ghost. Overwriting the RGB regardless of source alpha copies the engine's
    // already-composited scene/HUD verbatim.
    D3D12_BLEND_DESC opaque_blend{};
    opaque_blend.AlphaToCoverageEnable = FALSE;
    opaque_blend.IndependentBlendEnable = FALSE;
    opaque_blend.RenderTarget[0].BlendEnable = FALSE;
    opaque_blend.RenderTarget[0].LogicOpEnable = FALSE;
    opaque_blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.blendDesc = opaque_blend;

    m_sprite_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");
}

void D3D12Component::render_srv_to_rtv(ID3D12GraphicsCommandList* command_list, const d3d12::TextureContext& src, const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    if (m_sprite_batch == nullptr) {
        return;
    }

    d3d12::render_srv_to_rtv(m_sprite_batch.get(), command_list, src, dst, src_state, dst_state);
}

Flat3DCompose::RepackParams D3D12Component::build_flat3d_params(VR* vr, uint32_t out_width, uint32_t out_height) {
    Flat3DCompose::RepackParams params{};
    params.out_size[0] = (int32_t)out_width;
    params.out_size[1] = (int32_t)out_height;
    params.eye_swap = vr->m_flat3d_eye_swap->value() ? 1 : 0;

    switch ((VR::Flat3DOutputMode)vr->m_flat3d_output_mode->value()) {
    case VR::FLAT3D_SBS: params.mode = Flat3DCompose::MODE_SBS; break;
    case VR::FLAT3D_TAB: params.mode = Flat3DCompose::MODE_TAB; break;
    case VR::FLAT3D_ROW_INTERLACED: params.mode = Flat3DCompose::MODE_ROW_INTERLACED; break;
    case VR::FLAT3D_COLUMN_INTERLACED: params.mode = Flat3DCompose::MODE_COL_INTERLACED; break;
    case VR::FLAT3D_CHECKERBOARD: params.mode = Flat3DCompose::MODE_CHECKERBOARD; break;
    case VR::FLAT3D_LEIA_SR: params.mode = Flat3DCompose::MODE_LEIA_SR; break; // SbS compose; weaver consumes it
    case VR::FLAT3D_ANAGLYPH_RC: params.mode = Flat3DCompose::MODE_ANA_RC; break;
    case VR::FLAT3D_ANAGLYPH_RC_DUBOIS: params.mode = Flat3DCompose::MODE_ANA_RC_DUBOIS; break;
    case VR::FLAT3D_ANAGLYPH_RC_HALFCOLOR: params.mode = Flat3DCompose::MODE_ANA_RC_COMPROMISE; break;
    case VR::FLAT3D_ANAGLYPH_GM: params.mode = Flat3DCompose::MODE_ANA_GM; break;
    case VR::FLAT3D_ANAGLYPH_GM_DUBOIS: params.mode = Flat3DCompose::MODE_ANA_GM_DUBOIS; break;
    case VR::FLAT3D_ANAGLYPH_BLUE_AMBER: params.mode = Flat3DCompose::MODE_ANA_BLUE_AMBER; break;
    case VR::FLAT3D_DEBUG_LEFT_ONLY: params.mode = Flat3DCompose::MODE_DEBUG_LEFT; break;
    case VR::FLAT3D_DEBUG_RIGHT_ONLY: params.mode = Flat3DCompose::MODE_DEBUG_RIGHT; break;
    default: params.mode = Flat3DCompose::MODE_SBS; break;
    }

    const auto& left = m_openvr.get_left();

    if (left.texture != nullptr) {
        const auto desc = left.texture->GetDesc();
        params.eye_size[0] = (int32_t)desc.Width;
        params.eye_size[1] = (int32_t)desc.Height;

        // Optional centered 16:9 crop per eye for SbS/TaB on sources wider than 16:9
        if (vr->m_flat3d_crop_eyes_169->value() &&
            (params.mode == Flat3DCompose::MODE_SBS || params.mode == Flat3DCompose::MODE_LEIA_SR || params.mode == Flat3DCompose::MODE_TAB))
        {
            const auto aspect = (float)desc.Width / (float)desc.Height;
            constexpr float target_aspect = 16.0f / 9.0f;

            if (aspect > target_aspect + 1e-4f) {
                params.crop_frac_u = target_aspect / aspect;
                params.crop_origin_u = (1.0f - params.crop_frac_u) * 0.5f;
            }
        }
    }

    // Native-output override: map the engine's believed sub-region to the full output (this is
    // where the render-res -> display-native upscale actually happens; the pattern picking stays
    // in output pixels = display-exact).
    if (auto& hookp = g_framework->get_d3d12_hook(); hookp != nullptr) {
        const auto bw = hookp->get_engine_believed_width();
        const auto bh = hookp->get_engine_believed_height();

        if (bw != 0 && bh != 0 && bw <= out_width && bh <= out_height && (bw != out_width || bh != out_height)) {
            params.content_frac_u = (float)bw / (float)out_width;
            params.content_frac_v = (float)bh / (float)out_height;
        }
    }

    // AFW renders PARALLEL (shear-free) projections - convergence applies HERE instead, as the
    // symmetric-projection compose shift (a uniform per-eye image shift is exactly what the shear
    // did to the image), with cover-zoom hiding the revealed edges. Keeps the warp
    // translation-only (the plugin's model) and puts the skybox at proper infinity depth in BOTH
    // eyes. Sign follows the shear's validated convention (shear_dir_left).
    if (vr->is_using_flat3d_afw()) {
        const auto p00 = vr->m_flat3d_game_p00.load();
        const auto sep = vr->m_flat3d->separation_eff;
        const auto conv = std::max(vr->m_flat3d->convergence, 0.01f);
        const float s = (sep * 0.5f / conv) * p00 * 0.5f; // shear NDC offset -> UV units
        params.scene_shift_uv = (vr->m_flat3d->shear_dir_left > 0.0f) ? -s : s;
        // Cover-zoom hides the shift-revealed edge strips. Only the SCENE zooms - the extracted
        // GUI is composited at raw coords (native size); black-bar fallback stays for any
        // remaining out-of-range samples.
        params.scene_scale = 1.0f + 2.0f * std::abs(params.scene_shift_uv);

        // Overlay-RT redirect: composite the captured GUI texture into BOTH halves at the GUI
        // plane's disparity. Same formula family as the scene shift: content at depth d nets
        // sep*p00*0.25*(1/d - 1/conv) per eye after the compose shift, which equals
        // scene_shift_uv * (1 - conv/d) - at d=inf the GUI moves exactly like the shifted-to-
        // infinity scene, at d=conv it sits on the screen plane.
        if (m_flat3d_afw_ui_tex != nullptr) {
            // Clamp to the slider range (a persisted value from the old 15 m range may exceed it).
            const float d = std::clamp(vr->m_ui_distance_option->value(), 0.05f, 8.0f);
            params.ui_enabled = vr->m_flat3d_afw_debug->value() ? 2 : 1; // 2 = alpha-channel debug view
            params.ui_shift_uv = params.scene_shift_uv * (1.0f - conv / d);
            // Horizontal fit-squish about center so the plane shift can't clip the GUI's sides
            // (dynamic3d "fit": shrink the overlay, never crop it). Floor guards degenerate math.
            params.ui_fit_scale = std::max(1.0f - 2.0f * std::abs(params.ui_shift_uv), 0.7f);
        }
    }

    // Depth-aware dynamic crosshair (own-reticle): per-eye shift computed from
    // the sampled center depth via the shift_px formula in eye-UV units:
    // shift_uv = (sep * P00 * 0.25) * (1/z - 1/conv).
    if (vr->m_flat3d_dynamic_crosshair->value() && params.eye_size[1] > 0) {
        params.crosshair_enabled = 1;

        auto z = vr->m_flat3d_depth_sampler.get_center_depth();

        if (z <= 0.0f) {
            z = vr->m_flat3d_crosshair_depth->value(); // static fallback
        }

        const auto p00 = vr->m_flat3d_game_p00.load();
        const auto sep = vr->m_flat3d->separation_eff;
        const auto conv = std::max(vr->m_flat3d->convergence, 0.01f);

        auto shift_uv = (sep * p00 * 0.25f) * (1.0f / std::max(z, 0.01f) - 1.0f / conv);

        // The compositor shift sign is INDEPENDENT of the projection shear sign
        // (dynamic3d 1.3) - each gets its own empirical flip toggle.
        if (vr->m_flat3d_swap_shift_sign->value()) {
            shift_uv = -shift_uv;
        }

        params.crosshair_shift_uv = shift_uv;
        params.crosshair_len_px = std::max(8.0f, (float)params.eye_size[1] * 0.012f);
        params.crosshair_thick_px = std::max(1.5f, (float)params.eye_size[1] * 0.0018f);
    }

    // (GUI-match compose removed with flat3d multipass; HUD depth will return via per-eye UI
    // compositing.)

    return params;
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    REF_PROFILE_FUNCTION();

    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    const auto& vr = VR::get();
    const auto is_multipass = vr->is_using_multipass();

    // Get the existing backbuffer
    // so we can get the format and stuff.
    bool has_multipass_buffer = false;
    if (is_multipass && vr->m_multipass.eye_textures[0].Get() != nullptr) {
        backbuffer = vr->m_multipass.eye_textures[0];
        has_multipass_buffer = true;
    }

    if (backbuffer.Get() == nullptr && FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& openxr = vr->m_openxr;

    this->contexts.clear();
    this->contexts.resize(openxr->views.size());

    this->last_format = backbuffer_desc.Format;

    DXGI_FORMAT swapchain_format{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};

    switch (backbuffer_desc.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            break;

        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            swapchain_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            break;
        
        default:
            spdlog::error("[VR] Possibly unsupported backbuffer format: {}", backbuffer_desc.Format);
            break;
    };
    
    for (auto i = 0; i < openxr->views.size(); ++i) {
        spdlog::info("[VR] Creating swapchain for eye {}", i);
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        backbuffer_desc.Width = vr->get_hmd_width();
        backbuffer_desc.Height = vr->get_hmd_height();

        if (swapchain_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        } else {
            backbuffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        }

        // Create the swapchain.
        XrSwapchainCreateInfo swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchain_create_info.arraySize = 1;
        swapchain_create_info.format = swapchain_format;
        swapchain_create_info.width = backbuffer_desc.Width;
        swapchain_create_info.height = backbuffer_desc.Height;
        swapchain_create_info.mipCount = 1;
        swapchain_create_info.faceCount = 1;
        swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
        swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains.push_back(swapchain);

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        spdlog::info("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }

        for (uint32_t j = 0; j < image_count; ++j) {
            uint32_t real_index{};
            XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

            result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &real_index);
            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to acquire swapchain image.");
                return "Failed to acquire swapchain image.";
            }

            XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to wait for swapchain image.");
                return "Failed to wait for swapchain image.";
            }

            ctx.texture_contexts[real_index] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[real_index]->setup(device, ctx.textures[real_index].texture, swapchain_format, swapchain_format, (std::wstring{L"OpenXR Swapchain "} + std::to_wstring(i) + L" " + std::to_wstring(real_index)).c_str());

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to release swapchain image.");
                return "Failed to release swapchain image.";
            }

            //ctx.texture_contexts[j]->texture = ctx.textures[j].texture;
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

	if (this->contexts.empty()) {
        return;
    }

    spdlog::info("[VR] Destroying swapchains.");

    this->wait_for_all_copies();

    for (auto i = 0; i < this->contexts.size(); ++i) {
        auto& ctx = this->contexts[i];
        ctx.texture_contexts.clear();

        auto result = xrDestroySwapchain(VR::get()->m_openxr->swapchains[i].handle);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to destroy swapchain {}.", i);
        } else {
            spdlog::info("[VR] Destroyed swapchain {}.", i);
        }

        ctx.textures.clear();
    }

    this->contexts.clear();
    VR::get()->m_openxr->swapchains.clear();
}

void D3D12Component::OpenXR::copy(
    uint32_t swapchain_idx, 
    ID3D12Resource* resource, 
    D3D12_BOX* src_box, 
    D3D12_RESOURCE_STATES src_state,
    OpenXR::CopyFn copy_fn)
{
    REF_PROFILE_FUNCTION();

    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->m_openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);

            if (copy_fn == nullptr) {
                if (src_box != nullptr) {
                    texture_ctx->commands.copy_region(
                        resource, 
                        ctx.textures[texture_index].texture, 
                        src_box, src_state, 
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                } else {
                    texture_ctx->commands.copy(
                        resource, 
                        ctx.textures[texture_index].texture, 
                        src_state, 
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            } else {
                copy_fn(texture_ctx->commands, *texture_ctx, src_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            texture_ctx->commands.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
        }
    }
}
} // namespace vrmod
