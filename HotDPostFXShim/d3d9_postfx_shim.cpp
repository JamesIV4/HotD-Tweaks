#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT kCreateDeviceIndex = 16;
constexpr UINT kCreatePixelShaderIndex = 106;
constexpr UINT kSetPixelShaderIndex = 107;
constexpr UINT kSetPixelShaderConstantFIndex = 109;
constexpr UINT kDrawPrimitiveIndex = 81;
constexpr UINT kDrawIndexedPrimitiveIndex = 82;
constexpr UINT kDrawPrimitiveUPIndex = 83;
constexpr UINT kDrawIndexedPrimitiveUPIndex = 84;
constexpr UINT kMaxPixelShaderConstants = 256;
constexpr UINT kCutsceneDofCompositeResource = 5000;

enum FixKind {
    FixInvTex,
    FixBufferOffset
};

struct Config {
    bool enabled = true;
    bool disableCutsceneEffects = false;
    bool log = true;
    bool adjustInvTex = false;
    bool adjustBufferOffset = false;
    bool installShaderHooks = true;
    bool dumpTargetShaders = true;
    bool logShaderCreation = true;
    bool patchOnDraw = true;
    bool logFullscreenDraws = true;
    bool patchFullscreenSmallConstants = true;
    float scale = 3.0f;
    float bloomRadiusDivisor = 0.0f;
    float cutsceneBloomIntensity = 0.666667f;
    float cutsceneBloomOffsetXTexels = -0.4375f;
    float cutsceneDofRadiusDivisor = 0.0f;
    float cutsceneDofBlurOffsetXTexels = -2.0f;
    float cutsceneDofBlurOffsetYTexels = -1.75f;
    float cutsceneDofMaskOffsetXTexels = 0.0f;
    float cutsceneDofMaskOffsetYTexels = 0.0f;
    float cutsceneDofStrength = 1.0f;
    int maxAdjustmentLogs = 200;
    int maxShaderCreationLogs = 200;
    int maxTargetShaderLogs = 200;
    int maxDrawPatchLogs = 200;
    int maxFullscreenDrawLogs = 400;
};

struct FixRange {
    std::string name;
    UINT reg = 0;
    UINT count = 0;
    FixKind kind = FixInvTex;
};

struct ShaderInfo {
    std::vector<FixRange> fixes;
};

struct DeviceState {
    IDirect3DPixelShader9* currentPixelShader = nullptr;
    bool psConstantValid[kMaxPixelShaderConstants] = {};
    float psConstants[kMaxPixelShaderConstants][4] = {};
    bool cutscenePipelineActive = false;
    float bloomOutputScale = 1.0f;
    UINT logicalOutputWidth = 1280;
    UINT logicalOutputHeight = 720;
    IDirect3DPixelShader9* correctedDofCompositeShader = nullptr;
};

struct DrawPatch {
    UINT reg = 0;
    UINT count = 0;
    std::string name;
    std::vector<float> original;
    std::vector<float> patched;
};

using Direct3DCreate9Proc = IDirect3D9* (WINAPI*)(UINT);
using Direct3DCreate9ExProc = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
using CreateDeviceProc = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using CreatePixelShaderProc = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using SetPixelShaderProc = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPixelShaderConstantFProc = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, const float*, UINT);
using DrawPrimitiveProc = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveProc = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPProc = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPProc = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);

HMODULE g_module = nullptr;
HMODULE g_realD3D9 = nullptr;
bool g_loadAttempted = false;
std::wstring g_moduleDir;
std::wstring g_iniPath;
std::wstring g_logPath;
Config g_config;
bool g_configLoaded = false;

std::mutex g_mutex;
std::unordered_map<IDirect3DPixelShader9*, ShaderInfo> g_shaderInfo;
std::unordered_map<IDirect3DDevice9*, DeviceState> g_deviceState;
std::unordered_set<std::string> g_loggedFullscreenSignatures;
std::unordered_set<DWORD> g_dumpedCinematicPixelShaders;
std::unordered_set<DWORD> g_dumpedCinematicVertexShaders;
std::unordered_map<DWORD, int> g_cinematicConstantLogCounts;

CreateDeviceProc g_realCreateDevice = nullptr;
CreatePixelShaderProc g_realCreatePixelShader = nullptr;
SetPixelShaderProc g_realSetPixelShader = nullptr;
SetPixelShaderConstantFProc g_realSetPixelShaderConstantF = nullptr;
DrawPrimitiveProc g_realDrawPrimitive = nullptr;
DrawIndexedPrimitiveProc g_realDrawIndexedPrimitive = nullptr;
DrawPrimitiveUPProc g_realDrawPrimitiveUP = nullptr;
DrawIndexedPrimitiveUPProc g_realDrawIndexedPrimitiveUP = nullptr;

int g_adjustmentLogs = 0;
int g_shaderCreationLogs = 0;
int g_targetShaderLogs = 0;
int g_drawPatchLogs = 0;
int g_liveShaderIdentifyLogs = 0;
int g_fullscreenDrawLogs = 0;
int g_cutsceneDofBypassLogs = 0;
int g_cutsceneDofFixLogs = 0;
bool g_cutsceneDofShaderResourceErrorLogged = false;
thread_local bool g_internalConstantUpdate = false;

HRESULT STDMETHODCALLTYPE CreateDevice(
    IDirect3D9* self,
    UINT adapter,
    D3DDEVTYPE deviceType,
    HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* presentationParameters,
    IDirect3DDevice9** returnedDevice);
HRESULT STDMETHODCALLTYPE CreatePixelShader(
    IDirect3DDevice9* self,
    const DWORD* function,
    IDirect3DPixelShader9** shader);
HRESULT STDMETHODCALLTYPE SetPixelShader(
    IDirect3DDevice9* self,
    IDirect3DPixelShader9* shader);
HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(
    IDirect3DDevice9* self,
    UINT startRegister,
    const float* constantData,
    UINT vector4fCount);
HRESULT STDMETHODCALLTYPE DrawPrimitive(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    UINT startVertex,
    UINT primitiveCount);
HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    INT baseVertexIndex,
    UINT minVertexIndex,
    UINT numVertices,
    UINT startIndex,
    UINT primitiveCount);
HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    UINT primitiveCount,
    const void* vertexStreamZeroData,
    UINT vertexStreamZeroStride);
HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    UINT minVertexIndex,
    UINT numVertices,
    UINT primitiveCount,
    const void* indexData,
    D3DFORMAT indexDataFormat,
    const void* vertexStreamZeroData,
    UINT vertexStreamZeroStride);

std::wstring GetModuleDirectory()
{
    if (!g_moduleDir.empty()) {
        return g_moduleDir;
    }

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_module, path, MAX_PATH);
    g_moduleDir.assign(path);

    const size_t slash = g_moduleDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        g_moduleDir.resize(slash + 1);
    } else {
        g_moduleDir.clear();
    }

    return g_moduleDir;
}

float ReadIniFloat(const wchar_t* section, const wchar_t* key, float fallback)
{
    wchar_t value[64] = {};
    wchar_t fallbackText[64] = {};
    swprintf_s(fallbackText, L"%.6f", fallback);
    GetPrivateProfileStringW(section, key, fallbackText, value, ARRAYSIZE(value), g_iniPath.c_str());
    return static_cast<float>(_wtof(value));
}

