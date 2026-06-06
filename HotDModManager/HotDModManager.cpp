#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr int IDC_RESOLUTION = 101;
constexpr int IDC_CROSSHAIR = 102;
constexpr int IDC_POSTFX = 103;
constexpr int IDC_CUTSCENE_DOF = 104;
constexpr int IDC_APPLY = 105;
constexpr int IDC_REFRESH = 106;
constexpr int IDC_OPEN_LOG = 107;
constexpr int IDC_STATUS = 108;

constexpr int kPostFxShimResource = 4000;
constexpr int kDgVoodooD3D9Resource = 4001;

struct AssetEntry {
    const wchar_t* fileName;
};

const AssetEntry kCrosshairAssets[] = {
    {L"l1_plantation_house_hud_permasector.pc"},
    {L"l2_hospital_hud_permasector.pc"},
    {L"l3_carnival_hud_permasector.pc"},
    {L"l4_train_hud_permasector.pc"},
    {L"l5_swamps_hud_permasector.pc"},
    {L"l6_prison_hud_permasector.pc"},
    {L"l7_labs_hud_permasector.pc"},
    {L"l7_labs_world.pc"},
    {L"l8_strip_club_hud_permasector.pc"},
    {L"l9_factory_hud_permasector.pc"},
    {L"w_civilians.pc"},
    {L"w_database_world.pc"},
    {L"w_shootinggallery.pc"},
    {L"w_survivalmode_hud_permasector.pc"},
};

HWND g_status = nullptr;
HWND g_mainWindow = nullptr;
HWND g_resolutionCombo = nullptr;
HWND g_crosshairCombo = nullptr;
HWND g_postFxCombo = nullptr;
HWND g_cutsceneDofCombo = nullptr;
HWND g_applyButton = nullptr;
HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_subtitleFont = nullptr;
HBRUSH g_backgroundBrush = nullptr;
HBRUSH g_cardBrush = nullptr;
std::wstring g_gameDir;
bool g_loadingUi = false;
bool g_dirty = false;

struct Settings {
    int resolutionScale = 0;
    bool crosshairVisible = true;
    bool postFxEnabled = true;
    bool cutsceneDofEnabled = true;
};

Settings g_appliedSettings;

std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path);
    const size_t slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        result.resize(slash);
    }
    return result;
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
{
    if (left.empty()) {
        return right;
    }
    if (left.back() == L'\\' || left.back() == L'/') {
        return left + right;
    }
    return left + L"\\" + right;
}

std::wstring RootFile(const wchar_t* fileName);

