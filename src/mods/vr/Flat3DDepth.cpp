#include <algorithm>
#include <vector>

#include <spdlog/spdlog.h>

#include "REFramework.hpp"

#include "Flat3DDepth.hpp"

namespace vrmod {
// The engine's CopyTexture command manages the game depth buffer's states for
// us; only the CLONE (touched exclusively by that copy and our readback) needs
// an assumed resting state. If depth reads come back as garbage on some title,
// this is the first thing to re-check.
static constexpr D3D12_RESOURCE_STATES CLONE_ASSUMED_STATE = D3D12_RESOURCE_STATE_COPY_DEST;

void Flat3DDepth::update_engine_copy(sdk::renderer::RenderContext* context, sdk::renderer::layer::Scene* scene_layer) {
    if (context == nullptr || scene_layer == nullptr) {
        return;
    }

    auto depth = scene_layer->get_depth_stencil();

    if (depth == nullptr) {
        return;
    }

    const auto depth_d3d12 = scene_layer->get_depth_stencil_d3d12();

    if (depth_d3d12 == nullptr) {
        return;
    }

    {
        std::scoped_lock _{m_mtx};

        if (m_depth_clone == nullptr || m_last_depth != depth_d3d12) {
            m_depth_clone = depth->clone();
            m_last_depth = depth_d3d12;

            spdlog::info("[Flat3D] Cloned depth stencil for readback @ {:x}", (uintptr_t)m_depth_clone.get());
        }
    }

    if (m_depth_clone != nullptr) {
        context->copy_texture(m_depth_clone, depth);
    }
}

void Flat3DDepth::create_readback(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc) {
    m_readback.Reset();
    m_copy_in_flight = false;

    // Figure out the copyable footprint of the depth plane (plane/subresource 0).
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, nullptr);

    m_copy_format = fp.Footprint.Format;
    m_width = desc.Width;
    m_height = desc.Height;
    m_row_pitch = (uint32_t)((desc.Width * 4 + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)) & ~(uint64_t)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));

    switch (m_copy_format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        break; // 4 bytes per texel, supported
    default:
        spdlog::error("[Flat3D] Unsupported depth copy format: {}", (int)m_copy_format);
        m_copy_format = DXGI_FORMAT_UNKNOWN;
        return;
    }

    // Sparse rows across the middle ~90% of the frame.
    for (uint32_t i = 0; i < NUM_ROWS; ++i) {
        const auto t = 0.05f + 0.9f * ((float)i + 0.5f) / (float)NUM_ROWS;
        m_row_ys[i] = std::min((uint32_t)(t * (float)m_height), m_height - 1);
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = (uint64_t)m_row_pitch * NUM_ROWS;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc = {1, 0};
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_readback)))) {
        spdlog::error("[Flat3D] Failed to create depth readback buffer");
        return;
    }

    if (!m_commands_setup) {
        m_commands_setup = m_commands.setup(L"Flat3D Depth Readback");
    }

    spdlog::info("[Flat3D] Depth readback ready: {}x{}, copy format {}", m_width, m_height, (int)m_copy_format);
}

