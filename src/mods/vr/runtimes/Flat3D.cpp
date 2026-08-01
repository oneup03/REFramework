#include "../../VR.hpp"

#include "Flat3D.hpp"

namespace runtimes {
uint32_t Flat3D::get_width() const {
    if (g_framework != nullptr) {
        if (g_framework->is_dx12()) {
            if (auto& hook = g_framework->get_d3d12_hook(); hook != nullptr) {
                if (auto* swapchain = hook->get_swap_chain(); swapchain != nullptr) {
                    DXGI_SWAP_CHAIN_DESC1 desc{};
                    if (SUCCEEDED(swapchain->GetDesc1(&desc))) {
                        m_last_width = desc.Width;
                        m_last_height = desc.Height;
                    }
                }
            }
        } else {
            if (auto& hook = g_framework->get_d3d11_hook(); hook != nullptr) {
                if (auto* swapchain = hook->get_swap_chain(); swapchain != nullptr) {
                    DXGI_SWAP_CHAIN_DESC desc{};
                    if (SUCCEEDED(swapchain->GetDesc(&desc))) {
                        m_last_width = desc.BufferDesc.Width;
                        m_last_height = desc.BufferDesc.Height;
                    }
                }
            }
        }
    }

    if (m_last_width == 0) {
        m_last_width = (uint32_t)GetSystemMetrics(SM_CXSCREEN);
        m_last_height = (uint32_t)GetSystemMetrics(SM_CYSCREEN);
    }

    return m_last_width;
}

uint32_t Flat3D::get_height() const {
    if (m_last_height == 0) {
        get_width(); // refreshes both dimensions
    }

    return m_last_height;
}

VRRuntime::Error Flat3D::update_matrices(float nearz, float farz) {
    std::unique_lock __{ this->projections_mtx };
    std::unique_lock ___{ this->eyes_mtx };

    // Same convention as OpenVR's GetEyeToHeadTransform: left eye sits at -x.
    // VR::on_camera_get_view_matrix applies these with the flipped-eye trick,
    // which handles the view-matrix inversion.
    const auto half_sep = this->separation_eff * 0.5f;

    this->eyes[(uint32_t)VRRuntime::Eye::LEFT] = glm::identity<Matrix4x4f>();
    this->eyes[(uint32_t)VRRuntime::Eye::LEFT][3] = Vector4f{-half_sep, 0.0f, 0.0f, 1.0f};
    this->eyes[(uint32_t)VRRuntime::Eye::RIGHT] = glm::identity<Matrix4x4f>();
    this->eyes[(uint32_t)VRRuntime::Eye::RIGHT][3] = Vector4f{half_sep, 0.0f, 0.0f, 1.0f};

    // Never consumed in flat3d (the projection hook shears the game's own
    // matrix in place instead of replacing it), kept as identity for safety.
    this->projections[(uint32_t)VRRuntime::Eye::LEFT] = glm::identity<Matrix4x4f>();
    this->projections[(uint32_t)VRRuntime::Eye::RIGHT] = glm::identity<Matrix4x4f>();

    return VRRuntime::Error::SUCCESS;
}
}
