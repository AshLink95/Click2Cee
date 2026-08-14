#include "server.hpp"

//TODO: for windows, capture with DXGI (1 frame)
#if defined(_WIN32) || defined(_WIN64)
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
bool capture_w(ComPtr<ID3D11Texture2D>& out, std::string& msg) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        msg = "D3D11CreateDevice failed";
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDev;   device.As(&dxgiDev);
    ComPtr<IDXGIAdapter> adapter;  dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIOutput> output;    adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput1> output1;  output.As(&output1); //TODO: explore multi monitor options
    ComPtr<IDXGIOutputDuplication> dupl;
    if (FAILED(output1->DuplicateOutput(device.Get(), &dupl))) {
        msg = "DuplicateOutput failed";
        return false;
    }

    ComPtr<IDXGIResource> res;
    DXGI_OUTDUPL_FRAME_INFO info;
    HRESULT hr = DXGI_ERROR_WAIT_TIMEOUT;
    for (int i = 0; i < 10 && hr == DXGI_ERROR_WAIT_TIMEOUT; ++i)
        hr = dupl->AcquireNextFrame(500, &info, &res);
    if (FAILED(hr)) {
        msg = "AcquireNextFrame failed";
        return false;
    }

    ComPtr<ID3D11Texture2D> gpuTex;  res.As(&gpuTex);
    D3D11_TEXTURE2D_DESC desc;       gpuTex->GetDesc(&desc);
    msg = std::format("captured {0}x{1}", desc.Width, desc.Height);
    desc.Usage          = D3D11_USAGE_STAGING;
    desc.BindFlags      = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags      = 0;
    device->CreateTexture2D(&desc, nullptr, &out);
    ctx->CopyResource(out.Get(), gpuTex.Get());
    dupl->ReleaseFrame();

    //TODO: compress/encode video with H.246 (use NVENC)

    return true;
}

//TODO: for linux, capture with pipewire
#elif defined(__linux__)
bool capture_l(std::vector<uint8_t>& out, std::string& msg) { //TODO
    return false;
}
#endif

//TODO: allow sending through UDP or TCP

log_entry snd(snd_typ typ) {
    ComPtr<ID3D11Texture2D> out;
    std::string message;
    #if defined(_WIN32) || defined(_WIN64)
        bool status = capture_w(out, message);
    #elif defined(__linux__)
    bool status = capture_l(out, message);
    #endif
    return log_entry(status, message);
}