void LoadConfig()
{
    if (g_configLoaded) {
        return;
    }

    const std::wstring dir = GetModuleDirectory();
    g_iniPath = dir + L"HotDPostFXFix.ini";
    g_logPath = dir + L"HotDPostFXFix.log";

    g_config.enabled = GetPrivateProfileIntW(L"PostFXFix", L"Enabled", 1, g_iniPath.c_str()) != 0;
    g_config.disableCutsceneEffects =
        GetPrivateProfileIntW(L"PostFXFix", L"DisableCutsceneEffects", 0, g_iniPath.c_str()) != 0;
    g_config.log = GetPrivateProfileIntW(L"PostFXFix", L"Log", 1, g_iniPath.c_str()) != 0;
    g_config.adjustInvTex = GetPrivateProfileIntW(L"PostFXFix", L"AdjustInvTex", 0, g_iniPath.c_str()) != 0;
    g_config.adjustBufferOffset = GetPrivateProfileIntW(L"PostFXFix", L"AdjustBufferOffset", 0, g_iniPath.c_str()) != 0;
    g_config.installShaderHooks = GetPrivateProfileIntW(L"PostFXFix", L"InstallShaderHooks", 1, g_iniPath.c_str()) != 0;
    g_config.dumpTargetShaders = GetPrivateProfileIntW(L"PostFXFix", L"DumpTargetShaders", 1, g_iniPath.c_str()) != 0;
    g_config.logShaderCreation = GetPrivateProfileIntW(L"PostFXFix", L"LogShaderCreation", 1, g_iniPath.c_str()) != 0;
    g_config.patchOnDraw = GetPrivateProfileIntW(L"PostFXFix", L"PatchOnDraw", 1, g_iniPath.c_str()) != 0;
    g_config.logFullscreenDraws = GetPrivateProfileIntW(L"PostFXFix", L"LogFullscreenDraws", 1, g_iniPath.c_str()) != 0;
    g_config.patchFullscreenSmallConstants = GetPrivateProfileIntW(L"PostFXFix", L"PatchFullscreenSmallConstants", 1, g_iniPath.c_str()) != 0;
    g_config.scale = ReadIniFloat(L"PostFXFix", L"Scale", 1.0f);
    g_config.bloomRadiusDivisor = ReadIniFloat(L"PostFXFix", L"BloomRadiusDivisor", 0.0f);
    g_config.cutsceneBloomIntensity =
        ReadIniFloat(L"PostFXFix", L"CutsceneBloomIntensity", 0.666667f);
    g_config.cutsceneBloomOffsetXTexels =
        ReadIniFloat(L"PostFXFix", L"CutsceneBloomOffsetXTexels", -0.4375f);
    g_config.cutsceneDofRadiusDivisor =
        ReadIniFloat(L"PostFXFix", L"CutsceneDofRadiusDivisor", 0.0f);
    g_config.cutsceneDofBlurOffsetXTexels =
        ReadIniFloat(L"PostFXFix", L"CutsceneDofBlurOffsetXTexels", -2.0f);
    g_config.cutsceneDofBlurOffsetYTexels =
        ReadIniFloat(L"PostFXFix", L"CutsceneDofBlurOffsetYTexels", -1.75f);
    g_config.cutsceneDofMaskOffsetXTexels =
        ReadIniFloat(L"PostFXFix", L"CutsceneDofMaskOffsetXTexels", 0.0f);
    g_config.cutsceneDofMaskOffsetYTexels =
        ReadIniFloat(L"PostFXFix", L"CutsceneDofMaskOffsetYTexels", 0.0f);
    g_config.cutsceneDofStrength =
        ReadIniFloat(L"PostFXFix", L"CutsceneDofStrength", 1.0f);
    g_config.maxAdjustmentLogs = GetPrivateProfileIntW(L"PostFXFix", L"MaxAdjustmentLogs", 200, g_iniPath.c_str());
    g_config.maxShaderCreationLogs = GetPrivateProfileIntW(L"PostFXFix", L"MaxShaderCreationLogs", 200, g_iniPath.c_str());
    g_config.maxTargetShaderLogs = GetPrivateProfileIntW(L"PostFXFix", L"MaxTargetShaderLogs", 200, g_iniPath.c_str());
    g_config.maxDrawPatchLogs = GetPrivateProfileIntW(L"PostFXFix", L"MaxDrawPatchLogs", 200, g_iniPath.c_str());
    g_config.maxFullscreenDrawLogs = GetPrivateProfileIntW(L"PostFXFix", L"MaxFullscreenDrawLogs", 400, g_iniPath.c_str());

    if (g_config.scale <= 0.01f) {
        g_config.scale = 1.0f;
    }
    g_config.cutsceneBloomIntensity =
        std::max(0.0f, std::min(2.0f, g_config.cutsceneBloomIntensity));
    g_config.cutsceneDofStrength =
        std::max(0.0f, std::min(1.0f, g_config.cutsceneDofStrength));

    g_configLoaded = true;
}

void Log(const char* format, ...)
{
    LoadConfig();
    if (!g_config.log) {
        return;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    FILE* file = nullptr;
    if (_wfopen_s(&file, g_logPath.c_str(), L"a") != 0 || !file) {
        return;
    }

    std::fprintf(
        file,
        "[%04u-%02u-%02u %02u:%02u:%02u] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);

    std::fprintf(file, "\n");
    std::fclose(file);
}

HMODULE LoadRealD3D9()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_realD3D9 || g_loadAttempted) {
        return g_realD3D9;
    }

    g_loadAttempted = true;
    LoadConfig();

    const std::wstring dir = GetModuleDirectory();
    const std::wstring backendPath = dir + L"dgVoodooBackend\\D3D9.dll";
    const std::wstring fallbackPath = dir + L"dgVoodoo_D3D9.dll";

    std::wstring path = backendPath;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        path = fallbackPath;
    }

    g_realD3D9 = LoadLibraryW(path.c_str());
    if (!g_realD3D9) {
        Log("ERROR: failed to load backend D3D9 from %ls. GetLastError=%lu", path.c_str(), GetLastError());
    } else {
        Log("Loaded backend D3D9 from %ls. Enabled=%d DisableCutsceneEffects=%d Scale=%.3f BloomRadiusDivisor=%.3f BloomIntensity=%.3f BloomOffsetXTexels=%.4f DofRadiusDivisor=%.3f DofBlurOffset=(%.4f,%.4f) DofMaskOffset=(%.4f,%.4f) DofStrength=%.3f ShaderHooks=%d AdjustInvTex=%d AdjustBufferOffset=%d",
            path.c_str(),
            g_config.enabled ? 1 : 0,
            g_config.disableCutsceneEffects ? 1 : 0,
            g_config.scale,
            g_config.bloomRadiusDivisor,
            g_config.cutsceneBloomIntensity,
            g_config.cutsceneBloomOffsetXTexels,
            g_config.cutsceneDofRadiusDivisor,
            g_config.cutsceneDofBlurOffsetXTexels,
            g_config.cutsceneDofBlurOffsetYTexels,
            g_config.cutsceneDofMaskOffsetXTexels,
            g_config.cutsceneDofMaskOffsetYTexels,
            g_config.cutsceneDofStrength,
            g_config.installShaderHooks ? 1 : 0,
            g_config.adjustInvTex ? 1 : 0,
            g_config.adjustBufferOffset ? 1 : 0);
    }

    return g_realD3D9;
}

FARPROC RealProc(const char* name)
{
    HMODULE real = LoadRealD3D9();
    return real ? GetProcAddress(real, name) : nullptr;
}

bool PatchVtableSlot(void* object, UINT index, void* hook, void** original)
{
    if (!object) {
        return false;
    }

    void*** objectAsVtable = reinterpret_cast<void***>(object);
    void** vtable = *objectAsVtable;
    if (!vtable || vtable[index] == hook) {
        return true;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("ERROR: VirtualProtect failed while patching vtable slot %u. GetLastError=%lu", index, GetLastError());
        return false;
    }

    if (*original == nullptr) {
        *original = vtable[index];
    }

    vtable[index] = hook;

    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored);
    return true;
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool IsPrintableString(const char* text, size_t maxLen)
{
    if (!text || !*text) {
        return false;
    }

    for (size_t i = 0; i < maxLen && text[i]; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch < 32 || ch > 126) {
            return false;
        }
    }

    return true;
}

bool ReadStringFromOffset(const unsigned char* base, size_t totalBytes, DWORD offset, std::string& out)
{
    if (offset >= totalBytes) {
        return false;
    }

    const char* text = reinterpret_cast<const char*>(base + offset);
    const size_t maxLen = totalBytes - offset;
    if (!IsPrintableString(text, maxLen)) {
        return false;
    }

    out.assign(text);
    return true;
}

bool NameToFixKind(const std::string& name, FixKind& kind)
{
    const std::string lower = Lower(name);

    if (lower == "cinvtexsize" || lower == "cinvtexdim") {
        kind = FixInvTex;
        return true;
    }

    if (lower == "cbufferoffset_size") {
        kind = FixBufferOffset;
        return true;
    }

    return false;
}

bool TryParseCtabAtBase(const unsigned char* base, size_t totalBytes, ShaderInfo& out)
{
    if (totalBytes < 28) {
        return false;
    }

    const DWORD constants = *reinterpret_cast<const DWORD*>(base + 12);
    const DWORD constantInfoOffset = *reinterpret_cast<const DWORD*>(base + 16);

    if (constants == 0 || constants > 512) {
        return false;
    }

    const size_t infoSize = 20;
    if (constantInfoOffset >= totalBytes || constantInfoOffset + (constants * infoSize) > totalBytes) {
        return false;
    }

    bool parsedAny = false;
    for (DWORD i = 0; i < constants; ++i) {
        const unsigned char* info = base + constantInfoOffset + (i * infoSize);
        const DWORD nameOffset = *reinterpret_cast<const DWORD*>(info + 0);
        const WORD registerSet = *reinterpret_cast<const WORD*>(info + 4);
        const WORD registerIndex = *reinterpret_cast<const WORD*>(info + 6);
        const WORD registerCount = *reinterpret_cast<const WORD*>(info + 8);

        std::string name;
        if (!ReadStringFromOffset(base, totalBytes, nameOffset, name)) {
            continue;
        }

        parsedAny = true;

        FixKind kind = FixInvTex;
        if (registerSet == 2 && registerCount > 0 && NameToFixKind(name, kind)) {
            if ((kind == FixInvTex && g_config.adjustInvTex) ||
                (kind == FixBufferOffset && g_config.adjustBufferOffset)) {
                FixRange fix;
                fix.name = name;
                fix.reg = registerIndex;
                fix.count = registerCount;
                fix.kind = kind;
                out.fixes.push_back(fix);
            }
        }
    }

    return parsedAny;
}

