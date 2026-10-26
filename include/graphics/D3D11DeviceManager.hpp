#pragma once

#include "core/Common.hpp"
#include "core/Result.hpp"
#include "graphics/AdapterInfo.hpp"
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <vector>
#include <memory>

namespace LiteCapture::Graphics {

class D3D11DeviceManager {
public:
    D3D11DeviceManager();
    ~D3D11DeviceManager();

    // Enumerate system GPU adapters and connected displays
    Core::Result<std::vector<AdapterInfo>> EnumerateAdapters();

    // Initialize D3D11 device and immediate context
    Core::Status Initialize(int preferredAdapterIndex = -1);
    void Shutdown();

    bool IsInitialized() const { return m_device != nullptr; }

    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    IDXGIDevice* GetDXGIDevice() const { return m_dxgiDevice.Get(); }
    IDXGIAdapter1* GetActiveAdapterHandle() const { return m_activeAdapterHandle.Get(); }

    const AdapterInfo* GetActiveAdapterInfo() const {
        return m_activeAdapterIndex >= 0 && m_activeAdapterIndex < static_cast<int>(m_adapters.size())
            ? &m_adapters[m_activeAdapterIndex] : nullptr;
    }

    const std::vector<AdapterInfo>& GetAdapters() const { return m_adapters; }
    D3D_FEATURE_LEVEL GetFeatureLevel() const { return m_featureLevel; }

    // Check for GPU device removal or reset
    Core::Status CheckDeviceRemoved();

    // Helper to allocate GPU 2D textures
    Core::Result<Core::ComPtr<ID3D11Texture2D>> CreateTexture2D(
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
        UINT bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
        UINT cpuAccessFlags = 0
    );

private:
    Core::ComPtr<ID3D11Device> m_device;
    Core::ComPtr<ID3D11DeviceContext> m_context;
    Core::ComPtr<IDXGIDevice> m_dxgiDevice;
    Core::ComPtr<IDXGIAdapter1> m_activeAdapterHandle;
    Core::ComPtr<ID3D11Multithread> m_multithread;

    std::vector<AdapterInfo> m_adapters;
    int m_activeAdapterIndex{-1};
    D3D_FEATURE_LEVEL m_featureLevel{D3D_FEATURE_LEVEL_11_0};
};

} // namespace LiteCapture::Graphics
