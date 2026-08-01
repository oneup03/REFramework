#include <spdlog/spdlog.h>
#include <utility/String.hpp>

#include "Flat3DLeiaSR.hpp"

#ifdef REF_LEIASR

#include <sr/management/srcontext.h>
#include <sr/weaver/dx12weaver.h>

namespace vrmod {
bool Flat3DLeiaSR::available() {
    if (m_checked_available) {
        return m_available;
    }

    m_checked_available = true;
    m_available = false;

    // Preflight: the SR import libs are /DELAYLOAD'ed, so touching any SR
    // symbol without the runtime installed would raise a module-not-found
    // exception. LoadLibraryW resolves through the SR runtime's PATH entry.
    static const wchar_t* required_dlls[]{
        L"SimulatedRealityCore.dll",
        L"SimulatedRealityDisplays.dll",
        L"SimulatedRealityDirectX.dll",
        L"simulatedreality.dll",
    };

    for (const auto dll : required_dlls) {
        if (LoadLibraryW(dll) == nullptr) {
            spdlog::info("[Flat3D] LeiaSR runtime not present ({} missing); LeiaSR mode will fall back to SbS", utility::narrow(dll));
            return false;
        }
    }

    m_available = true;
    return true;
}

bool Flat3DLeiaSR::init(ID3D12Device* device, HWND window) {
    if (m_weaver != nullptr) {
        return true;
    }

    if (m_init_attempted) {
        return false; // failed before; don't retry every frame
    }

    if (device == nullptr || window == nullptr || !available()) {
        return false;
    }

    m_init_attempted = true;

    // The project compiles with /EHa, so catch(...) also covers SEH from the
    // delay-load thunks and SR service connection failures.
    try {
        if (m_context == nullptr) {
            m_context = SR::SRContext::create();
        }

        if (m_context == nullptr) {
            spdlog::error("[Flat3D] SRContext::create failed (is the SR service running?)");
            return false;
        }

        SR::IDX12Weaver1* weaver = nullptr;
        const auto err = SR::CreateDX12Weaver(m_context, device, window, &weaver);

        if (err != WeaverSuccess || weaver == nullptr) {
            spdlog::error("[Flat3D] CreateDX12Weaver failed: {}", (int)err);
            return false;
        }

        m_weaver = weaver;

        // Eye textures are plain UNORM (no sRGB views) on both ends.
        m_weaver->setShaderSRGBConversion(false, false);
        m_weaver->setLatencyInFrames(2);

        // Must happen AFTER the weaver exists: starts eye tracking and binds
        // it to the created weaver(s).
        m_context->initialize();

        spdlog::info("[Flat3D] LeiaSR weaver created and SR context initialized");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[Flat3D] LeiaSR init failed: {}", e.what());
    } catch (...) {
        spdlog::error("[Flat3D] LeiaSR init failed (unknown/SEH exception)");
    }

    m_weaver = nullptr;
    return false;
}

void Flat3DLeiaSR::set_input(ID3D12Resource* sbs_texture, int width, int height, DXGI_FORMAT format) {
    if (m_weaver == nullptr || sbs_texture == nullptr) {
        return;
    }

    if (m_last_input == sbs_texture) {
        return;
    }

    try {
        m_weaver->setInputViewTexture(sbs_texture, width, height, format);
        m_last_input = sbs_texture;
    } catch (...) {
        spdlog::error("[Flat3D] LeiaSR setInputViewTexture failed");
        destroy_weaver();
    }
}

bool Flat3DLeiaSR::weave(ID3D12GraphicsCommandList* cmd_list, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor, DXGI_FORMAT output_format) {
    if (m_weaver == nullptr || cmd_list == nullptr || m_last_input == nullptr) {
        return false;
    }

    try {
        if (m_last_output_format != output_format) {
            m_weaver->setOutputFormat(output_format);
            m_last_output_format = output_format;
        }

        m_weaver->setCommandList(cmd_list);
        m_weaver->setViewport(viewport);
        m_weaver->setScissorRect(scissor);
        m_weaver->weave();
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
    if (m_weaver != nullptr) {
        try {
            m_weaver->destroy();
        } catch (...) {
        }

        m_weaver = nullptr;
    }

    m_last_input = nullptr;
    m_last_output_format = DXGI_FORMAT_UNKNOWN;
    m_init_attempted = false; // allow re-init (e.g. after device reset)
}
} // namespace vrmod

#else // !REF_LEIASR

namespace vrmod {
bool Flat3DLeiaSR::available() {
    return false;
}

bool Flat3DLeiaSR::init(ID3D12Device*, HWND) {
    return false;
}

void Flat3DLeiaSR::set_input(ID3D12Resource*, int, int, DXGI_FORMAT) {
}

bool Flat3DLeiaSR::weave(ID3D12GraphicsCommandList*, const D3D12_VIEWPORT&, const D3D12_RECT&, DXGI_FORMAT) {
    return false;
}

void Flat3DLeiaSR::destroy_weaver() {
}
} // namespace vrmod

#endif // REF_LEIASR
