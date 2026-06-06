#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cwchar>

constexpr UINT kD3DSdkVersion = 32;
constexpr UINT kCutsceneDofCompositeResource = 5000;

using Direct3DCreate9Proc = IDirect3D9* (WINAPI*)(UINT);

int main()
{
    wchar_t cwd[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    std::wprintf(L"cwd=%ls\n", cwd);

    wchar_t d3d9Path[MAX_PATH] = {};
    GetFullPathNameW(L"D3D9.dll", MAX_PATH, d3d9Path, nullptr);
    std::wprintf(L"request=%ls\n", d3d9Path);

    HMODULE d3d9 = LoadLibraryW(d3d9Path);
    if (!d3d9) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    wchar_t loadedPath[MAX_PATH] = {};
    GetModuleFileNameW(d3d9, loadedPath, MAX_PATH);
    std::wprintf(L"loaded=%ls\n", loadedPath);

    auto create = reinterpret_cast<Direct3DCreate9Proc>(GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!create) {
        std::printf("GetProcAddress failed: %lu\n", GetLastError());
        return 2;
    }

    IDirect3D9* object = create(kD3DSdkVersion);
    if (!object) {
        std::printf("Direct3DCreate9 returned null\n");
        return 3;
    }

    std::printf("Direct3DCreate9 OK\n");

    HWND window = CreateWindowExW(
        0,
        L"STATIC",
        L"HotD post-FX smoke test",
        WS_OVERLAPPED,
        0,
        0,
        64,
        64,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (!window) {
        object->Release();
        return 4;
    }

    D3DPRESENT_PARAMETERS present = {};
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.BackBufferFormat = D3DFMT_UNKNOWN;
    present.hDeviceWindow = window;

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = object->CreateDevice(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &present,
        &device);
    if (FAILED(hr) || !device) {
        std::printf("CreateDevice failed: %08lX\n", static_cast<unsigned long>(hr));
        DestroyWindow(window);
        object->Release();
        return 5;
    }

    HRSRC resource = FindResourceW(
        d3d9,
        MAKEINTRESOURCEW(kCutsceneDofCompositeResource),
        MAKEINTRESOURCEW(10));
    HGLOBAL loaded = resource ? LoadResource(d3d9, resource) : nullptr;
    const DWORD size = resource ? SizeofResource(d3d9, resource) : 0;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || size < 8) {
        std::printf("Corrected shader resource missing\n");
        device->Release();
        DestroyWindow(window);
        object->Release();
        return 6;
    }

    IDirect3DPixelShader9* shader = nullptr;
    hr = device->CreatePixelShader(static_cast<const DWORD*>(data), &shader);
    if (FAILED(hr) || !shader) {
        std::printf("Corrected shader validation failed: %08lX\n", static_cast<unsigned long>(hr));
        device->Release();
        DestroyWindow(window);
        object->Release();
        return 7;
    }

    std::printf("Corrected cutscene DoF shader OK (%lu bytes)\n", static_cast<unsigned long>(size));
    shader->Release();
    device->Release();
    DestroyWindow(window);
    object->Release();
    return 0;
}