bool FileExists(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t FileSizeOrZero(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::wstring ReadIniString(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* fallback)
{
    wchar_t value[512] = {};
    GetPrivateProfileStringW(section, key, fallback, value, ARRAYSIZE(value), file.c_str());
    return value;
}

int ParseResolutionScale(const std::wstring& value)
{
    if (value.size() == 2 && value[1] == L'x' && value[0] >= L'1' && value[0] <= L'6') {
        return value[0] - L'0';
    }
    return 0;
}

std::wstring ScaleIniValue(int scale)
{
    wchar_t text[16] = {};
    swprintf_s(text, L"%dx", scale);
    return text;
}

std::wstring ScaleFloatValue(int scale)
{
    wchar_t text[16] = {};
    swprintf_s(text, L"%d.0", scale);
    return text;
}

std::wstring ScaleLabel(int scale)
{
    wchar_t text[64] = {};
    swprintf_s(text, L"%dx (%dx%d)", scale, 1280 * scale, 720 * scale);
    return text;
}

int CurrentResolutionScale()
{
    return ParseResolutionScale(ReadIniString(RootFile(L"dgVoodoo.conf"), L"DirectX", L"Resolution", L""));
}

int SelectedResolutionScale()
{
    if (!g_resolutionCombo) {
        const int current = CurrentResolutionScale();
        return current > 0 ? current : 3;
    }

    const LRESULT selected = SendMessageW(g_resolutionCombo, CB_GETCURSEL, 0, 0);
    if (selected >= 0 && selected <= 6) {
        return static_cast<int>(selected);
    }

    const int current = CurrentResolutionScale();
    return current > 0 ? current : 3;
}

void SyncResolutionCombo()
{
    if (!g_resolutionCombo) {
        return;
    }

    const int scale = CurrentResolutionScale();
    SendMessageW(g_resolutionCombo, CB_SETCURSEL, scale >= 1 && scale <= 6 ? scale : 0, 0);
}

bool WriteIniString(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    return WritePrivateProfileStringW(section, key, value, file.c_str()) != FALSE;
}

std::string NarrowAscii(const wchar_t* text)
{
    std::string result;
    while (text && *text) {
        result.push_back(static_cast<char>(*text));
        ++text;
    }
    return result;
}

std::string TrimAscii(std::string text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

bool EqualsNoCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = static_cast<char>(tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

std::string DgVoodooLine(const std::string& key, const std::string& value)
{
    std::string padded = key;
    if (padded.size() < 36) {
        padded.append(36 - padded.size(), ' ');
    }
    return padded + "= " + value;
}

bool ReadWholeFile(const std::wstring& path, std::string& out)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = out.empty() || ReadFile(file, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != out.size()) {
        return false;
    }
    return true;
}

bool WriteWholeFile(const std::wstring& path, const std::string& data)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const bool ok = data.empty() || WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == data.size();
}

std::vector<std::string> SplitLines(const std::string& content)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= content.size()) {
        size_t end = content.find('\n', start);
        if (end == std::string::npos) {
            end = content.size();
        }
        std::string line = content.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
        if (end == content.size()) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

bool SetDgVoodooString(const std::wstring& file, const wchar_t* sectionWide, const wchar_t* keyWide, const wchar_t* valueWide)
{
    std::string content;
    if (!ReadWholeFile(file, content)) {
        return false;
    }

    const std::string section = NarrowAscii(sectionWide);
    const std::string key = NarrowAscii(keyWide);
    const std::string value = NarrowAscii(valueWide);
    std::vector<std::string> lines = SplitLines(content);

    bool inSection = false;
    bool sectionSeen = false;
    bool keyWritten = false;
    size_t insertAt = lines.size();

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = TrimAscii(lines[i]);
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            if (inSection && !keyWritten) {
                insertAt = i;
            }
            inSection = EqualsNoCase(trimmed.substr(1, trimmed.size() - 2), section);
            sectionSeen = sectionSeen || inSection;
            continue;
        }

        if (!inSection || trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
            continue;
        }

        const size_t equals = lines[i].find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string lineKey = TrimAscii(lines[i].substr(0, equals));
        if (!EqualsNoCase(lineKey, key)) {
            continue;
        }

        if (!keyWritten) {
            lines[i] = DgVoodooLine(key, value);
            keyWritten = true;
        } else {
            lines.erase(lines.begin() + i);
            --i;
        }
    }

    if (!sectionSeen) {
        lines.push_back("");
        lines.push_back("[" + section + "]");
        lines.push_back(DgVoodooLine(key, value));
    } else if (!keyWritten) {
        if (insertAt == lines.size()) {
            lines.push_back(DgVoodooLine(key, value));
        } else {
            lines.insert(lines.begin() + insertAt, DgVoodooLine(key, value));
        }
    }

    std::string output;
    for (const std::string& line : lines) {
        output += line;
        output += "\r\n";
    }
    return WriteWholeFile(file, output);
}

bool RemoveDgVoodooString(const std::wstring& file, const wchar_t* sectionWide, const wchar_t* keyWide)
{
    std::string content;
    if (!ReadWholeFile(file, content)) {
        return false;
    }

    const std::string section = NarrowAscii(sectionWide);
    const std::string key = NarrowAscii(keyWide);
    std::vector<std::string> lines = SplitLines(content);
    bool inSection = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = TrimAscii(lines[i]);
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            inSection = EqualsNoCase(trimmed.substr(1, trimmed.size() - 2), section);
            continue;
        }

        if (!inSection || trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
            continue;
        }

        const size_t equals = lines[i].find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string lineKey = TrimAscii(lines[i].substr(0, equals));
        if (EqualsNoCase(lineKey, key)) {
            lines.erase(lines.begin() + i);
            --i;
        }
    }

    std::string output;
    for (const std::string& line : lines) {
        output += line;
        output += "\r\n";
    }
    return WriteWholeFile(file, output);
}

