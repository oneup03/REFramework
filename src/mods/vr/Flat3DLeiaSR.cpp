#include <spdlog/spdlog.h>

#include "Flat3DLeiaSR.hpp"

#ifdef REF_LEIASR

#include <exception>

#include <SR.hpp>

namespace vrmod {
bool Flat3DLeiaSR::init(ID3D12Device* device, HWND window) {
    if (m_sr != nullptr) {
        return true;
    }

    if (m_init_attempted) {
        return false; // failed before; don't retry every frame
    }

    if (device == nullptr || window == nullptr) {
        return false;
    }

    m_init_attempted = true;

    // One call replaces what used to be the DLL preflight, SRContext::create,
    // CreateDX12Weaver and SRContext::initialize here. SR-lib performs them in
    // the order the SDK requires and probes the delay-loaded SR runtime (core
    // *and* the DirectX weaver DLL) before touching any SDK entry point, so a
    // machine without SR Platform installed returns a failed HRESULT rather
    // than raising SEH out of a delay-load thunk.
    //
    // The project compiles with /EHa, so the catch below also covers anything
    // the SDK throws past SR-lib's own handling.
    try {
        SimulatedReality::SRInterfaceDX12* sr = nullptr;
        const auto hr = SimulatedReality::CreateSRInterfaceDX12(device, window, &sr);

        if (FAILED(hr) || sr == nullptr) {
            spdlog::info("[Flat3D] LeiaSR unavailable (CreateSRInterfaceDX12 hr 0x{:08X}); falling back to SbS", (uint32_t)hr);
            return false;
        }

        m_sr = sr;

        // Eye textures are plain UNORM (no sRGB views) on both ends, so the
        // weaver must not convert either direction or it double-applies gamma.
        m_sr->SetShaderSRGBConversion(false, false);

        // SR-lib defaults to 1; we sit a frame deeper behind the game's own
        // present pipeline.
        m_sr->SetLatencyInFrames(2);

        spdlog::info("[Flat3D] LeiaSR weaver created and SR context initialized");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[Flat3D] LeiaSR init failed: {}", e.what());
    } catch (...) {
        spdlog::error("[Flat3D] LeiaSR init failed (unknown/SEH exception)");
    }

    m_sr = nullptr;
    return false;
}

void Flat3DLeiaSR::set_input(ID3D12Resource* sbs_texture) {
    if (m_sr == nullptr || sbs_texture == nullptr) {
        return;
    }

    // Re-bound every frame rather than cached on the pointer: the same resource
    // can come back with the SDK's internal view invalidated by external state
    // changes (swapchain resize and friends).
    try {
        m_sr->SetInputTexture(sbs_texture);
    } catch (...) {
        spdlog::error("[Flat3D] LeiaSR SetInputTexture failed");
        destroy_weaver();
    }
}

bool Flat3DLeiaSR::weave(ID3D12GraphicsCommandList* cmd_list, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor, DXGI_FORMAT output_format) {
    if (m_sr == nullptr || cmd_list == nullptr) {
        return false;
    }

    try {
        if (m_last_output_format != output_format) {
            m_sr->SetOutputFormat(output_format);
            m_last_output_format = output_format;
        }

        m_sr->Weave(cmd_list, viewport, scissor);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[Flat3D] LeiaSR weave failed: {}", e.what());
    } catch (...) {
        spdlog::error("[Flat3D] LeiaSR weave failed (unknown/SEH exception)");
    }

    destroy_weaver();
    return false;
}

void Flat3DLeiaSR::destroy_weaver() {
    if (m_sr != nullptr) {
        // Delete() destroys the weaver and then releases the SRContext once no
        // other interface is using it. Never `delete` it - the objects live in
        // the SR DLL and the weaver is an IDestroyable.
        try {
            m_sr->Delete();
        } catch (...) {
        }

        m_sr = nullptr;
    }

    m_last_output_format = DXGI_FORMAT_UNKNOWN;
    m_init_attempted = false; // allow re-init (e.g. after device reset)
}
} // namespace vrmod

#else // !REF_LEIASR

namespace vrmod {
bool Flat3DLeiaSR::init(ID3D12Device*, HWND) {
    return false;
}

void Flat3DLeiaSR::set_input(ID3D12Resource*) {
}

bool Flat3DLeiaSR::weave(ID3D12GraphicsCommandList*, const D3D12_VIEWPORT&, const D3D12_RECT&, DXGI_FORMAT) {
    return false;
}

void Flat3DLeiaSR::destroy_weaver() {
}
} // namespace vrmod

#endif // REF_LEIASR
