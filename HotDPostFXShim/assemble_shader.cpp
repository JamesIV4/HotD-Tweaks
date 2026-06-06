#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

#include <cstdio>
#include <vector>

struct ID3DXBuffer : public IUnknown {
    virtual void* STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0;
};

using D3DXAssembleShaderProc = HRESULT (WINAPI*)(
    const char*,
    UINT,
    const void*,
    void*,
    DWORD,
    ID3DXBuffer**,
    ID3DXBuffer**);

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: assemble_shader input.asm output.bin\n");
        return 2;
    }

    FILE* input = nullptr;
    if (_wfopen_s(&input, argv[1], L"rb") != 0 || !input) {
        std::fwprintf(stderr, L"could not open %ls\n", argv[1]);
        return 3;
    }

    std::fseek(input, 0, SEEK_END);
    const long length = std::ftell(input);
    std::fseek(input, 0, SEEK_SET);
    if (length <= 0) {
        std::fclose(input);
        return 4;
    }

    std::vector<char> source(static_cast<size_t>(length));
    const size_t read = std::fread(source.data(), 1, source.size(), input);
    std::fclose(input);
    if (read != source.size()) {
        return 5;
    }

    HMODULE d3dx = LoadLibraryW(L"d3dx9_43.dll");
    if (!d3dx) {
        std::fwprintf(stderr, L"could not load d3dx9_43.dll\n");
        return 6;
    }

    auto assemble = reinterpret_cast<D3DXAssembleShaderProc>(
        GetProcAddress(d3dx, "D3DXAssembleShader"));
    if (!assemble) {
        FreeLibrary(d3dx);
        return 7;
    }

    ID3DXBuffer* shader = nullptr;
    ID3DXBuffer* errors = nullptr;
    const HRESULT hr = assemble(
        source.data(),
        static_cast<UINT>(source.size()),
        nullptr,
        nullptr,
        0,
        &shader,
        &errors);
    if (FAILED(hr) || !shader) {
        if (errors) {
            std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
            errors->Release();
        }
        FreeLibrary(d3dx);
        return 8;
    }

    FILE* output = nullptr;
    if (_wfopen_s(&output, argv[2], L"wb") != 0 || !output) {
        shader->Release();
        if (errors) {
            errors->Release();
        }
        FreeLibrary(d3dx);
        return 9;
    }

    std::fwrite(shader->GetBufferPointer(), 1, shader->GetBufferSize(), output);
    std::fclose(output);
    shader->Release();
    if (errors) {
        errors->Release();
    }
    FreeLibrary(d3dx);
    return 0;
}
