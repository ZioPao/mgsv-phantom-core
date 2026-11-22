#include "core/gpu_timer.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using Microsoft::WRL::ComPtr;
namespace gpu = fxcore::gpu;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void passesAreNamedOnce()
{
    const auto a = gpu::registerPass("Test pass A");
    const auto b = gpu::registerPass("Test pass B");
    expect(a != b, "two names are two passes");
    expect(gpu::registerPass("Test pass A") == a, "the same name is the same pass");

    // More passes than there is room for share the last.
    gpu::Pass last{};
    for (int i = 0; i < 100; ++i)
    {
        static std::string names[100];
        names[i] = "Overflow " + std::to_string(i);
        last = gpu::registerPass(names[i].c_str());
    }
    expect(last == gpu::MaxPasses - 1, "passes beyond the room count under the last");
}

void reportsWhatRan()
{
    gpu::Costs costs;
    costs.frame = 20.0f;
    costs.gpuFrame = 12.0f;
    costs.passes = {{"Pool", 0.5f}, {"Decals", 0.0f}, {"Wound", 1.5f}};
    const auto text = gpu::report(costs);
    expect(text.find("2.00 ms") != std::string::npos, "the report adds the passes");
    expect(text.find("10.0%") != std::string::npos, "the report gives the share of the frame");
    expect(text.find("Pool 0.50") != std::string::npos && text.find("Wound 1.50") != std::string::npos, "the report names the passes that ran");
    expect(text.find("Decals") == std::string::npos, "the report leaves out a pass that did not run");
}

// Real timestamp queries on the software device: a pass that clears a large target takes time, and a pass nothing was
// recorded in does not. Skipped where the device cannot be made.
bool timesAPassOnTheGpu()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
            context.GetAddressOf())))
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 2048;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf())) || FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, target.GetAddressOf())))
        return false;

    const auto busy = gpu::registerPass("Busy");
    const auto idle = gpu::registerPass("Idle");
    gpu::attach(device.Get(), context.Get());
    gpu::watch();

    const float colour[4]{0.2f, 0.4f, 0.6f, 1.0f};
    float busyMilliseconds{};
    for (int frame = 0; frame < 400 && busyMilliseconds <= 0.0f; ++frame)
    {
        gpu::watch();
        gpu::executing(1);
        {
            gpu::Timed timed(context.Get(), busy);
            for (int i = 0; i < 20; ++i) context->ClearRenderTargetView(target.Get(), colour);
        }
        gpu::present();
        context->Flush();
        Sleep(5);

        for (const auto& pass : gpu::status().costs.passes)
            if (std::string(pass.name) == "Busy") busyMilliseconds = pass.milliseconds;
    }

    float idleMilliseconds{-1};
    for (const auto& pass : gpu::status().costs.passes)
        if (std::string(pass.name) == "Idle") idleMilliseconds = pass.milliseconds;
    (void)idle;

    const auto costs = gpu::status().costs;
    gpu::detach();
    expect(busyMilliseconds > 0.0f, "a pass that cleared a large target took GPU time");
    expect(idleMilliseconds == 0.0f, "a pass nothing was recorded in costs nothing");
    expect(costs.frame > 0.0f, "the frame's time was measured");

    return true;
}
}

int main()
{
    try
    {
        // First: the overflow test below fills every pass there is room for.
        if (!timesAPassOnTheGpu()) std::cout << "GPU timer: no software device here; the timed run was skipped.\n";
        reportsWhatRan();
        passesAreNamedOnce();
        std::cout << "Core GPU timer tests passed.\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "Core GPU timer tests failed: " << error.what() << '\n';

        return 1;
    }

    return 0;
}