void ParseCtabComment(const unsigned char* data, size_t bytes, ShaderInfo& out)
{
    if (bytes < 32) {
        return;
    }

    if (bytes >= 4 && std::memcmp(data, "CTAB", 4) == 0) {
        TryParseCtabAtBase(data + 4, bytes - 4, out);
        return;
    }

    for (size_t offset = 0; offset + 32 < bytes; offset += 4) {
        if (std::memcmp(data + offset, "CTAB", 4) == 0) {
            if (TryParseCtabAtBase(data + offset + 4, bytes - offset - 4, out)) {
                return;
            }
        }
    }

    TryParseCtabAtBase(data, bytes, out);
}

ShaderInfo ParsePixelShader(const DWORD* function)
{
    ShaderInfo info;
    if (!function) {
        return info;
    }

    // Shader bytecode is a DWORD stream. Comment tokens contain the D3DX CTAB
    // constant table when the shader was not stripped.
    for (size_t i = 0; i < 262144; ++i) {
        const DWORD token = function[i];
        if (token == 0x0000FFFF) {
            break;
        }

        if ((token & 0xFFFF) == 0xFFFE) {
            const DWORD dwordCount = token >> 16;
            if (dwordCount == 0 || dwordCount > 65535) {
                continue;
            }

            const unsigned char* commentData = reinterpret_cast<const unsigned char*>(&function[i + 1]);
            ParseCtabComment(commentData, static_cast<size_t>(dwordCount) * sizeof(DWORD), info);
            i += dwordCount;
        }
    }

    return info;
}

ShaderInfo ParseLivePixelShader(IDirect3DPixelShader9* shader)
{
    ShaderInfo info;
    if (!shader) {
        return info;
    }

    UINT byteCount = 0;
    HRESULT hr = shader->GetFunction(nullptr, &byteCount);
    if (FAILED(hr) || byteCount < sizeof(DWORD) || byteCount > 1024 * 1024) {
        return info;
    }

    std::vector<DWORD> bytecode((byteCount + sizeof(DWORD) - 1) / sizeof(DWORD), 0);
    hr = shader->GetFunction(bytecode.data(), &byteCount);
    if (FAILED(hr)) {
        return info;
    }

    return ParsePixelShader(bytecode.data());
}

template <typename T>
bool ReadLiveShaderBytecode(T* shader, std::vector<unsigned char>& bytecode)
{
    if (!shader) {
        return false;
    }

    UINT byteCount = 0;
    HRESULT hr = shader->GetFunction(nullptr, &byteCount);
    if (FAILED(hr) || byteCount == 0 || byteCount > 1024 * 1024) {
        return false;
    }

    bytecode.resize(byteCount);
    hr = shader->GetFunction(bytecode.data(), &byteCount);
    if (FAILED(hr)) {
        bytecode.clear();
        return false;
    }
    bytecode.resize(byteCount);
    return true;
}

template <typename T>
DWORD HashLiveShader(T* shader)
{
    std::vector<unsigned char> bytecode;
    if (!ReadLiveShaderBytecode(shader, bytecode)) {
        return 0;
    }

    DWORD hash = 2166136261u;
    for (unsigned char byte : bytecode) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

DWORD HashLivePixelShader(IDirect3DPixelShader9* shader)
{
    return HashLiveShader(shader);
}

DWORD HashLiveVertexShader(IDirect3DVertexShader9* shader)
{
    return HashLiveShader(shader);
}

template <typename T>
void DumpCinematicShader(T* shader, const wchar_t* stage, DWORD hash, std::unordered_set<DWORD>& dumpedHashes)
{
    if (!shader || !g_config.dumpTargetShaders || hash == 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (dumpedHashes.find(hash) != dumpedHashes.end()) {
            return;
        }
        dumpedHashes.insert(hash);
    }

    std::vector<unsigned char> bytecode;
    if (!ReadLiveShaderBytecode(shader, bytecode)) {
        return;
    }

    const std::wstring dumpDir = GetModuleDirectory() + L"HotDPostFXShaders";
    CreateDirectoryW(dumpDir.c_str(), nullptr);

    wchar_t fileName[64] = {};
    swprintf_s(fileName, L"%s_%08lX.bin", stage, static_cast<unsigned long>(hash));
    const std::wstring dumpPath = dumpDir + L"\\" + fileName;

    FILE* file = nullptr;
    if (_wfopen_s(&file, dumpPath.c_str(), L"wb") == 0 && file) {
        std::fwrite(bytecode.data(), 1, bytecode.size(), file);
        std::fclose(file);
        Log("Dumped cinematic %ls shader hash=%08lX bytes=%u path=%ls",
            stage,
            static_cast<unsigned long>(hash),
            static_cast<unsigned>(bytecode.size()),
            dumpPath.c_str());
    }
}

bool ShouldScaleValue(float value)
{
    const float absValue = std::fabs(value);
    return absValue > 0.0f && absValue <= 0.05f;
}

bool IsFullscreenCandidate(D3DPRIMITIVETYPE primitiveType, UINT primitiveCount)
{
    return primitiveCount > 0 &&
        primitiveCount <= 2 &&
        (primitiveType == D3DPT_TRIANGLELIST || primitiveType == D3DPT_TRIANGLESTRIP);
}

bool IsCinematicBloomShader(DWORD shaderHash)
{
    switch (shaderHash) {
    case 0x7CA5B0FDu: // Four-tap downsample.
    case 0xD4B27AA3u: // Bloom threshold/combine.
    case 0xA65CC643u: // Diagonal blur.
    case 0x4761E642u: // Horizontal blur at 160x90.
    case 0x5CFC4683u: // Horizontal blur at 640x360.
    case 0x5C1D7192u: // Cinematic depth packing.
    case 0x0489C361u: // Cinematic DOF setup.
    case 0x2A4CAE43u: // Cinematic DOF downsample.
    case 0xAD78FD19u: // Final post-processing composite.
        return true;
    default:
        return false;
    }
}

UINT CinematicSampleOffsetCount(DWORD shaderHash)
{
    switch (shaderHash) {
    case 0x4761E642u:
        return 15; // Final HDR bloom spread only.
    case 0x5CFC4683u:
        return 7; // Separable cutscene DoF/occlusion blur.
    case 0x2A4CAE43u:
        return 5; // Cutscene blurred-scene downsample/blur.
    default:
        return 0;
    }
}

bool IsCinematicDofBlurShader(DWORD shaderHash)
{
    return shaderHash == 0x5CFC4683u || shaderHash == 0x2A4CAE43u;
}

float ActiveOutputScale(UINT width, UINT height)
{
    if (width == 0 || height == 0) {
        return 1.0f;
    }

    const float activeHeight = std::min(
        static_cast<float>(height),
        static_cast<float>(width) * (9.0f / 16.0f));
    return std::max(1.0f, activeHeight / 720.0f);
}

bool GetTexture2DSize(IDirect3DDevice9* device, DWORD stage, UINT& width, UINT& height)
{
    width = 0;
    height = 0;

    IDirect3DBaseTexture9* baseTexture = nullptr;
    if (!device || FAILED(device->GetTexture(stage, &baseTexture)) || !baseTexture) {
        return false;
    }

    bool result = false;
    if (baseTexture->GetType() == D3DRTYPE_TEXTURE) {
        IDirect3DTexture9* texture = static_cast<IDirect3DTexture9*>(baseTexture);
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(texture->GetLevelDesc(0, &desc))) {
            width = desc.Width;
            height = desc.Height;
            result = true;
        }
    }

    baseTexture->Release();
    return result;
}

bool GetRenderTargetSize(IDirect3DDevice9* device, UINT& width, UINT& height)
{
    width = 0;
    height = 0;

    IDirect3DSurface9* renderTarget = nullptr;
    if (!device || FAILED(device->GetRenderTarget(0, &renderTarget)) || !renderTarget) {
        return false;
    }

    D3DSURFACE_DESC desc = {};
    const bool result = SUCCEEDED(renderTarget->GetDesc(&desc));
    if (result) {
        width = desc.Width;
        height = desc.Height;
    }
    renderTarget->Release();
    return result;
}

void UpdateBloomOutputScaleFromComposite(IDirect3DDevice9* device)
{
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    UINT sceneWidth = 0;
    UINT sceneHeight = 0;
    if (!GetRenderTargetSize(device, outputWidth, outputHeight) ||
        !GetTexture2DSize(device, 1, sceneWidth, sceneHeight) ||
        sceneWidth == 0 || sceneHeight == 0) {
        return;
    }

    const float widthScale = static_cast<float>(outputWidth) / static_cast<float>(sceneWidth);
    const float heightScale = static_cast<float>(outputHeight) / static_cast<float>(sceneHeight);
    if (!std::isfinite(widthScale) || !std::isfinite(heightScale) ||
        widthScale < 0.75f || heightScale < 0.75f ||
        std::fabs(widthScale - heightScale) > 0.05f) {
        return;
    }

    const float outputScale = std::max(1.0f, (widthScale + heightScale) * 0.5f);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        DeviceState& state = g_deviceState[device];
        changed = std::fabs(state.bloomOutputScale - outputScale) > 0.001f ||
            state.logicalOutputWidth != outputWidth ||
            state.logicalOutputHeight != outputHeight;
        state.bloomOutputScale = outputScale;
        state.logicalOutputWidth = outputWidth;
        state.logicalOutputHeight = outputHeight;
    }

    if (changed) {
        Log("BloomScale confirmed from final composite: output=%ux%u scene=%ux%u divisor=%.6f",
            outputWidth,
            outputHeight,
            sceneWidth,
            sceneHeight,
            outputScale);
    }
}