void Flat3DDepth::on_frame(float nearz) {
    std::scoped_lock _{m_mtx};

    // Source select: external (DLSS io depth - real on Wilds) wins over the engine clone.
    constexpr auto k_external_state = (D3D12_RESOURCE_STATES)(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12Resource* resource = m_external;
    auto resting_state = k_external_state;

    if (resource == nullptr) {
        if (m_depth_clone == nullptr) {
            return;
        }

        const auto container = m_depth_clone->get_d3d12_resource_container();

        if (container == nullptr) {
            return;
        }

        resource = container->get_native_resource();

        if (resource == nullptr) {
            return;
        }

        resting_state = CLONE_ASSUMED_STATE;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();

    if (device == nullptr) {
        return;
    }

    const auto desc = resource->GetDesc();

    if (m_readback == nullptr || desc.Width != m_width || desc.Height != m_height) {
        create_readback(device, desc);
    }

    if (m_readback == nullptr || m_copy_format == DXGI_FORMAT_UNKNOWN || !m_commands_setup) {
        return;
    }

    // Poll (never block) the in-flight copy; reduce it once complete.
    if (m_copy_in_flight) {
        if (m_commands.fence->GetCompletedValue() < m_commands.fence_value) {
            return; // still in flight, try again next present
        }

        process(nearz);
        m_copy_in_flight = false;
    }

    // Kick the next copy: N sparse rows from the depth clone into the buffer.
    m_commands.wait(INFINITE); // fence already complete; resets allocator/list

    auto cmd_list = m_commands.cmd_list.Get();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = resting_state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd_list->ResourceBarrier(1, &barrier);

    for (uint32_t i = 0; i < NUM_ROWS; ++i) {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = (uint64_t)m_row_pitch * i;
        dst.PlacedFootprint.Footprint.Format = m_copy_format;
        dst.PlacedFootprint.Footprint.Width = (uint32_t)m_width;
        dst.PlacedFootprint.Footprint.Height = 1;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = m_row_pitch;

        D3D12_BOX box{};
        box.left = 0;
        box.right = (uint32_t)m_width;
        box.top = m_row_ys[i];
        box.bottom = m_row_ys[i] + 1;
        box.front = 0;
        box.back = 1;

        cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = resting_state;
    cmd_list->ResourceBarrier(1, &barrier);

    m_commands.has_commands = true;
    m_commands.execute();
    m_copy_in_flight = true;
}

void Flat3DDepth::process(float nearz) {
    uint8_t* mapped = nullptr;
    const D3D12_RANGE read_range{0, (size_t)m_row_pitch * NUM_ROWS};

    if (FAILED(m_readback->Map(0, &read_range, (void**)&mapped)) || mapped == nullptr) {
        return;
    }

    const auto is_unorm24 = m_copy_format == DXGI_FORMAT_R24G8_TYPELESS || m_copy_format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    const auto safe_nearz = std::max(nearz, 0.001f);

    // Device depth -> view-space meters. RE Engine uses reversed-Z; device
    // depth 0 = far/infinity. z_view = nearz / device_depth.
    const auto to_view_z = [&](float device_depth) -> float {
        if (device_depth <= 1.0e-7f) {
            return 1.0e6f; // sky / no geometry
        }

        return std::clamp(safe_nearz / device_depth, safe_nearz, 1.0e6f);
    };

    std::vector<float> samples{};
    samples.reserve(NUM_ROWS * NUM_COLUMNS);

    const auto col_min = (uint32_t)(0.075f * (float)m_width);
    const auto col_max = (uint32_t)(0.925f * (float)m_width);
    const auto col_step = std::max(1u, (col_max - col_min) / NUM_COLUMNS);

    for (uint32_t i = 0; i < NUM_ROWS; ++i) {
        const auto row = (const uint8_t*)(mapped + (size_t)m_row_pitch * i);

        for (auto x = col_min; x < col_max; x += col_step) {
            float device_depth = 0.0f;

            if (is_unorm24) {
                const auto raw = *(const uint32_t*)(row + (size_t)x * 4);
                device_depth = (float)(raw & 0xFFFFFF) / 16777215.0f;
            } else {
                device_depth = *(const float*)(row + (size_t)x * 4);
            }

            samples.push_back(to_view_z(device_depth));
        }
    }

    // Center depth: median of a small patch around screen center, from the row
    // closest to mid-height.
    std::vector<float> center_samples{};
    {
        const auto center_row_idx = NUM_ROWS / 2;
        const auto row = (const uint8_t*)(mapped + (size_t)m_row_pitch * center_row_idx);
        const auto cx = (uint32_t)(m_width / 2);
        const auto start = cx >= 8 ? cx - 8 : 0;

        for (auto x = start; x < std::min<uint32_t>(start + 16, (uint32_t)m_width); ++x) {
            float device_depth = 0.0f;

            if (is_unorm24) {
                const auto raw = *(const uint32_t*)(row + (size_t)x * 4);
                device_depth = (float)(raw & 0xFFFFFF) / 16777215.0f;
            } else {
                device_depth = *(const float*)(row + (size_t)x * 4);
            }

            center_samples.push_back(to_view_z(device_depth));
        }
    }

    const D3D12_RANGE no_write{0, 0};
    m_readback->Unmap(0, &no_write);

    if (!samples.empty()) {
        // Robust "nearest": a low percentile over the wide ROI, not min() - a
        // single stray texel must not slam convergence (dynamic3d 3.1).
        const auto idx = std::max<size_t>(samples.size() / 50, 1);
        std::nth_element(samples.begin(), samples.begin() + idx, samples.end());
        m_nearest_m.store(samples[idx]);
    }

    if (!center_samples.empty()) {
        std::nth_element(center_samples.begin(), center_samples.begin() + center_samples.size() / 2, center_samples.end());
        const auto center = center_samples[center_samples.size() / 2];

        // Median then EMA with a relative deadband.
        auto ema = m_center_ema_m.load();

        if (ema <= 0.0f) {
            ema = center;
        } else if (std::abs(center - ema) / ema > 0.01f) {
            ema += (center - ema) * 0.25f;
        }

        m_center_ema_m.store(ema);
    }
}

void Flat3DDepth::reset() {
    std::scoped_lock _{m_mtx};

    if (m_commands_setup) {
        m_commands.reset();
        m_commands_setup = false;
    }

    m_readback.Reset();
    m_depth_clone.reset();
    m_last_depth = nullptr;
    m_copy_in_flight = false;
    m_width = 0;
    m_height = 0;
    m_copy_format = DXGI_FORMAT_UNKNOWN;
    m_nearest_m.store(-1.0f);
    m_center_ema_m.store(-1.0f);
}
} // namespace vrmod