bool EnsureDirectory(const std::wstring& path)
{
    if (DirectoryExists(path)) {
        return true;
    }
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool CopyFileChecked(const std::wstring& source, const std::wstring& target, bool overwrite)
{
    return CopyFileW(source.c_str(), target.c_str(), overwrite ? FALSE : TRUE) != FALSE;
}

bool IsGameRunning()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring exe(entry.szExeFile);
            std::transform(exe.begin(), exe.end(), exe.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(towlower(ch));
            });
            if (exe == L"hotd_ng.exe" || exe == L"totdo.exe") {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

void SetStatus(const std::wstring& text)
{
    SetWindowTextW(g_status, text.c_str());
}

void AppendStatus(const std::wstring& line)
{
    int length = GetWindowTextLengthW(g_status);
    std::wstring current(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(g_status, &current[0], length + 1);
    }
    current.resize(length);
    if (!current.empty()) {
        current += L"\r\n";
    }
    current += line;
    SetWindowTextW(g_status, current.c_str());
}

void ShowError(const std::wstring& message)
{
    MessageBoxW(g_mainWindow, message.c_str(), L"HOTD Tweaks", MB_ICONERROR | MB_OK);
    AppendStatus(L"ERROR: " + message);
}

bool RequireGameClosed()
{
    if (!IsGameRunning()) {
        return true;
    }
    ShowError(L"Close Typing of the Dead before changing mod files or INI settings.");
    return false;
}

std::wstring RootFile(const wchar_t* fileName)
{
    return JoinPath(g_gameDir, fileName);
}

std::wstring BackupDir(bool original)
{
    return JoinPath(g_gameDir, original ? L"TOTDO-no-crosshair-original-assets" : L"TOTDO-no-crosshair-patched-assets");
}

bool WriteResourceToFile(int resourceId, const std::wstring& target)
{
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) {
        return false;
    }

    HGLOBAL loaded = LoadResource(nullptr, resource);
    if (!loaded) {
        return false;
    }

    const void* data = LockResource(loaded);
    const DWORD size = SizeofResource(nullptr, resource);
    if (!data || size == 0) {
        return false;
    }

    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    DWORD remaining = size;
    bool ok = true;
    while (remaining > 0) {
        const DWORD chunk = std::min<DWORD>(remaining, 1 << 20);
        DWORD written = 0;
        if (!WriteFile(file, bytes, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        bytes += chunk;
        remaining -= chunk;
    }

    CloseHandle(file);
    return ok;
}

bool RestoreOriginalCrosshairAssetsIfNeeded()
{
    const std::wstring originalDir = BackupDir(true);
    const std::wstring patchedDir = BackupDir(false);
    if (!DirectoryExists(originalDir) || !DirectoryExists(patchedDir)) {
        return true;
    }

    bool legacyPatchDetected = true;
    for (const AssetEntry& asset : kCrosshairAssets) {
        if (FileSizeOrZero(RootFile(asset.fileName)) !=
            FileSizeOrZero(JoinPath(patchedDir, asset.fileName))) {
            legacyPatchDetected = false;
            break;
        }
    }
    if (!legacyPatchDetected) {
        return true;
    }

    for (const AssetEntry& asset : kCrosshairAssets) {
        const std::wstring target = RootFile(asset.fileName);
        const std::wstring source = JoinPath(originalDir, asset.fileName);
        if (!CopyFileChecked(source, target, true)) {
            ShowError(L"Could not restore original game asset " + std::wstring(asset.fileName));
            return false;
        }
    }

    AppendStatus(L"Migrated legacy no-crosshair level files back to the original assets.");
    return true;
}

bool AssetSizesMatch(bool original)
{
    const std::wstring sourceDir = BackupDir(original);
    if (!DirectoryExists(sourceDir)) {
        return false;
    }

    for (const AssetEntry& asset : kCrosshairAssets) {
        const uint64_t rootSize = FileSizeOrZero(RootFile(asset.fileName));
        const uint64_t sourceSize = FileSizeOrZero(JoinPath(sourceDir, asset.fileName));
        if (rootSize == 0 || sourceSize == 0 || rootSize != sourceSize) {
            return false;
        }
    }
    return true;
}

std::wstring CrosshairState()
{
    const std::wstring ini = RootFile(L"HotDPostFXFix.ini");
    const std::wstring value = ReadIniString(ini, L"PostFXFix", L"HideCrosshair", L"0");
    return value == L"1" ? L"Hidden" : L"Visible";
}

bool EnsurePostFxBackend()
{
    const std::wstring backendDir = JoinPath(g_gameDir, L"dgVoodooBackend");
    if (!EnsureDirectory(backendDir)) {
        ShowError(L"Could not create " + backendDir);
        return false;
    }

    const std::wstring backendDll = JoinPath(backendDir, L"D3D9.dll");
    if (!FileExists(backendDll)) {
        const std::wstring renamed = RootFile(L"dgVoodoo_D3D9.dll");
        if (!FileExists(renamed)) {
            if (!WriteResourceToFile(kDgVoodooD3D9Resource, backendDll)) {
                ShowError(L"Missing dgVoodoo_D3D9.dll and could not extract bundled dgVoodoo backend.");
                return false;
            }
        } else if (!CopyFileChecked(renamed, backendDll, true)) {
            ShowError(L"Could not copy dgVoodoo_D3D9.dll into dgVoodooBackend.");
            return false;
        }
    }

    const std::wstring rootConf = RootFile(L"dgVoodoo.conf");
    const std::wstring backendConf = JoinPath(backendDir, L"dgVoodoo.conf");
    if (FileExists(rootConf) && !CopyFileChecked(rootConf, backendConf, true)) {
        ShowError(L"Could not copy dgVoodoo.conf into dgVoodooBackend.");
        return false;
    }

    return true;
}

bool InstallPostFxShim()
{
    if (!EnsurePostFxBackend()) {
        return false;
    }

    const std::wstring source = JoinPath(JoinPath(g_gameDir, L"HotDPostFXShim"), L"D3D9.dll");
    if (FileExists(source)) {
        if (!CopyFileChecked(source, RootFile(L"D3D9.dll"), true)) {
            ShowError(L"Could not install the post-FX D3D9 shim.");
            return false;
        }
    } else if (!WriteResourceToFile(kPostFxShimResource, RootFile(L"D3D9.dll"))) {
        ShowError(L"Could not install the post-FX D3D9 shim.");
        return false;
    }

    return true;
}

bool EnsureCompatibilityDefaults()
{
    const std::vector<std::wstring> configs = {
        RootFile(L"dgVoodoo.conf"),
        JoinPath(JoinPath(g_gameDir, L"dgVoodooBackend"), L"dgVoodoo.conf")
    };

    bool wroteAny = false;
    for (const std::wstring& conf : configs) {
        if (!FileExists(conf)) {
            continue;
        }

        wroteAny = true;
        RemoveDgVoodooString(conf, L"DirectX", L"AdapterIDType");
        RemoveDgVoodooString(conf, L"DirectX", L"EnumeratedResolutionBitdepths");
        RemoveDgVoodooString(conf, L"DirectX", L"RTTexturesForceScaleAndMSAA");
        SetDgVoodooString(conf, L"DirectXExt", L"AdapterIDType", L"nvidia");
        SetDgVoodooString(conf, L"DirectXExt", L"EnumeratedResolutionBitdepths", L"all");
        SetDgVoodooString(conf, L"DirectXExt", L"RTTexturesForceScaleAndMSAA", L"true");
    }
    return wroteAny;
}

bool SetCrosshairVisible(bool visible)
{
    if (!InstallPostFxShim() || !RestoreOriginalCrosshairAssetsIfNeeded()) {
        return false;
    }

    const std::wstring ini = RootFile(L"HotDPostFXFix.ini");
    if (!WriteIniString(ini, L"PostFXFix", L"HideCrosshair", visible ? L"0" : L"1")) {
        ShowError(L"Could not update the runtime crosshair setting.");
        return false;
    }

    AppendStatus(visible ? L"Crosshair visible." : L"Crosshair hidden by the runtime HUD hook.");
    return true;
}

bool SetPostFxEnabled(bool enabled)
{
    if (enabled && !InstallPostFxShim()) {
        return false;
    }

    const std::wstring ini = RootFile(L"HotDPostFXFix.ini");
    const int resolutionScale = CurrentResolutionScale();
    const std::wstring postFxScale = resolutionScale > 0 ? ScaleFloatValue(resolutionScale) : L"1.0";
    WriteIniString(ini, L"PostFXFix", L"Enabled", enabled ? L"1" : L"0");
    WriteIniString(ini, L"PostFXFix", L"Scale", postFxScale.c_str());
    WriteIniString(ini, L"PostFXFix", L"BloomRadiusDivisor", L"1.0");
    WriteIniString(ini, L"PostFXFix", L"CutsceneBloomIntensity", L"0.666667");
    WriteIniString(ini, L"PostFXFix", L"CutsceneBloomOffsetXTexels", L"-0.4375");
    WriteIniString(ini, L"PostFXFix", L"CutsceneDofRadiusDivisor", L"0.0");
    WriteIniString(ini, L"PostFXFix", L"CutsceneDofBlurOffsetXTexels", L"-2.0");
    WriteIniString(ini, L"PostFXFix", L"CutsceneDofBlurOffsetYTexels", L"-1.75");
    WriteIniString(ini, L"PostFXFix", L"CutsceneDofMaskOffsetXTexels", L"0.0");
    WriteIniString(ini, L"PostFXFix", L"CutsceneDofMaskOffsetYTexels", L"0.0");
    WriteIniString(ini, L"PostFXFix", L"CutsceneDofStrength", L"1.0");
    WriteIniString(ini, L"PostFXFix", L"Log", L"1");
    WriteIniString(ini, L"PostFXFix", L"AdjustInvTex", L"0");
    WriteIniString(ini, L"PostFXFix", L"AdjustBufferOffset", L"0");
    WriteIniString(ini, L"PostFXFix", L"InstallShaderHooks", L"1");
    WriteIniString(ini, L"PostFXFix", L"PatchOnDraw", L"1");

    AppendStatus(enabled ? L"Post-processing fix ON (bloom and cutscene DoF compensation)." : L"Post-processing fix OFF.");
    return true;
}

bool SetCutsceneEffectsEnabled(bool enabled)
{
    if (!enabled && !InstallPostFxShim()) {
        return false;
    }

    const std::wstring ini = RootFile(L"HotDPostFXFix.ini");
    WriteIniString(ini, L"PostFXFix", L"DisableCutsceneEffects", enabled ? L"0" : L"1");
    WriteIniString(ini, L"PostFXFix", L"Log", L"1");
    WriteIniString(ini, L"PostFXFix", L"InstallShaderHooks", L"1");
    WriteIniString(ini, L"PostFXFix", L"PatchOnDraw", L"1");
    WriteIniString(ini, L"PostFXFix", L"PatchFullscreenSmallConstants", L"1");

    AppendStatus(enabled ? L"Cutscene DoF ON." : L"Cutscene DoF OFF; bloom and other cutscene effects are unchanged.");
    return true;
}

bool SetResolutionFix(int scale)
{
    const bool enabled = scale > 0;
    if (enabled && !EnsurePostFxBackend()) {
        return false;
    }

    const int selectedScale = enabled ? scale : 1;
    const std::wstring resolutionValue = ScaleIniValue(selectedScale);
    const std::wstring postFxScale = ScaleFloatValue(selectedScale);

    const std::vector<std::wstring> configs = {
        RootFile(L"dgVoodoo.conf"),
        JoinPath(JoinPath(g_gameDir, L"dgVoodooBackend"), L"dgVoodoo.conf")
    };

    for (const std::wstring& conf : configs) {
        if (!FileExists(conf)) {
            continue;
        }

        SetDgVoodooString(conf, L"GeneralExt", L"FullscreenAttributes", L"fake");
        SetDgVoodooString(conf, L"GeneralExt", L"WatermarkDisplayDuration", L"0");
        SetDgVoodooString(conf, L"DirectX", L"Resolution", enabled ? resolutionValue.c_str() : L"unforced");
        SetDgVoodooString(conf, L"DirectX", L"dgVoodooWatermark", L"false");
        SetDgVoodooString(conf, L"Debug", L"Info", L"disable");
    }
    EnsureCompatibilityDefaults();

    const std::wstring postFxIni = RootFile(L"HotDPostFXFix.ini");
    WriteIniString(postFxIni, L"PostFXFix", L"Scale", enabled ? postFxScale.c_str() : L"1.0");

    AppendStatus(enabled ? L"Resolution fix ON: dgVoodoo DirectX Resolution = " + ScaleLabel(selectedScale) + L"." :
                           L"Resolution fix OFF: dgVoodoo DirectX Resolution = unforced.");
    return true;
}

std::wstring ResolutionState()
{
    const std::wstring conf = RootFile(L"dgVoodoo.conf");
    const std::wstring value = ReadIniString(conf, L"DirectX", L"Resolution", L"missing");
    const int scale = ParseResolutionScale(value);
    if (scale > 0) {
        return L"ON / " + ScaleLabel(scale);
    }
    if (value == L"unforced") {
        return L"OFF / unforced";
    }
    return L"custom / " + value;
}

std::wstring PostFxState()
{
    const std::wstring ini = RootFile(L"HotDPostFXFix.ini");
    const std::wstring value = ReadIniString(ini, L"PostFXFix", L"Enabled", L"0");
    return value == L"1" ? L"ON" : L"OFF";
}

std::wstring CutsceneFxState()
{
    const std::wstring ini = RootFile(L"HotDPostFXFix.ini");
    const std::wstring value = ReadIniString(ini, L"PostFXFix", L"DisableCutsceneEffects", L"0");
    return value == L"1" ? L"OFF" : L"ON";
}

void RefreshStatus()
{
    const int scale = CurrentResolutionScale();
    const bool crosshairVisible =
        ReadIniString(RootFile(L"HotDPostFXFix.ini"), L"PostFXFix", L"HideCrosshair", L"0") != L"1";
    const bool postFxEnabled = PostFxState() == L"ON";
    const bool cutsceneDofEnabled = CutsceneFxState() == L"ON";

    std::wstring status = IsGameRunning() ? L"Game is running. Close it before applying changes." :
                                            L"Ready. Changes are applied only when you press Apply Changes.";
    status += L"\r\nInstalled: ";
    status += scale > 0 ? std::to_wstring(scale) + L"x" : L"resolution off";
    status += crosshairVisible ? L" | crosshair visible" : L" | crosshair hidden";
    status += postFxEnabled ? L" | FX on" : L" | FX off";
    status += cutsceneDofEnabled ? L" | DoF on" : L" | DoF off";
    SetStatus(status);
}

void OpenFileIfPresent(const std::wstring& path)
{
    if (!FileExists(path)) {
        AppendStatus(L"File does not exist yet: " + path);
        return;
    }
    ShellExecuteW(g_mainWindow, L"open", path.c_str(), nullptr, g_gameDir.c_str(), SW_SHOWNORMAL);
}

Settings ReadSettings()
{
    Settings settings;
    settings.resolutionScale = CurrentResolutionScale();
    settings.crosshairVisible =
        ReadIniString(RootFile(L"HotDPostFXFix.ini"), L"PostFXFix", L"HideCrosshair", L"0") != L"1";
    settings.postFxEnabled = PostFxState() == L"ON";
    settings.cutsceneDofEnabled = CutsceneFxState() == L"ON";
    return settings;
}

Settings ReadUiSettings()
{
    Settings settings;
    settings.resolutionScale =
        static_cast<int>(SendMessageW(g_resolutionCombo, CB_GETCURSEL, 0, 0));
    settings.crosshairVisible =
        SendMessageW(g_crosshairCombo, CB_GETCURSEL, 0, 0) == 0;
    settings.postFxEnabled =
        SendMessageW(g_postFxCombo, CB_GETCURSEL, 0, 0) == 0;
    settings.cutsceneDofEnabled =
        SendMessageW(g_cutsceneDofCombo, CB_GETCURSEL, 0, 0) == 0;
    return settings;
}

bool SettingsEqual(const Settings& left, const Settings& right)
{
    return left.resolutionScale == right.resolutionScale &&
        left.crosshairVisible == right.crosshairVisible &&
        left.postFxEnabled == right.postFxEnabled &&
        left.cutsceneDofEnabled == right.cutsceneDofEnabled;
}

void SetDirty(bool dirty)
{
    g_dirty = dirty;
    if (g_applyButton) {
        EnableWindow(g_applyButton, dirty ? TRUE : FALSE);
        InvalidateRect(g_applyButton, nullptr, TRUE);
    }
    if (g_mainWindow) {
        InvalidateRect(g_mainWindow, nullptr, FALSE);
    }
}

void UpdateDirtyState()
{
    if (!g_loadingUi) {
        SetDirty(!SettingsEqual(ReadUiSettings(), g_appliedSettings));
    }
}

void LoadSettingsIntoUi()
{
    g_loadingUi = true;
    g_appliedSettings = ReadSettings();
    SendMessageW(g_resolutionCombo, CB_SETCURSEL, g_appliedSettings.resolutionScale, 0);
    SendMessageW(g_crosshairCombo, CB_SETCURSEL, g_appliedSettings.crosshairVisible ? 0 : 1, 0);
    SendMessageW(g_postFxCombo, CB_SETCURSEL, g_appliedSettings.postFxEnabled ? 0 : 1, 0);
    SendMessageW(g_cutsceneDofCombo, CB_SETCURSEL, g_appliedSettings.cutsceneDofEnabled ? 0 : 1, 0);
    g_loadingUi = false;
    SetDirty(false);
    RefreshStatus();
}

bool ApplyUiSettings()
{
    if (!RequireGameClosed()) {
        return false;
    }

    const Settings desired = ReadUiSettings();
    SetStatus(L"Applying changes...");

    if (!EnsurePostFxBackend() ||
        !EnsureCompatibilityDefaults() ||
        !InstallPostFxShim() ||
        !SetResolutionFix(desired.resolutionScale) ||
        !SetCrosshairVisible(desired.crosshairVisible) ||
        !SetPostFxEnabled(desired.postFxEnabled) ||
        !SetCutsceneEffectsEnabled(desired.cutsceneDofEnabled)) {
        RefreshStatus();
        return false;
    }

    g_appliedSettings = ReadSettings();
    SetDirty(false);
    SetStatus(L"Changes applied. The torso compatibility fix is active automatically.\r\n"
              L"Close HOTD Tweaks and launch either mode through Steam.");
    return true;
}

HWND AddButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, bool ownerDraw = false)
{
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        (ownerDraw ? BS_OWNERDRAW : BS_PUSHBUTTON);
    HWND button = CreateWindowW(
        L"BUTTON", text, style, x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), nullptr, nullptr);
    SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SetWindowTheme(button, L"Explorer", nullptr);
    return button;
}