float BloomRadiusDivisor(IDirect3DDevice9* device)
{
    if (g_config.bloomRadiusDivisor > 0.01f) {
        return std::max(1.0f, g_config.bloomRadiusDivisor);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    const auto stateIt = g_deviceState.find(device);
    if (stateIt == g_deviceState.end()) {
        return 1.0f;
    }
    return std::max(1.0f, stateIt->second.bloomOutputScale);
}

float DofRadiusDivisor(IDirect3DDevice9* device)
{
    if (g_config.cutsceneDofRadiusDivisor > 0.01f) {
        return std::max(1.0f, g_config.cutsceneDofRadiusDivisor);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    const auto stateIt = g_deviceState.find(device);
    if (stateIt == g_deviceState.end()) {
        return 1.0f;
    }
    return std::max(1.0f, stateIt->second.bloomOutputScale);
}

void LogCinematicPixelConstants(IDirect3DDevice9* device, DWORD shaderHash)
{
    int logIndex = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int& count = g_cinematicConstantLogCounts[shaderHash];
        if (count >= 4) {
            return;
        }
        logIndex = ++count;
    }

    float constants[32 * 4] = {};
    if (!device || FAILED(device->GetPixelShaderConstantF(0, constants, 32))) {
        return;
    }

    Log("CinematicPixelConstants hash=%08lX sample=%d",
        static_cast<unsigned long>(shaderHash),
        logIndex);
    for (UINT reg = 0; reg < 32; ++reg) {
        const float* c = constants + (reg * 4);
        Log("CinematicPixelConst hash=%08lX c%u %.9f %.9f %.9f %.9f",
            static_cast<unsigned long>(shaderHash),
            reg,
            c[0],
            c[1],
            c[2],
            c[3]);
    }
}

bool PatchGenericSmallVector(float* vector)
{
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        const float before = vector[i];
        if (ShouldScaleValue(vector[i])) {
            vector[i] /= g_config.scale;
        }
        changed = changed || before != vector[i];
    }
    return changed;
}

void ScaleXY(float* vector, const FixRange& fix)
{
    const float beforeX = vector[0];
    const float beforeY = vector[1];

    if (ShouldScaleValue(vector[0])) {
        vector[0] /= g_config.scale;
    }
    if (ShouldScaleValue(vector[1])) {
        vector[1] /= g_config.scale;
    }

    if (g_config.log && g_adjustmentLogs < g_config.maxAdjustmentLogs &&
        (beforeX != vector[0] || beforeY != vector[1])) {
        ++g_adjustmentLogs;
        Log("Adjusted %s r%u: xy %.9f, %.9f -> %.9f, %.9f",
            fix.name.c_str(),
            fix.reg,
            beforeX,
            beforeY,
            vector[0],
            vector[1]);
    }
}

void PatchVector(float* vector, const FixRange& fix)
{
    if (fix.kind == FixInvTex) {
        ScaleXY(vector, fix);
        return;
    }

    // cBufferOffset_Size appears in the bloom/downsample path. Depending on the
    // shader variant it can contain direct texel offsets, buffer dimensions, or
    // both, so fix both common layouts.
    for (int i = 0; i < 4; ++i) {
        if (ShouldScaleValue(vector[i])) {
            vector[i] /= g_config.scale;
        }
    }

    for (int i = 2; i < 4; ++i) {
        const float absValue = std::fabs(vector[i]);
        if (absValue >= 64.0f && absValue <= 8192.0f) {
            vector[i] *= g_config.scale;
        }
    }
}

void PatchDevice(IDirect3DDevice9* device)
{
    if (!device) {
        return;
    }

    PatchVtableSlot(device, kCreatePixelShaderIndex, reinterpret_cast<void*>(&CreatePixelShader), reinterpret_cast<void**>(&g_realCreatePixelShader));
    PatchVtableSlot(device, kSetPixelShaderIndex, reinterpret_cast<void*>(&SetPixelShader), reinterpret_cast<void**>(&g_realSetPixelShader));
    PatchVtableSlot(device, kSetPixelShaderConstantFIndex, reinterpret_cast<void*>(&SetPixelShaderConstantF), reinterpret_cast<void**>(&g_realSetPixelShaderConstantF));
    PatchVtableSlot(device, kDrawPrimitiveIndex, reinterpret_cast<void*>(&DrawPrimitive), reinterpret_cast<void**>(&g_realDrawPrimitive));
    PatchVtableSlot(device, kDrawIndexedPrimitiveIndex, reinterpret_cast<void*>(&DrawIndexedPrimitive), reinterpret_cast<void**>(&g_realDrawIndexedPrimitive));
    PatchVtableSlot(device, kDrawPrimitiveUPIndex, reinterpret_cast<void*>(&DrawPrimitiveUP), reinterpret_cast<void**>(&g_realDrawPrimitiveUP));
    PatchVtableSlot(device, kDrawIndexedPrimitiveUPIndex, reinterpret_cast<void*>(&DrawIndexedPrimitiveUP), reinterpret_cast<void**>(&g_realDrawIndexedPrimitiveUP));
}

HRESULT STDMETHODCALLTYPE CreateDevice(
    IDirect3D9* self,
    UINT adapter,
    D3DDEVTYPE deviceType,
    HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* presentationParameters,
    IDirect3DDevice9** returnedDevice)
{
    const HRESULT hr = g_realCreateDevice(
        self,
        adapter,
        deviceType,
        focusWindow,
        behaviorFlags,
        presentationParameters,
        returnedDevice);

    if (SUCCEEDED(hr) && returnedDevice && *returnedDevice) {
        if (g_config.installShaderHooks) {
            PatchDevice(*returnedDevice);
        }

        UINT backBufferWidth = presentationParameters ? presentationParameters->BackBufferWidth : 0;
        UINT backBufferHeight = presentationParameters ? presentationParameters->BackBufferHeight : 0;
        if (backBufferWidth == 0 || backBufferHeight == 0) {
            IDirect3DSurface9* backBuffer = nullptr;
            if (SUCCEEDED((*returnedDevice)->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) &&
                backBuffer) {
                D3DSURFACE_DESC desc = {};
                if (SUCCEEDED(backBuffer->GetDesc(&desc))) {
                    backBufferWidth = desc.Width;
                    backBufferHeight = desc.Height;
                }
                backBuffer->Release();
            }
        }

        const float initialBloomScale = ActiveOutputScale(backBufferWidth, backBufferHeight);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            DeviceState& state = g_deviceState[*returnedDevice];
            state.bloomOutputScale = initialBloomScale;
            state.logicalOutputWidth = backBufferWidth;
            state.logicalOutputHeight = backBufferHeight;
        }

        Log("CreateDevice hooked. BackBuffer=%ux%u Windowed=%u initialBloomDivisor=%.6f",
            backBufferWidth,
            backBufferHeight,
            presentationParameters ? presentationParameters->Windowed : 0,
            initialBloomScale);
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE CreatePixelShader(
    IDirect3DDevice9* self,
    const DWORD* function,
    IDirect3DPixelShader9** shader)
{
    const HRESULT hr = g_realCreatePixelShader(self, function, shader);

    if (SUCCEEDED(hr) && shader && *shader && g_config.enabled) {
        ShaderInfo info = ParsePixelShader(function);

        if (g_config.logShaderCreation && g_shaderCreationLogs < g_config.maxShaderCreationLogs) {
            ++g_shaderCreationLogs;
            Log("CreatePixelShader shader=%p fixes=%u",
                *shader,
                static_cast<unsigned>(info.fixes.size()));
        }

        if (!info.fixes.empty()) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_shaderInfo[*shader] = info;

            if (g_config.dumpTargetShaders) {
                for (const FixRange& fix : info.fixes) {
                    Log("Target pixel shader %p constant %s r%u count=%u kind=%u",
                        *shader,
                        fix.name.c_str(),
                        fix.reg,
                        fix.count,
                        static_cast<unsigned>(fix.kind));
                }
            }
        }
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DDevice9* self, IDirect3DPixelShader9* shader)
{
    if (g_config.enabled || g_config.disableCutsceneEffects) {
        std::lock_guard<std::mutex> lock(g_mutex);
        DeviceState& state = g_deviceState[self];
        state.currentPixelShader = shader;

        const auto infoIt = g_shaderInfo.find(shader);
        if (shader && infoIt != g_shaderInfo.end() && !infoIt->second.fixes.empty() &&
            g_targetShaderLogs < g_config.maxTargetShaderLogs) {
            ++g_targetShaderLogs;
            Log("SetPixelShader target shader=%p fixes=%u",
                shader,
                static_cast<unsigned>(infoIt->second.fixes.size()));
        }
    }

    return g_realSetPixelShader(self, shader);
}

void ShadowPixelShaderConstants(IDirect3DDevice9* self, UINT startRegister, const float* constantData, UINT vector4fCount)
{
    if (!constantData || startRegister >= kMaxPixelShaderConstants) {
        return;
    }

    const UINT count = std::min(vector4fCount, kMaxPixelShaderConstants - startRegister);
    std::lock_guard<std::mutex> lock(g_mutex);
    DeviceState& state = g_deviceState[self];
    for (UINT i = 0; i < count; ++i) {
        state.psConstantValid[startRegister + i] = true;
        std::memcpy(state.psConstants[startRegister + i], constantData + (static_cast<size_t>(i) * 4), sizeof(float) * 4);
    }
}

HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(
    IDirect3DDevice9* self,
    UINT startRegister,
    const float* constantData,
    UINT vector4fCount)
{
    if (!g_config.enabled || !constantData || vector4fCount == 0) {
        if (!g_internalConstantUpdate) {
            ShadowPixelShaderConstants(self, startRegister, constantData, vector4fCount);
        }
        return g_realSetPixelShaderConstantF(self, startRegister, constantData, vector4fCount);
    }

    if (!g_internalConstantUpdate) {
        ShadowPixelShaderConstants(self, startRegister, constantData, vector4fCount);
    }

    ShaderInfo info;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto currentIt = g_deviceState.find(self);
        if (currentIt == g_deviceState.end() || !currentIt->second.currentPixelShader) {
            return g_realSetPixelShaderConstantF(self, startRegister, constantData, vector4fCount);
        }

        const auto infoIt = g_shaderInfo.find(currentIt->second.currentPixelShader);
        if (infoIt == g_shaderInfo.end() || infoIt->second.fixes.empty()) {
            return g_realSetPixelShaderConstantF(self, startRegister, constantData, vector4fCount);
        }

        info = infoIt->second;
    }

    const UINT endRegister = startRegister + vector4fCount;
    std::vector<float> patched(constantData, constantData + (static_cast<size_t>(vector4fCount) * 4));
    bool changed = false;

    for (const FixRange& fix : info.fixes) {
        const UINT fixStart = fix.reg;
        const UINT fixEnd = fix.reg + fix.count;
        if (fixEnd <= startRegister || fixStart >= endRegister) {
            continue;
        }

        const UINT intersectStart = std::max(fixStart, startRegister);
        const UINT intersectEnd = std::min(fixEnd, endRegister);

        for (UINT reg = intersectStart; reg < intersectEnd; ++reg) {
            float* vector = &patched[(static_cast<size_t>(reg - startRegister) * 4)];
            const float beforeX = vector[0];
            const float beforeY = vector[1];
            PatchVector(vector, fix);
            changed = changed || beforeX != vector[0] || beforeY != vector[1];
        }
    }

    return g_realSetPixelShaderConstantF(
        self,
        startRegister,
        changed ? patched.data() : constantData,
        vector4fCount);
}

void TextureSizeString(IDirect3DDevice9* self, DWORD stage, char* out, size_t outSize)
{
    if (!out || outSize == 0) {
        return;
    }
    out[0] = '\0';

    IDirect3DBaseTexture9* baseTexture = nullptr;
    if (!self || FAILED(self->GetTexture(stage, &baseTexture)) || !baseTexture) {
        std::snprintf(out, outSize, "s%lu=none", static_cast<unsigned long>(stage));
        return;
    }

    D3DRESOURCETYPE type = baseTexture->GetType();
    if (type == D3DRTYPE_TEXTURE) {
        IDirect3DTexture9* texture = static_cast<IDirect3DTexture9*>(baseTexture);
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(texture->GetLevelDesc(0, &desc))) {
            std::snprintf(out, outSize, "s%lu=%ux%u", static_cast<unsigned long>(stage), desc.Width, desc.Height);
        } else {
            std::snprintf(out, outSize, "s%lu=tex?", static_cast<unsigned long>(stage));
        }
    } else {
        std::snprintf(out, outSize, "s%lu=type%u", static_cast<unsigned long>(stage), static_cast<unsigned>(type));
    }

    baseTexture->Release();
}

void LogFullscreenDrawCandidate(
    IDirect3DDevice9* self,
    const char* drawName,
    D3DPRIMITIVETYPE primitiveType,
    UINT primitiveCount,
    IDirect3DPixelShader9* shader,
    const void* vertexData,
    UINT vertexStride)
{
    if (!g_config.logFullscreenDraws || g_fullscreenDrawLogs >= g_config.maxFullscreenDrawLogs) {
        return;
    }

    D3DVIEWPORT9 viewport = {};
    if (self) {
        self->GetViewport(&viewport);
    }

    UINT rtWidth = 0;
    UINT rtHeight = 0;
    IDirect3DSurface9* renderTarget = nullptr;
    if (self && SUCCEEDED(self->GetRenderTarget(0, &renderTarget)) && renderTarget) {
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(renderTarget->GetDesc(&desc))) {
            rtWidth = desc.Width;
            rtHeight = desc.Height;
        }
        renderTarget->Release();
    }

    char tex0[32] = {};
    char tex1[32] = {};
    char tex2[32] = {};
    char tex3[32] = {};
    char tex4[32] = {};
    char tex5[32] = {};
    char tex6[32] = {};
    TextureSizeString(self, 0, tex0, sizeof(tex0));
    TextureSizeString(self, 1, tex1, sizeof(tex1));
    TextureSizeString(self, 2, tex2, sizeof(tex2));
    TextureSizeString(self, 3, tex3, sizeof(tex3));
    TextureSizeString(self, 4, tex4, sizeof(tex4));
    TextureSizeString(self, 5, tex5, sizeof(tex5));
    TextureSizeString(self, 6, tex6, sizeof(tex6));

    float constants[4 * 4] = {};
    bool constantsOk = self && SUCCEEDED(self->GetPixelShaderConstantF(0, constants, 4));

    const DWORD shaderHash = HashLivePixelShader(shader);
    char signature[768] = {};
    std::snprintf(signature, sizeof(signature), "%s|%u|%u|%08lX|%ux%u|%ux%u|%s|%s|%s|%s|%s|%s|%s|%u",
        drawName,
        static_cast<unsigned>(primitiveType),
        primitiveCount,
        static_cast<unsigned long>(shaderHash),
        rtWidth,
        rtHeight,
        viewport.Width,
        viewport.Height,
        tex0,
        tex1,
        tex2,
        tex3,
        tex4,
        tex5,
        tex6,
        vertexStride);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_loggedFullscreenSignatures.find(signature) != g_loggedFullscreenSignatures.end()) {
            return;
        }
        g_loggedFullscreenSignatures.insert(signature);
    }

    ++g_fullscreenDrawLogs;

    Log("FullscreenDraw %s primType=%u prims=%u stride=%u shader=%p hash=%08lX rt=%ux%u vp=%u,%u %ux%u tex=%s %s %s %s %s %s %s",
        drawName,
        static_cast<unsigned>(primitiveType),
        primitiveCount,
        vertexStride,
        shader,
        static_cast<unsigned long>(shaderHash),
        rtWidth,
        rtHeight,
        viewport.X,
        viewport.Y,
        viewport.Width,
        viewport.Height,
        tex0,
        tex1,
        tex2,
        tex3,
        tex4,
        tex5,
        tex6);

    if (IsCinematicBloomShader(shaderHash)) {
        DumpCinematicShader(shader, L"ps", shaderHash, g_dumpedCinematicPixelShaders);

        IDirect3DVertexShader9* vertexShader = nullptr;
        if (self && SUCCEEDED(self->GetVertexShader(&vertexShader)) && vertexShader) {
            const DWORD vertexHash = HashLiveVertexShader(vertexShader);
            DumpCinematicShader(vertexShader, L"vs", vertexHash, g_dumpedCinematicVertexShaders);

            DWORD fvf = 0;
            self->GetFVF(&fvf);
            Log("CinematicVertexState ps=%08lX vs=%08lX fvf=%08lX",
                static_cast<unsigned long>(shaderHash),
                static_cast<unsigned long>(vertexHash),
                static_cast<unsigned long>(fvf));

            float vertexConstants[16 * 4] = {};
            if (SUCCEEDED(self->GetVertexShaderConstantF(0, vertexConstants, 16))) {
                for (UINT reg = 0; reg < 16; ++reg) {
                    const float* c = vertexConstants + (reg * 4);
                    Log("CinematicVertexConst c%u %.9f %.9f %.9f %.9f",
                        reg, c[0], c[1], c[2], c[3]);
                }
            }
            vertexShader->Release();
        } else {
            Log("CinematicVertexState ps=%08lX vs=fixed-function",
                static_cast<unsigned long>(shaderHash));
        }
    }

    if (constantsOk) {
        for (UINT reg = 0; reg < 4; ++reg) {
            const float* c = constants + (reg * 4);
            Log("FullscreenDrawConst c%u %.9f %.9f %.9f %.9f", reg, c[0], c[1], c[2], c[3]);
        }
    }

    if (vertexData && vertexStride >= sizeof(float) * 4) {
        UINT vertexCount = 0;
        if (primitiveType == D3DPT_TRIANGLESTRIP) {
            vertexCount = primitiveCount + 2;
        } else if (primitiveType == D3DPT_TRIANGLELIST) {
            vertexCount = primitiveCount * 3;
        }
        vertexCount = std::min<UINT>(vertexCount, 4);

        for (UINT vertex = 0; vertex < vertexCount; ++vertex) {
            const unsigned char* bytes = static_cast<const unsigned char*>(vertexData) + (static_cast<size_t>(vertex) * vertexStride);
            float values[12] = {};
            const UINT floatCount = std::min<UINT>(12, vertexStride / sizeof(float));
            for (UINT i = 0; i < floatCount; ++i) {
                std::memcpy(&values[i], bytes + (static_cast<size_t>(i) * sizeof(float)), sizeof(float));
            }
            Log("FullscreenDrawVertex v%u floats %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f",
                vertex,
                values[0], values[1], values[2], values[3],
                values[4], values[5], values[6], values[7],
                values[8], values[9], values[10], values[11]);
        }
    }
}