HWND AddCombo(HWND parent, int id, int x, int y, int w)
{
    HWND combo = CreateWindowW(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, y, w, 220, parent,
        reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), nullptr, nullptr);
    SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SetWindowTheme(combo, L"Explorer", nullptr);
    return combo;
}

void AddOnOffItems(HWND combo, const wchar_t* onText, const wchar_t* offText)
{
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(onText));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(offText));
}

void DrawTextLine(
    HDC dc,
    const wchar_t* text,
    int x,
    int y,
    HFONT font,
    COLORREF color)
{
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT rect = {x, y, 680, y + 32};
    DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, oldFont);
}

void PaintWindow(HWND hwnd)
{
    PAINTSTRUCT paint = {};
    HDC dc = BeginPaint(hwnd, &paint);
    RECT client = {};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, g_backgroundBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(224, 229, 235));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, borderPen));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, g_cardBrush));
    RoundRect(dc, 28, 108, client.right - 28, 402, 16, 16);
    RoundRect(dc, 28, 420, client.right - 28, 510, 16, 16);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(borderPen);

    DrawTextLine(dc, L"HOTD Tweaks", 32, 22, g_titleFont, RGB(24, 32, 42));
    DrawTextLine(
        dc,
        L"Resolution and post-FX fixes for House of the Dead and Typing of the Dead",
        32,
        62,
        g_subtitleFont,
        RGB(91, 103, 117));

    struct Row {
        const wchar_t* title;
        const wchar_t* description;
        int y;
    };
    const Row rows[] = {
        {L"Rendering resolution", L"Internal 1280x720 render multiplied before desktop scaling.", 126},
        {L"In-game crosshair", L"Runtime HUD hook; original level files remain untouched.", 190},
        {L"Post-FX fix", L"Corrects high-resolution cutscene bloom and DoF alignment.", 254},
        {L"Cutscene depth of field", L"Keep the corrected effect, or bypass DoF only.", 318},
    };
    for (const Row& row : rows) {
        DrawTextLine(dc, row.title, 52, row.y, g_font, RGB(30, 41, 54));
        DrawTextLine(dc, row.description, 52, row.y + 24, g_subtitleFont, RGB(101, 113, 128));
    }

    DrawTextLine(
        dc,
        g_dirty ? L"Unsaved changes" : L"Configuration status",
        52,
        428,
        g_font,
        g_dirty ? RGB(37, 99, 235) : RGB(30, 41, 54));

    EndPaint(hwnd, &paint);
}