std::vector<DrawPatch> BuildDrawPatches(
    IDirect3DDevice9* self,
    const char* drawName,
    D3DPRIMITIVETYPE primitiveType,
    UINT primitiveCount,
    const void* vertexData = nullptr,
    UINT vertexStride = 0,
    DWORD* shaderHash = nullptr,
    bool* cutscenePass = nullptr)
{
    std::vector<DrawPatch> patches;
    if (shaderHash) {
        *shaderHash = 0;
    }
    if (cutscenePass) {
        *cutscenePass = false;
    }
    if ((!g_config.enabled && !g_config.disableCutsceneEffects) || !g_config.patchOnDraw) {
        return patches;
    }

    IDirect3DPixelShader9* queriedShader = nullptr;
    IDirect3DPixelShader9* currentShader = nullptr;
    if (self && SUCCEEDED(self->GetPixelShader(&queriedShader)) && queriedShader) {
        currentShader = queriedShader;
    } else {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto stateIt = g_deviceState.find(self);
        if (stateIt != g_deviceState.end()) {
            currentShader = stateIt->second.currentPixelShader;
        }
    }

    if (!currentShader) {
        return patches;
    }

    const bool fullscreenCandidate = IsFullscreenCandidate(primitiveType, primitiveCount);
    if (fullscreenCandidate) {
        LogFullscreenDrawCandidate(self, drawName, primitiveType, primitiveCount, currentShader, vertexData, vertexStride);
    }
    const DWORD currentShaderHash =
        fullscreenCandidate && g_config.patchFullscreenSmallConstants
        ? HashLivePixelShader(currentShader)
        : 0;
    if (shaderHash) {
        *shaderHash = currentShaderHash;
    }

    bool isCutscenePass = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        DeviceState& state = g_deviceState[self];
        if (currentShaderHash == 0x5C1D7192u) {
            state.cutscenePipelineActive = true;
        }
        isCutscenePass = state.cutscenePipelineActive;
        if (currentShaderHash == 0xAD78FD19u) {
            state.cutscenePipelineActive = false;
        }
    }
    if (cutscenePass) {
        *cutscenePass = isCutscenePass;
    }

    if (currentShaderHash == 0xAD78FD19u && isCutscenePass) {
        UpdateBloomOutputScaleFromComposite(self);
    }
    if (IsCinematicBloomShader(currentShaderHash)) {
        LogCinematicPixelConstants(self, currentShaderHash);
    }

    auto addSingleRegisterPatch = [&](UINT reg, const char* name, const float* replacement) {
        DrawPatch patch;
        patch.reg = reg;
        patch.count = 1;
        patch.name = name;
        patch.original.resize(4);
        patch.patched.assign(replacement, replacement + 4);
        if (FAILED(self->GetPixelShaderConstantF(reg, patch.original.data(), 1))) {
            return;
        }
        if (patch.original != patch.patched) {
            patches.push_back(patch);
        }
    };

    if (currentShaderHash == 0xAD78FD19u && isCutscenePass) {
        float value[4] = {};
        if (g_config.enabled && std::fabs(g_config.cutsceneBloomIntensity - 1.0f) > 0.0001f) {
            if (SUCCEEDED(self->GetPixelShaderConstantF(2, value, 1))) {
                value[0] *= g_config.cutsceneBloomIntensity;
                addSingleRegisterPatch(2, "cutscene-bloom-intensity", value);
            }
        }
    }

    if (!g_config.enabled) {
        if (queriedShader) {
            queriedShader->Release();
        }
        return patches;
    }

    const UINT sampleOffsetCount = CinematicSampleOffsetCount(currentShaderHash);

    ShaderInfo info;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto infoIt = g_shaderInfo.find(currentShader);
        if (infoIt != g_shaderInfo.end()) {
            info = infoIt->second;
        }
    }

    if (info.fixes.empty()) {
        ShaderInfo liveInfo = ParseLivePixelShader(currentShader);
        if (!liveInfo.fixes.empty()) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_shaderInfo[currentShader] = liveInfo;
            info = liveInfo;

            if (g_liveShaderIdentifyLogs < g_config.maxTargetShaderLogs) {
                ++g_liveShaderIdentifyLogs;
                for (const FixRange& fix : info.fixes) {
                    Log("Live draw shader %p constant %s r%u count=%u kind=%u",
                        currentShader,
                        fix.name.c_str(),
                        fix.reg,
                        fix.count,
                        static_cast<unsigned>(fix.kind));
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        DeviceState& state = g_deviceState[self];
        if (state.currentPixelShader != currentShader &&
            !info.fixes.empty() &&
            g_targetShaderLogs < g_config.maxTargetShaderLogs) {
            ++g_targetShaderLogs;
            Log("GetPixelShader target shader=%p fixes=%u",
                currentShader,
                static_cast<unsigned>(info.fixes.size()));
        }
        state.currentPixelShader = currentShader;
    }

    if (queriedShader) {
        queriedShader->Release();
    }

    for (const FixRange& fix : info.fixes) {
        if (fix.reg >= kMaxPixelShaderConstants) {
            continue;
        }

        const UINT count = std::min(fix.count, kMaxPixelShaderConstants - fix.reg);
        if (count == 0) {
            continue;
        }

        DrawPatch patch;
        patch.reg = fix.reg;
        patch.count = count;
        patch.name = fix.name;
        patch.original.resize(static_cast<size_t>(count) * 4);
        patch.patched.resize(static_cast<size_t>(count) * 4);

        HRESULT readHr = self->GetPixelShaderConstantF(fix.reg, patch.original.data(), count);
        if (FAILED(readHr)) {
            bool allValid = true;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto stateIt = g_deviceState.find(self);
                if (stateIt == g_deviceState.end()) {
                    allValid = false;
                } else {
                    const DeviceState& state = stateIt->second;
                    for (UINT i = 0; i < count; ++i) {
                        if (!state.psConstantValid[fix.reg + i]) {
                            allValid = false;
                            break;
                        }
                        std::memcpy(
                            &patch.original[static_cast<size_t>(i) * 4],
                            state.psConstants[fix.reg + i],
                            sizeof(float) * 4);
                    }
                }
            }

            if (!allValid) {
                continue;
            }
        }

        std::memcpy(patch.patched.data(), patch.original.data(), patch.original.size() * sizeof(float));

        for (UINT i = 0; i < count; ++i) {
            float* patched = &patch.patched[static_cast<size_t>(i) * 4];
            PatchVector(patched, fix);
        }

        patches.push_back(patch);
    }

    if (sampleOffsetCount > 0) {
        const bool dofBlurShader = IsCinematicDofBlurShader(currentShaderHash);
        const float radiusDivisor =
            dofBlurShader
            ? (isCutscenePass ? DofRadiusDivisor(self) : 1.0f)
            : BloomRadiusDivisor(self);
        std::vector<float> original(static_cast<size_t>(sampleOffsetCount) * 4);
        if (self && SUCCEEDED(self->GetPixelShaderConstantF(0, original.data(), sampleOffsetCount))) {
            const bool horizontalPass =
                sampleOffsetCount > 1 &&
                std::fabs(original[4]) > std::fabs(original[5]);
            UINT bloomWidth = 0;
            UINT bloomHeight = 0;
            GetTexture2DSize(self, 0, bloomWidth, bloomHeight);
            const float horizontalBias =
                !dofBlurShader && isCutscenePass && horizontalPass && bloomWidth > 0
                ? g_config.cutsceneBloomOffsetXTexels / static_cast<float>(bloomWidth)
                : 0.0f;

            for (UINT reg = 0; reg < sampleOffsetCount; ++reg) {
                bool alreadyPatched = false;
                for (const DrawPatch& existing : patches) {
                    if (reg >= existing.reg && reg < existing.reg + existing.count) {
                        alreadyPatched = true;
                        break;
                    }
                }
                if (alreadyPatched) {
                    continue;
                }

                const float* source = original.data() + (static_cast<size_t>(reg) * 4);
                float patched[4] = {source[0], source[1], source[2], source[3]};
                bool changed = false;
                if (radiusDivisor > 1.0001f) {
                    for (UINT component = 0; component < 2; ++component) {
                        if (std::isfinite(patched[component]) && patched[component] != 0.0f) {
                            patched[component] /= radiusDivisor;
                            changed = true;
                        }
                    }
                }
                if (horizontalBias != 0.0f && std::isfinite(patched[0])) {
                    patched[0] += horizontalBias;
                    changed = true;
                }
                if (!changed) {
                    continue;
                }

                DrawPatch patch;
                patch.reg = reg;
                patch.count = 1;
                char patchName[80] = {};
                std::snprintf(
                    patchName,
                    sizeof(patchName),
                    "%s-%08lX-div-%.3f-bias-%.6f",
                    dofBlurShader ? "cutscene-dof-radius" : "bloom-align",
                    static_cast<unsigned long>(currentShaderHash),
                    radiusDivisor,
                    horizontalBias);
                patch.name = patchName;
                patch.original.assign(source, source + 4);
                patch.patched.assign(patched, patched + 4);
                patches.push_back(patch);
            }
        }
    }

    return patches;
}

void ApplyDrawPatches(IDirect3DDevice9* self, const std::vector<DrawPatch>& patches)
{
    if (!g_realSetPixelShaderConstantF) {
        return;
    }

    g_internalConstantUpdate = true;
    for (const DrawPatch& patch : patches) {
        if (g_drawPatchLogs < g_config.maxDrawPatchLogs) {
            ++g_drawPatchLogs;
            Log("DrawPatch %s r%u count=%u xy %.9f, %.9f -> %.9f, %.9f",
                patch.name.c_str(),
                patch.reg,
                patch.count,
                patch.original.empty() ? 0.0f : patch.original[0],
                patch.original.size() < 2 ? 0.0f : patch.original[1],
                patch.patched.empty() ? 0.0f : patch.patched[0],
                patch.patched.size() < 2 ? 0.0f : patch.patched[1]);
            if (patch.original.size() >= 4 && patch.patched.size() >= 4) {
                Log("DrawPatchDetail %s r%u xyzw %.9f, %.9f, %.9f, %.9f -> %.9f, %.9f, %.9f, %.9f",
                    patch.name.c_str(),
                    patch.reg,
                    patch.original[0],
                    patch.original[1],
                    patch.original[2],
                    patch.original[3],
                    patch.patched[0],
                    patch.patched[1],
                    patch.patched[2],
                    patch.patched[3]);
            }
        }
        g_realSetPixelShaderConstantF(self, patch.reg, patch.patched.data(), patch.count);
    }
    g_internalConstantUpdate = false;
}

void RestoreDrawPatches(IDirect3DDevice9* self, const std::vector<DrawPatch>& patches)
{
    if (!g_realSetPixelShaderConstantF) {
        return;
    }

    g_internalConstantUpdate = true;
    for (const DrawPatch& patch : patches) {
        g_realSetPixelShaderConstantF(self, patch.reg, patch.original.data(), patch.count);
    }
    g_internalConstantUpdate = false;
}