void DrawApplyButton(const DRAWITEMSTRUCT* draw)
{
    const bool enabled = (draw->itemState & ODS_DISABLED) == 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const COLORREF fill = enabled
        ? (pressed ? RGB(29, 78, 216) : RGB(37, 99, 235))
        : RGB(222, 227, 233);
    const COLORREF text = enabled ? RGB(255, 255, 255) : RGB(112, 123, 136);

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(draw->hDC, brush));
    HPEN oldPen = static_cast<HPEN>(SelectObject(draw->hDC, pen));
    RoundRect(
        draw->hDC,
        draw->rcItem.left,
        draw->rcItem.top,
        draw->rcItem.right,
        draw->rcItem.bottom,
        10,
        10);
    SelectObject(draw->hDC, oldBrush);
    SelectObject(draw->hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t label[64] = {};
    GetWindowTextW(draw->hwndItem, label, ARRAYSIZE(label));
    HFONT oldFont = static_cast<HFONT>(SelectObject(draw->hDC, g_font));
    SetTextColor(draw->hDC, text);
    SetBkMode(draw->hDC, TRANSPARENT);
    RECT textRect = draw->rcItem;
    DrawTextW(draw->hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(draw->hDC, oldFont);

    if (draw->itemState & ODS_FOCUS) {
        RECT focus = draw->rcItem;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(draw->hDC, &focus);
    }
}

bool ConfirmDiscardChanges()
{
    if (!g_dirty) {
        return true;
    }
    return MessageBoxW(
        g_mainWindow,
        L"Discard the unapplied setting changes?",
        L"HOTD Tweaks",
        MB_ICONQUESTION | MB_YESNO) == IDYES;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
        g_font = CreateFontW(
            -17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_titleFont = CreateFontW(
            -30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_subtitleFont = CreateFontW(
            -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_backgroundBrush = CreateSolidBrush(RGB(246, 248, 250));
        g_cardBrush = CreateSolidBrush(RGB(255, 255, 255));

        g_resolutionCombo = AddCombo(hwnd, IDC_RESOLUTION, 420, 130, 240);
        SendMessageW(g_resolutionCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Off"));
        for (int scale = 1; scale <= 6; ++scale) {
            const std::wstring label = ScaleLabel(scale);
            SendMessageW(g_resolutionCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }

        g_crosshairCombo = AddCombo(hwnd, IDC_CROSSHAIR, 420, 194, 240);
        AddOnOffItems(g_crosshairCombo, L"Visible", L"Hidden");
        g_postFxCombo = AddCombo(hwnd, IDC_POSTFX, 420, 258, 240);
        AddOnOffItems(g_postFxCombo, L"On", L"Off");
        g_cutsceneDofCombo = AddCombo(hwnd, IDC_CUTSCENE_DOF, 420, 322, 240);
        AddOnOffItems(g_cutsceneDofCombo, L"On (corrected)", L"Off");

        g_status = CreateWindowW(
            L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            52, 452, 600, 50, hwnd,
            reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATUS)), nullptr, nullptr);
        SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_subtitleFont), TRUE);

        AddButton(hwnd, IDC_REFRESH, L"Reload", 32, 528, 110, 40);
        AddButton(hwnd, IDC_OPEN_LOG, L"Open Log", 154, 528, 120, 40);
        g_applyButton = AddButton(hwnd, IDC_APPLY, L"Apply Changes", 500, 528, 176, 40, true);

        if (!IsGameRunning()) {
            EnsureCompatibilityDefaults();
        }
        LoadSettingsIntoUi();
        return 0;

    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wparam), RGB(91, 103, 117));
        return reinterpret_cast<LRESULT>(g_cardBrush);

    case WM_DRAWITEM:
        if (wparam == IDC_APPLY) {
            DrawApplyButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (HIWORD(wparam) == CBN_SELCHANGE) {
            switch (LOWORD(wparam)) {
            case IDC_RESOLUTION:
            case IDC_CROSSHAIR:
            case IDC_POSTFX:
            case IDC_CUTSCENE_DOF:
                UpdateDirtyState();
                return 0;
            default:
                break;
            }
        }

        switch (LOWORD(wparam)) {
        case IDC_APPLY:
            ApplyUiSettings();
            return 0;
        case IDC_REFRESH:
            if (ConfirmDiscardChanges()) {
                LoadSettingsIntoUi();
            }
            return 0;
        case IDC_OPEN_LOG:
            OpenFileIfPresent(RootFile(L"HotDPostFXFix.log"));
            return 0;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        if (ConfirmDiscardChanges()) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        DeleteObject(g_font);
        DeleteObject(g_titleFont);
        DeleteObject(g_subtitleFont);
        DeleteObject(g_backgroundBrush);
        DeleteObject(g_cardBrush);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    g_gameDir = ExeDirectory();

    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && argumentCount > 1) {
        bool handled = false;
        bool crosshairVisible = true;
        if (_wcsicmp(arguments[1], L"/crosshair:on") == 0) {
            handled = true;
            crosshairVisible = true;
        } else if (_wcsicmp(arguments[1], L"/crosshair:off") == 0) {
            handled = true;
            crosshairVisible = false;
        }

        if (handled) {
            LocalFree(arguments);
            if (IsGameRunning()) {
                MessageBoxW(
                    nullptr,
                    L"Close Typing of the Dead before changing the crosshair setting.",
                    L"HOTD Tweaks",
                    MB_ICONWARNING | MB_OK);
                return 1;
            }
            return EnsurePostFxBackend() &&
                    EnsureCompatibilityDefaults() &&
                    SetCrosshairVisible(crosshairVisible)
                ? 0
                : 1;
        }
    }
    if (arguments) {
        LocalFree(arguments);
    }

    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    const wchar_t* className = L"HOTD_Tweaks_Window";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = className;

    RegisterClassW(&wc);

    g_mainWindow = CreateWindowExW(
        WS_EX_APPWINDOW,
        className,
        L"HOTD Tweaks",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        724,
        616,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!g_mainWindow) {
        return 1;
    }

    ShowWindow(g_mainWindow, showCommand);
    UpdateWindow(g_mainWindow);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