IDirect3DPixelShader9* GetCorrectedDofCompositeShader(IDirect3DDevice9* self)
{
    if (!self || !g_realCreatePixelShader) {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto stateIt = g_deviceState.find(self);
        if (stateIt != g_deviceState.end() && stateIt->second.correctedDofCompositeShader) {
            return stateIt->second.correctedDofCompositeShader;
        }
    }

    HRSRC resource = FindResourceW(
        g_module,
        MAKEINTRESOURCEW(kCutsceneDofCompositeResource),
        MAKEINTRESOURCEW(10));
    HGLOBAL loaded = resource ? LoadResource(g_module, resource) : nullptr;
    const DWORD resourceSize = resource ? SizeofResource(g_module, resource) : 0;
    const void* resourceData = loaded ? LockResource(loaded) : nullptr;
    if (!resourceData || resourceSize < sizeof(DWORD) * 2) {
        if (!g_cutsceneDofShaderResourceErrorLogged) {
            g_cutsceneDofShaderResourceErrorLogged = true;
            Log("ERROR: corrected cutscene DoF composite shader resource is missing");
        }
        return nullptr;
    }

    IDirect3DPixelShader9* created = nullptr;
    const HRESULT hr = g_realCreatePixelShader(
        self,
        static_cast<const DWORD*>(resourceData),
        &created);
    if (FAILED(hr) || !created) {
        if (!g_cutsceneDofShaderResourceErrorLogged) {
            g_cutsceneDofShaderResourceErrorLogged = true;
            Log("ERROR: corrected cutscene DoF composite shader creation failed hr=%08lX size=%lu",
                static_cast<unsigned long>(hr),
                static_cast<unsigned long>(resourceSize));
        }
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    DeviceState& state = g_deviceState[self];
    if (!state.correctedDofCompositeShader) {
        state.correctedDofCompositeShader = created;
        Log("Created corrected cutscene DoF composite shader bytes=%lu",
            static_cast<unsigned long>(resourceSize));
    } else {
        created->Release();
    }
    return state.correctedDofCompositeShader;
}

struct CutsceneDofFixOverride {
    IDirect3DPixelShader9* originalShader = nullptr;
    float originalConstants[8] = {};
    bool constantsCaptured = false;
    bool applied = false;
};

CutsceneDofFixOverride ApplyCutsceneDofFixOverride(
    IDirect3DDevice9* self,
    DWORD shaderHash,
    bool cutscenePass)
{
    CutsceneDofFixOverride result;
    if (!self || !g_config.enabled || g_config.disableCutsceneEffects ||
        !cutscenePass || shaderHash != 0xAD78FD19u ||
        !g_realSetPixelShader || !g_realSetPixelShaderConstantF) {
        return result;
    }

    UINT blurWidth = 0;
    UINT blurHeight = 0;
    UINT maskWidth = 0;
    UINT maskHeight = 0;
    if (!GetTexture2DSize(self, 5, blurWidth, blurHeight) ||
        !GetTexture2DSize(self, 0, maskWidth, maskHeight) ||
        blurWidth == 0 || blurHeight == 0 || maskWidth == 0 || maskHeight == 0) {
        return result;
    }

    IDirect3DPixelShader9* corrected = GetCorrectedDofCompositeShader(self);
    if (!corrected ||
        FAILED(self->GetPixelShader(&result.originalShader)) ||
        !result.originalShader) {
        return result;
    }

    result.constantsCaptured =
        SUCCEEDED(self->GetPixelShaderConstantF(14, result.originalConstants, 2));

    const float constants[8] = {
        g_config.cutsceneDofBlurOffsetXTexels / static_cast<float>(blurWidth),
        g_config.cutsceneDofBlurOffsetYTexels / static_cast<float>(blurHeight),
        g_config.cutsceneDofStrength,
        0.0f,
        g_config.cutsceneDofMaskOffsetXTexels / static_cast<float>(maskWidth),
        g_config.cutsceneDofMaskOffsetYTexels / static_cast<float>(maskHeight),
        0.0f,
        0.0f
    };

    g_internalConstantUpdate = true;
    const HRESULT shaderHr = g_realSetPixelShader(self, corrected);
    const HRESULT constantHr = SUCCEEDED(shaderHr)
        ? g_realSetPixelShaderConstantF(self, 14, constants, 2)
        : shaderHr;
    g_internalConstantUpdate = false;
    if (FAILED(shaderHr) || FAILED(constantHr)) {
        if (SUCCEEDED(shaderHr)) {
            g_realSetPixelShader(self, result.originalShader);
        }
        result.originalShader->Release();
        result.originalShader = nullptr;
        return result;
    }

    result.applied = true;
    if (g_cutsceneDofFixLogs < 40) {
        ++g_cutsceneDofFixLogs;
        Log("Cutscene DoF fix applied: blur=%ux%u offset=(%.6f,%.6f) mask=%ux%u offset=(%.6f,%.6f) strength=%.3f radiusDivisor=%.3f",
            blurWidth,
            blurHeight,
            constants[0],
            constants[1],
            maskWidth,
            maskHeight,
            constants[4],
            constants[5],
            constants[2],
            DofRadiusDivisor(self));
    }
    return result;
}

void RestoreCutsceneDofFixOverride(
    IDirect3DDevice9* self,
    CutsceneDofFixOverride& state)
{
    if (!state.applied) {
        return;
    }

    g_internalConstantUpdate = true;
    g_realSetPixelShader(self, state.originalShader);
    if (state.constantsCaptured) {
        g_realSetPixelShaderConstantF(self, 14, state.originalConstants, 2);
    }
    g_internalConstantUpdate = false;
    state.originalShader->Release();
    state.originalShader = nullptr;
    state.applied = false;
}

struct CutsceneDofOverride {
    IDirect3DBaseTexture9* originalBlur = nullptr;
    bool applied = false;
};

CutsceneDofOverride ApplyCutsceneDofOverride(
    IDirect3DDevice9* self,
    DWORD shaderHash,
    bool cutscenePass)
{
    CutsceneDofOverride result;
    if (!self || !g_config.disableCutsceneEffects || !cutscenePass || shaderHash != 0xAD78FD19u) {
        return result;
    }

    IDirect3DBaseTexture9* sharpScene = nullptr;
    if (FAILED(self->GetTexture(1, &sharpScene)) || !sharpScene) {
        return result;
    }

    if (SUCCEEDED(self->GetTexture(5, &result.originalBlur)) &&
        SUCCEEDED(self->SetTexture(5, sharpScene))) {
        result.applied = true;
        if (g_cutsceneDofBypassLogs < 20) {
            ++g_cutsceneDofBypassLogs;
            Log("Cutscene DoF bypass applied: texBlur(s5) replaced with texScene(s1)");
        }
    }
    sharpScene->Release();
    return result;
}

void RestoreCutsceneDofOverride(IDirect3DDevice9* self, CutsceneDofOverride& state)
{
    if (state.applied) {
        self->SetTexture(5, state.originalBlur);
    }
    if (state.originalBlur) {
        state.originalBlur->Release();
        state.originalBlur = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE DrawPrimitive(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    UINT startVertex,
    UINT primitiveCount)
{
    DWORD shaderHash = 0;
    bool cutscenePass = false;
    const std::vector<DrawPatch> patches =
        BuildDrawPatches(
            self, "DrawPrimitive", primitiveType, primitiveCount, nullptr, 0, &shaderHash, &cutscenePass);
    ApplyDrawPatches(self, patches);
    CutsceneDofFixOverride dofFix =
        ApplyCutsceneDofFixOverride(self, shaderHash, cutscenePass);
    CutsceneDofOverride dofOverride =
        ApplyCutsceneDofOverride(self, shaderHash, cutscenePass);
    const HRESULT hr = g_realDrawPrimitive(self, primitiveType, startVertex, primitiveCount);
    RestoreCutsceneDofOverride(self, dofOverride);
    RestoreCutsceneDofFixOverride(self, dofFix);
    RestoreDrawPatches(self, patches);
    return hr;
}

HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    INT baseVertexIndex,
    UINT minVertexIndex,
    UINT numVertices,
    UINT startIndex,
    UINT primitiveCount)
{
    DWORD shaderHash = 0;
    bool cutscenePass = false;
    const std::vector<DrawPatch> patches =
        BuildDrawPatches(
            self, "DrawIndexedPrimitive", primitiveType, primitiveCount, nullptr, 0, &shaderHash, &cutscenePass);
    ApplyDrawPatches(self, patches);
    CutsceneDofFixOverride dofFix =
        ApplyCutsceneDofFixOverride(self, shaderHash, cutscenePass);
    CutsceneDofOverride dofOverride =
        ApplyCutsceneDofOverride(self, shaderHash, cutscenePass);
    const HRESULT hr = g_realDrawIndexedPrimitive(
        self,
        primitiveType,
        baseVertexIndex,
        minVertexIndex,
        numVertices,
        startIndex,
        primitiveCount);
    RestoreCutsceneDofOverride(self, dofOverride);
    RestoreCutsceneDofFixOverride(self, dofFix);
    RestoreDrawPatches(self, patches);
    return hr;
}

HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    UINT primitiveCount,
    const void* vertexStreamZeroData,
    UINT vertexStreamZeroStride)
{
    DWORD shaderHash = 0;
    bool cutscenePass = false;
    const std::vector<DrawPatch> patches = BuildDrawPatches(
        self,
        "DrawPrimitiveUP",
        primitiveType,
        primitiveCount,
        vertexStreamZeroData,
        vertexStreamZeroStride,
        &shaderHash,
        &cutscenePass);
    ApplyDrawPatches(self, patches);
    CutsceneDofFixOverride dofFix =
        ApplyCutsceneDofFixOverride(self, shaderHash, cutscenePass);
    CutsceneDofOverride dofOverride =
        ApplyCutsceneDofOverride(self, shaderHash, cutscenePass);
    const HRESULT hr = g_realDrawPrimitiveUP(
        self,
        primitiveType,
        primitiveCount,
        vertexStreamZeroData,
        vertexStreamZeroStride);
    RestoreCutsceneDofOverride(self, dofOverride);
    RestoreCutsceneDofFixOverride(self, dofFix);
    RestoreDrawPatches(self, patches);
    return hr;
}

HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    UINT minVertexIndex,
    UINT numVertices,
    UINT primitiveCount,
    const void* indexData,
    D3DFORMAT indexDataFormat,
    const void* vertexStreamZeroData,
    UINT vertexStreamZeroStride)
{
    DWORD shaderHash = 0;
    bool cutscenePass = false;
    const std::vector<DrawPatch> patches = BuildDrawPatches(
        self,
        "DrawIndexedPrimitiveUP",
        primitiveType,
        primitiveCount,
        vertexStreamZeroData,
        vertexStreamZeroStride,
        &shaderHash,
        &cutscenePass);
    ApplyDrawPatches(self, patches);
    CutsceneDofFixOverride dofFix =
        ApplyCutsceneDofFixOverride(self, shaderHash, cutscenePass);
    CutsceneDofOverride dofOverride =
        ApplyCutsceneDofOverride(self, shaderHash, cutscenePass);
    const HRESULT hr = g_realDrawIndexedPrimitiveUP(
        self,
        primitiveType,
        minVertexIndex,
        numVertices,
        primitiveCount,
        indexData,
        indexDataFormat,
        vertexStreamZeroData,
        vertexStreamZeroStride);
    RestoreCutsceneDofOverride(self, dofOverride);
    RestoreCutsceneDofFixOverride(self, dofFix);
    RestoreDrawPatches(self, patches);
    return hr;
}

void PatchD3D9(IDirect3D9* d3d9)
{
    if (!d3d9) {
        return;
    }

    PatchVtableSlot(d3d9, kCreateDeviceIndex, reinterpret_cast<void*>(&CreateDevice), reinterpret_cast<void**>(&g_realCreateDevice));
}

} // namespace

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    auto real = reinterpret_cast<Direct3DCreate9Proc>(RealProc("Direct3DCreate9"));
    if (!real) {
        return nullptr;
    }

    IDirect3D9* d3d9 = real(sdkVersion);
    PatchD3D9(d3d9);
    return d3d9;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** d3d9Ex)
{
    auto real = reinterpret_cast<Direct3DCreate9ExProc>(RealProc("Direct3DCreate9Ex"));
    if (!real) {
        return E_FAIL;
    }

    const HRESULT hr = real(sdkVersion, d3d9Ex);
    if (SUCCEEDED(hr) && d3d9Ex && *d3d9Ex) {
        PatchD3D9(*d3d9Ex);
    }
    return hr;
}

extern "C" void* WINAPI Direct3DShaderValidatorCreate9()
{
    using Proc = void* (WINAPI*)();
    auto real = reinterpret_cast<Proc>(RealProc("Direct3DShaderValidatorCreate9"));
    return real ? real() : nullptr;
}

extern "C" void WINAPI DebugSetMute()
{
    using Proc = void (WINAPI*)();
    auto real = reinterpret_cast<Proc>(RealProc("DebugSetMute"));
    if (real) {
        real();
    }
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name)
{
    using Proc = int (WINAPI*)(D3DCOLOR, LPCWSTR);
    auto real = reinterpret_cast<Proc>(RealProc("D3DPERF_BeginEvent"));
    return real ? real(color, name) : 0;
}

extern "C" int WINAPI D3DPERF_EndEvent()
{
    using Proc = int (WINAPI*)();
    auto real = reinterpret_cast<Proc>(RealProc("D3DPERF_EndEvent"));
    return real ? real() : 0;
}

extern "C" DWORD WINAPI D3DPERF_GetStatus()
{
    using Proc = DWORD (WINAPI*)();
    auto real = reinterpret_cast<Proc>(RealProc("D3DPERF_GetStatus"));
    return real ? real() : 0;
}

extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame()
{
    using Proc = BOOL (WINAPI*)();
    auto real = reinterpret_cast<Proc>(RealProc("D3DPERF_QueryRepeatFrame"));
    return real ? real() : FALSE;
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name)
{
    using Proc = void (WINAPI*)(D3DCOLOR, LPCWSTR);
    auto real = reinterpret_cast<Proc>(RealProc("D3DPERF_SetMarker"));
    if (real) {
        real(color, name);
    }
}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD options)
{
    using Proc = void (WINAPI*)(DWORD);
    auto real = reinterpret_cast<Proc>(RealProc("D3DPERF_SetOptions"));
    if (real) {
        real(options);
    }
}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name)
{
    using Proc = void (WINAPI*)(D3DCOLOR, LPCWSTR);
    auto real = reinterpret_cast<Proc>(RealProc("D3DPERF_SetRegion"));
    if (real) {
        real(color, name);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
