#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "../third_party/zlib/zlib.h"

#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr int IDC_RESOLUTION = 101;
constexpr int IDC_CROSSHAIR = 102;
constexpr int IDC_POSTFX = 103;
constexpr int IDC_CUTSCENE_DOF = 104;
constexpr int IDC_APPLY = 105;
constexpr int IDC_REFRESH = 106;
constexpr int IDC_OPEN_LOG = 107;
constexpr int IDC_STATUS = 108;
constexpr int IDC_PROGRESS_TEXT = 109;
constexpr int IDC_PROGRESS = 110;
constexpr int IDC_TORSO_FIX = 111;
constexpr int IDC_RETICLE_WARNING = 112;

constexpr int kPostFxShimResource = 4000;
constexpr int kDgVoodooD3D9Resource = 4001;
constexpr int kCrosshairPatchResource = 4002;
constexpr int kAppIconResource = 5000;
constexpr int kMaxResolutionScale = 8;
constexpr size_t kAssetHeaderSize = 0x190;
constexpr size_t kCompressedSizeOffset = 0x140;
constexpr size_t kUncompressedSizeOffset = 0x144;

struct AssetEntry {
    const wchar_t* fileName;
    const char* patchName;
};

const AssetEntry kCrosshairAssets[] = {
    {L"l1_plantation_house_hud_permasector.pc", "campaign"},
    {L"l2_hospital_hud_permasector.pc", "campaign"},
    {L"l3_carnival_hud_permasector.pc", "campaign"},
    {L"l4_train_hud_permasector.pc", "campaign"},
    {L"l5_swamps_hud_permasector.pc", "campaign"},
    {L"l6_prison_hud_permasector.pc", "campaign"},
    {L"l7_labs_hud_permasector.pc", "campaign"},
    {L"l7_labs_world.pc", "labs"},
    {L"l8_strip_club_hud_permasector.pc", "campaign"},
    {L"l9_factory_hud_permasector.pc", "campaign"},
    {L"w_civilians.pc", "civilians"},
    {L"w_database_world.pc", "database"},
    {L"w_shootinggallery.pc", "shooting"},
    {L"w_survivalmode_hud_permasector.pc", "survival"},
};

struct CrosshairPatchRun {
    uint32_t offset = 0;
    const uint8_t* originalBytes = nullptr;
    uint32_t length = 0;
};

struct CrosshairPatch {
    std::string name;
    uint32_t uncompressedSize = 0;
    std::array<uint8_t, 32> originalHash = {};
    std::array<uint8_t, 32> patchedHash = {};
    std::vector<CrosshairPatchRun> runs;
};

HWND g_status = nullptr;
HWND g_mainWindow = nullptr;
HWND g_resolutionCombo = nullptr;
HWND g_crosshairCombo = nullptr;
HWND g_postFxCombo = nullptr;
HWND g_cutsceneDofCombo = nullptr;
HWND g_torsoFixCombo = nullptr;
HWND g_applyButton = nullptr;
HWND g_refreshButton = nullptr;
HWND g_openLogButton = nullptr;
HWND g_progressText = nullptr;
HWND g_progressBar = nullptr;
HWND g_reticleWarning = nullptr;
HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_subtitleFont = nullptr;
HBRUSH g_backgroundBrush = nullptr;
HBRUSH g_cardBrush = nullptr;
std::wstring g_gameDir;
bool g_loadingUi = false;
bool g_dirty = false;
bool g_applyInProgress = false;
int g_progressCurrent = 0;
int g_progressMaximum = 1;
std::wstring g_progressStep;

struct Settings {
    int resolutionScale = 0;
    bool crosshairVisible = true;
    bool postFxEnabled = true;
    bool cutsceneDofEnabled = true;
    bool torsoFixEnabled = true;
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

std::wstring ParentDirectory(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring FindGameDirectory()
{
    std::wstring candidate = ExeDirectory();
    for (int depth = 0; depth < 5 && !candidate.empty(); ++depth) {
        if (FileExists(JoinPath(candidate, L"HOTD_NG.exe"))) {
            return candidate;
        }
        candidate = ParentDirectory(candidate);
    }
    return ExeDirectory();
}

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ReadIniString(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* fallback)
{
    wchar_t value[512] = {};
    GetPrivateProfileStringW(section, key, fallback, value, ARRAYSIZE(value), file.c_str());
    return value;
}

int ParseResolutionScale(const std::wstring& value)
{
    if (value.size() == 2 && value[1] == L'x' &&
        value[0] >= L'1' &&
        value[0] <= L'0' + kMaxResolutionScale) {
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
    if (selected >= 0 && selected <= kMaxResolutionScale) {
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
    SendMessageW(
        g_resolutionCombo,
        CB_SETCURSEL,
        scale >= 1 && scale <= kMaxResolutionScale ? scale : 0,
        0);
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

void PumpUiMessages()
{
    if (!g_mainWindow) {
        return;
    }

    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UpdateWindow(g_mainWindow);
}

void SetInteractiveControlsEnabled(bool enabled)
{
    EnableWindow(g_resolutionCombo, enabled);
    EnableWindow(g_crosshairCombo, enabled);
    EnableWindow(g_postFxCombo, enabled);
    EnableWindow(g_cutsceneDofCombo, enabled);
    EnableWindow(g_torsoFixCombo, enabled);
    EnableWindow(g_refreshButton, enabled);
    EnableWindow(g_openLogButton, enabled);
    EnableWindow(g_applyButton, enabled && g_dirty);
}

void SetReticleWarningVisible(bool visible)
{
    if (!g_reticleWarning) {
        return;
    }
    ShowWindow(g_reticleWarning, visible ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_reticleWarning, nullptr, TRUE);
    PumpUiMessages();
}

void BeginApplyProgress(int maximum)
{
    g_applyInProgress = true;
    g_progressCurrent = 0;
    g_progressMaximum = std::max(maximum, 1);
    g_progressStep = L"Preparing changes...";
    SendMessageW(g_progressBar, PBM_SETSTATE, PBST_NORMAL, 0);
    SendMessageW(g_progressBar, PBM_SETRANGE32, 0, g_progressMaximum);
    SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_progressText, g_progressStep.c_str());
    SetReticleWarningVisible(false);
    SetInteractiveControlsEnabled(false);
    PumpUiMessages();
}

void SetApplyProgressStep(const std::wstring& text)
{
    if (!g_applyInProgress) {
        return;
    }
    g_progressStep = text;
    SetWindowTextW(g_progressText, text.c_str());
    PumpUiMessages();
}

void CompleteApplyProgressStep()
{
    if (!g_applyInProgress) {
        return;
    }
    g_progressCurrent = std::min(g_progressCurrent + 1, g_progressMaximum);
    SendMessageW(g_progressBar, PBM_SETPOS, g_progressCurrent, 0);
    PumpUiMessages();
}

void FinishApplyProgress(bool success)
{
    if (!g_applyInProgress) {
        return;
    }

    if (success) {
        g_progressCurrent = g_progressMaximum;
        SendMessageW(g_progressBar, PBM_SETSTATE, PBST_NORMAL, 0);
        SendMessageW(g_progressBar, PBM_SETPOS, g_progressMaximum, 0);
        SetWindowTextW(g_progressText, L"All changes applied successfully.");
    } else {
        SendMessageW(g_progressBar, PBM_SETSTATE, PBST_ERROR, 0);
        SetWindowTextW(
            g_progressText,
            (L"Stopped while " + g_progressStep).c_str());
    }

    g_applyInProgress = false;
    SetReticleWarningVisible(false);
    SetInteractiveControlsEnabled(true);
    PumpUiMessages();
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

bool ReadBinaryFile(const std::wstring& path, std::vector<uint8_t>& data)
{
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 256LL * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    data.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    bool ok = true;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(data.size() - offset, 1 << 20));
        DWORD read = 0;
        if (!ReadFile(file, data.data() + offset, chunk, &read, nullptr) ||
            read != chunk) {
            ok = false;
            break;
        }
        offset += read;
    }

    CloseHandle(file);
    return ok;
}

bool WriteBinaryFileAtomic(const std::wstring& path, const std::vector<uint8_t>& data)
{
    const std::wstring temporary = path + L".hotdtweaks.tmp";
    DeleteFileW(temporary.c_str());

    HANDLE file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    size_t offset = 0;
    bool ok = true;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(data.size() - offset, 1 << 20));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            ok = false;
            break;
        }
        offset += written;
    }

    if (ok) {
        ok = FlushFileBuffers(file) != FALSE;
    }
    CloseHandle(file);

    if (ok) {
        ok = MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) {
        DeleteFileW(temporary.c_str());
    }
    return ok;
}

uint32_t ReadUint32(const uint8_t* bytes)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

void WriteUint32(uint8_t* bytes, uint32_t value)
{
    std::memcpy(bytes, &value, sizeof(value));
}

bool Sha256(const std::vector<uint8_t>& data, std::array<uint8_t, 32>& hash)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hashing = nullptr;
    DWORD objectSize = 0;
    DWORD returned = 0;
    std::vector<uint8_t> object;

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) {
        status = BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &returned, 0);
    }
    if (status >= 0) {
        object.resize(objectSize);
        status = BCryptCreateHash(
            algorithm, &hashing, object.data(), objectSize, nullptr, 0, 0);
    }
    if (status >= 0 && !data.empty()) {
        status = BCryptHashData(
            hashing, const_cast<PUCHAR>(data.data()),
            static_cast<ULONG>(data.size()), 0);
    }
    if (status >= 0) {
        status = BCryptFinishHash(
            hashing, hash.data(), static_cast<ULONG>(hash.size()), 0);
    }

    if (hashing) {
        BCryptDestroyHash(hashing);
    }
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return status >= 0;
}

bool LoadCrosshairPatches(std::vector<CrosshairPatch>& patches)
{
    static bool loaded = false;
    static bool loadResult = false;
    static std::vector<CrosshairPatch> cached;
    if (loaded) {
        patches = cached;
        return loadResult;
    }
    loaded = true;

    HRSRC resource = FindResourceW(
        nullptr, MAKEINTRESOURCEW(kCrosshairPatchResource), RT_RCDATA);
    HGLOBAL handle = resource ? LoadResource(nullptr, resource) : nullptr;
    const uint8_t* cursor = handle
        ? static_cast<const uint8_t*>(LockResource(handle))
        : nullptr;
    const DWORD resourceSize = resource ? SizeofResource(nullptr, resource) : 0;
    const uint8_t* end = cursor ? cursor + resourceSize : nullptr;

    auto readBytes = [&](void* target, size_t size) {
        if (!cursor || size > static_cast<size_t>(end - cursor)) {
            return false;
        }
        std::memcpy(target, cursor, size);
        cursor += size;
        return true;
    };
    auto readUint32 = [&](uint32_t& value) {
        return readBytes(&value, sizeof(value));
    };

    char magic[8] = {};
    uint32_t version = 0;
    uint32_t patchCount = 0;
    if (!readBytes(magic, sizeof(magic)) ||
        std::memcmp(magic, "HOTDCHP1", sizeof(magic)) != 0 ||
        !readUint32(version) || version != 1 ||
        !readUint32(patchCount) || patchCount == 0 || patchCount > 32) {
        return false;
    }

    for (uint32_t patchIndex = 0; patchIndex < patchCount; ++patchIndex) {
        CrosshairPatch patch;
        uint32_t nameLength = 0;
        uint32_t runCount = 0;
        if (!readUint32(nameLength) || nameLength == 0 || nameLength > 64 ||
            nameLength > static_cast<uint32_t>(end - cursor)) {
            return false;
        }
        patch.name.assign(reinterpret_cast<const char*>(cursor), nameLength);
        cursor += nameLength;

        if (!readUint32(patch.uncompressedSize) ||
            !readBytes(patch.originalHash.data(), patch.originalHash.size()) ||
            !readBytes(patch.patchedHash.data(), patch.patchedHash.size()) ||
            !readUint32(runCount) || runCount > 100000) {
            return false;
        }

        patch.runs.reserve(runCount);
        for (uint32_t runIndex = 0; runIndex < runCount; ++runIndex) {
            CrosshairPatchRun run;
            if (!readUint32(run.offset) || !readUint32(run.length) ||
                run.length == 0 ||
                run.offset > patch.uncompressedSize ||
                run.length > patch.uncompressedSize - run.offset ||
                run.length > static_cast<uint32_t>(end - cursor)) {
                return false;
            }
            run.originalBytes = cursor;
            cursor += run.length;
            patch.runs.push_back(run);
        }
        cached.push_back(std::move(patch));
    }

    loadResult = cursor == end;
    if (!loadResult) {
        cached.clear();
    }
    patches = cached;
    return loadResult;
}

const CrosshairPatch* FindCrosshairPatch(
    const std::vector<CrosshairPatch>& patches,
    const char* name)
{
    for (const CrosshairPatch& patch : patches) {
        if (patch.name == name) {
            return &patch;
        }
    }
    return nullptr;
}

bool InflateAsset(
    const std::vector<uint8_t>& fileData,
    const CrosshairPatch& patch,
    std::vector<uint8_t>& raw)
{
    if (fileData.size() < kAssetHeaderSize) {
        return false;
    }

    const uint32_t compressedSize =
        ReadUint32(fileData.data() + kCompressedSizeOffset);
    const uint32_t uncompressedSize =
        ReadUint32(fileData.data() + kUncompressedSizeOffset);
    if (uncompressedSize != patch.uncompressedSize ||
        compressedSize > fileData.size() - kAssetHeaderSize) {
        return false;
    }

    raw.resize(uncompressedSize);
    z_stream stream = {};
    stream.next_in = const_cast<Bytef*>(fileData.data() + kAssetHeaderSize);
    stream.avail_in = compressedSize;
    stream.next_out = raw.data();
    stream.avail_out = uncompressedSize;

    if (inflateInit(&stream) != Z_OK) {
        return false;
    }
    const int result = inflate(&stream, Z_FINISH);
    const bool ok =
        result == Z_STREAM_END && stream.total_out == uncompressedSize;
    inflateEnd(&stream);
    return ok;
}

bool DeflateAsset(
    const std::vector<uint8_t>& originalFile,
    const std::vector<uint8_t>& raw,
    std::vector<uint8_t>& output)
{
    z_stream stream = {};
    if (deflateInit(&stream, Z_BEST_COMPRESSION) != Z_OK) {
        return false;
    }

    const uLong bound = deflateBound(&stream, static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(bound);
    stream.next_in = const_cast<Bytef*>(raw.data());
    stream.avail_in = static_cast<uInt>(raw.size());
    stream.next_out = compressed.data();
    stream.avail_out = static_cast<uInt>(compressed.size());

    const int result = deflate(&stream, Z_FINISH);
    const bool ok =
        result == Z_STREAM_END && stream.total_in == raw.size();
    const size_t compressedSize = static_cast<size_t>(stream.total_out);
    deflateEnd(&stream);
    if (!ok || compressedSize > UINT32_MAX) {
        return false;
    }

    compressed.resize(compressedSize);
    output.assign(
        originalFile.begin(), originalFile.begin() + kAssetHeaderSize);
    WriteUint32(
        output.data() + kCompressedSizeOffset,
        static_cast<uint32_t>(compressed.size()));
    WriteUint32(
        output.data() + kUncompressedSizeOffset,
        static_cast<uint32_t>(raw.size()));
    output.insert(output.end(), compressed.begin(), compressed.end());
    output.resize((output.size() + 15) & ~size_t(15), 0);
    return true;
}

enum class CrosshairAssetState {
    Original,
    Patched,
    Unknown
};

CrosshairAssetState GetCrosshairAssetState(
    const std::wstring& path,
    const CrosshairPatch& patch,
    std::vector<uint8_t>* fileDataOut = nullptr,
    std::vector<uint8_t>* rawOut = nullptr)
{
    std::vector<uint8_t> fileData;
    std::vector<uint8_t> raw;
    std::array<uint8_t, 32> hash = {};
    if (!ReadBinaryFile(path, fileData) ||
        !InflateAsset(fileData, patch, raw) ||
        !Sha256(raw, hash)) {
        return CrosshairAssetState::Unknown;
    }

    if (fileDataOut) {
        *fileDataOut = std::move(fileData);
    }
    if (rawOut) {
        *rawOut = std::move(raw);
    }

    if (hash == patch.originalHash) {
        return CrosshairAssetState::Original;
    }
    if (hash == patch.patchedHash) {
        return CrosshairAssetState::Patched;
    }
    return CrosshairAssetState::Unknown;
}

bool SetCrosshairAssetState(
    const AssetEntry& asset,
    const CrosshairPatch& patch,
    bool hidden)
{
    const std::wstring path = RootFile(asset.fileName);
    std::vector<uint8_t> fileData;
    std::vector<uint8_t> raw;
    const CrosshairAssetState state =
        GetCrosshairAssetState(path, patch, &fileData, &raw);
    const CrosshairAssetState desired =
        hidden ? CrosshairAssetState::Patched : CrosshairAssetState::Original;
    if (state == desired) {
        return true;
    }
    if (state == CrosshairAssetState::Unknown) {
        ShowError(
            L"Crosshair patch does not recognize " +
            std::wstring(asset.fileName) +
            L". Verify the game files in Steam, then try again.");
        return false;
    }

    for (const CrosshairPatchRun& run : patch.runs) {
        uint8_t* destination = raw.data() + run.offset;
        if (hidden) {
            std::memset(destination, 0, run.length);
        } else {
            std::memcpy(destination, run.originalBytes, run.length);
        }
    }

    std::array<uint8_t, 32> resultHash = {};
    const std::array<uint8_t, 32>& expectedHash =
        hidden ? patch.patchedHash : patch.originalHash;
    if (!Sha256(raw, resultHash) || resultHash != expectedHash) {
        ShowError(
            L"Internal crosshair patch verification failed for " +
            std::wstring(asset.fileName) + L".");
        return false;
    }

    std::vector<uint8_t> output;
    if (!DeflateAsset(fileData, raw, output) ||
        !WriteBinaryFileAtomic(path, output)) {
        ShowError(
            L"Could not update crosshair data in " +
            std::wstring(asset.fileName) + L".");
        return false;
    }
    return true;
}

bool ApplyCrosshairAssetPatches(bool hidden)
{
    std::vector<CrosshairPatch> patches;
    if (!LoadCrosshairPatches(patches)) {
        ShowError(L"The bundled crosshair patch resource is invalid.");
        return false;
    }

    int assetIndex = 0;
    for (const AssetEntry& asset : kCrosshairAssets) {
        ++assetIndex;
        SetApplyProgressStep(
            L"Updating level file " + std::to_wstring(assetIndex) + L"/" +
            std::to_wstring(ARRAYSIZE(kCrosshairAssets)) + L": " +
            asset.fileName);
        const CrosshairPatch* patch =
            FindCrosshairPatch(patches, asset.patchName);
        if (!patch) {
            ShowError(
                L"The bundled crosshair patch is missing data for " +
                std::wstring(asset.fileName) + L".");
            return false;
        }
        if (!SetCrosshairAssetState(asset, *patch, hidden)) {
            return false;
        }
        CompleteApplyProgressStep();
    }
    return true;
}

CrosshairAssetState DetectCrosshairState()
{
    std::vector<CrosshairPatch> patches;
    if (!LoadCrosshairPatches(patches)) {
        return CrosshairAssetState::Unknown;
    }
    const CrosshairPatch* patch =
        FindCrosshairPatch(patches, kCrosshairAssets[0].patchName);
    return patch
        ? GetCrosshairAssetState(RootFile(kCrosshairAssets[0].fileName), *patch)
        : CrosshairAssetState::Unknown;
}

std::wstring CrosshairState()
{
    const CrosshairAssetState state = DetectCrosshairState();
    if (state == CrosshairAssetState::Patched) {
        return L"Hidden";
    }
    if (state == CrosshairAssetState::Original) {
        return L"Visible";
    }
    return L"Unknown";
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

bool ApplyCompatibilitySettings(bool torsoFixEnabled)
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
        SetDgVoodooString(
            conf,
            L"DirectXExt",
            L"RTTexturesForceScaleAndMSAA",
            torsoFixEnabled ? L"true" : L"false");
    }
    return wroteAny;
}

bool SetCrosshairVisible(bool visible)
{
    if (!ApplyCrosshairAssetPatches(!visible)) {
        return false;
    }

    SetApplyProgressStep(L"Saving the crosshair preference...");
    const std::wstring ini = RootFile(L"HotDPostFXFix.ini");
    if (!WriteIniString(ini, L"PostFXFix", L"HideCrosshair", visible ? L"0" : L"1")) {
        ShowError(L"Could not save the crosshair setting.");
        return false;
    }
    CompleteApplyProgressStep();

    AppendStatus(
        visible
            ? L"Crosshair visible. Original reticule data restored."
            : L"Crosshair hidden. Reticule data removed from the compressed level assets.");
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

bool TorsoFixState()
{
    const std::vector<std::wstring> configs = {
        RootFile(L"dgVoodoo.conf"),
        JoinPath(JoinPath(g_gameDir, L"dgVoodooBackend"), L"dgVoodoo.conf")
    };
    for (const std::wstring& conf : configs) {
        if (!FileExists(conf)) {
            continue;
        }
        const std::wstring value = ReadIniString(
            conf,
            L"DirectXExt",
            L"RTTexturesForceScaleAndMSAA",
            L"missing");
        if (value != L"missing") {
            return _wcsicmp(value.c_str(), L"false") != 0 &&
                value != L"0";
        }
    }
    return true;
}

void RefreshStatus()
{
    const int scale = CurrentResolutionScale();
    const std::wstring crosshairState = CrosshairState();
    const bool postFxEnabled = PostFxState() == L"ON";
    const bool cutsceneDofEnabled = CutsceneFxState() == L"ON";
    const bool torsoFixEnabled = TorsoFixState();

    std::wstring status = IsGameRunning() ? L"Game is running. Close it before applying changes." :
                                            L"Ready. Changes are applied only when you press Apply Changes.";
    status += L"\r\nInstalled: ";
    status += scale > 0 ? std::to_wstring(scale) + L"x" : L"resolution off";
    status += L" | crosshair " + crosshairState;
    status += postFxEnabled ? L" | FX on" : L" | FX off";
    status += cutsceneDofEnabled ? L" | DoF on" : L" | DoF off";
    status += torsoFixEnabled ? L" | torso fix on" : L" | torso fix off";
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
    const CrosshairAssetState crosshairState = DetectCrosshairState();
    settings.crosshairVisible = crosshairState == CrosshairAssetState::Unknown
        ? ReadIniString(
            RootFile(L"HotDPostFXFix.ini"),
            L"PostFXFix",
            L"HideCrosshair",
            L"0") != L"1"
        : crosshairState == CrosshairAssetState::Original;
    settings.postFxEnabled = PostFxState() == L"ON";
    settings.cutsceneDofEnabled = CutsceneFxState() == L"ON";
    settings.torsoFixEnabled = TorsoFixState();
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
    settings.torsoFixEnabled =
        SendMessageW(g_torsoFixCombo, CB_GETCURSEL, 0, 0) == 0;
    return settings;
}

bool SettingsEqual(const Settings& left, const Settings& right)
{
    return left.resolutionScale == right.resolutionScale &&
        left.crosshairVisible == right.crosshairVisible &&
        left.postFxEnabled == right.postFxEnabled &&
        left.cutsceneDofEnabled == right.cutsceneDofEnabled &&
        left.torsoFixEnabled == right.torsoFixEnabled;
}

void UpdateCutsceneDofLabel()
{
    if (!g_resolutionCombo || !g_postFxCombo || !g_cutsceneDofCombo) {
        return;
    }

    const int resolutionScale =
        static_cast<int>(SendMessageW(g_resolutionCombo, CB_GETCURSEL, 0, 0));
    const bool postFxEnabled =
        SendMessageW(g_postFxCombo, CB_GETCURSEL, 0, 0) == 0;
    const int dofSelection =
        static_cast<int>(SendMessageW(g_cutsceneDofCombo, CB_GETCURSEL, 0, 0));

    const wchar_t* onLabel = L"On";
    if (resolutionScale >= 2) {
        onLabel = postFxEnabled ? L"On (corrected)" : L"On (uncorrected)";
    }

    SendMessageW(g_cutsceneDofCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(
        g_cutsceneDofCombo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(onLabel));
    SendMessageW(
        g_cutsceneDofCombo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(L"Off"));
    SendMessageW(
        g_cutsceneDofCombo,
        CB_SETCURSEL,
        dofSelection >= 0 ? dofSelection : 0,
        0);
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
    if (g_progressText && g_progressBar && !g_applyInProgress) {
        SendMessageW(g_progressBar, PBM_SETSTATE, PBST_NORMAL, 0);
        SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);
        SetWindowTextW(
            g_progressText,
            dirty ? L"Changes are ready to apply." : L"No pending changes.");
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
    SendMessageW(g_torsoFixCombo, CB_SETCURSEL, g_appliedSettings.torsoFixEnabled ? 0 : 1, 0);
    UpdateCutsceneDofLabel();
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
    const bool resolutionChanged =
        desired.resolutionScale != g_appliedSettings.resolutionScale;
    const bool crosshairChanged =
        desired.crosshairVisible != g_appliedSettings.crosshairVisible;
    const bool postFxChanged =
        desired.postFxEnabled != g_appliedSettings.postFxEnabled;
    const bool cutsceneDofChanged =
        desired.cutsceneDofEnabled != g_appliedSettings.cutsceneDofEnabled;
    const bool torsoFixChanged =
        desired.torsoFixEnabled != g_appliedSettings.torsoFixEnabled;
    int progressSteps = 3;
    progressSteps += resolutionChanged ? 1 : 0;
    progressSteps += crosshairChanged
        ? static_cast<int>(ARRAYSIZE(kCrosshairAssets)) + 1
        : 0;
    progressSteps += postFxChanged ? 1 : 0;
    progressSteps += cutsceneDofChanged ? 1 : 0;

    SetStatus(L"Applying changes...");
    BeginApplyProgress(progressSteps);

    SetApplyProgressStep(L"Preparing the DirectX backend...");
    if (!EnsurePostFxBackend()) {
        goto apply_failed;
    }
    CompleteApplyProgressStep();

    SetApplyProgressStep(
        torsoFixChanged
            ? L"Updating the disappearing zombie torso fix..."
            : L"Applying dgVoodoo compatibility settings...");
    if (!ApplyCompatibilitySettings(desired.torsoFixEnabled)) {
        goto apply_failed;
    }
    CompleteApplyProgressStep();

    SetApplyProgressStep(L"Installing the HOTD runtime fix...");
    if (!InstallPostFxShim()) {
        goto apply_failed;
    }
    CompleteApplyProgressStep();

    if (resolutionChanged) {
        SetApplyProgressStep(L"Updating the rendering resolution...");
        if (!SetResolutionFix(desired.resolutionScale)) {
            goto apply_failed;
        }
        CompleteApplyProgressStep();
    }
    if (crosshairChanged) {
        SetReticleWarningVisible(true);
        if (!SetCrosshairVisible(desired.crosshairVisible)) {
            goto apply_failed;
        }
        SetReticleWarningVisible(false);
    }
    if (postFxChanged) {
        SetApplyProgressStep(L"Updating the post-processing fix...");
        if (!SetPostFxEnabled(desired.postFxEnabled)) {
            goto apply_failed;
        }
        CompleteApplyProgressStep();
    }
    if (cutsceneDofChanged) {
        SetApplyProgressStep(L"Updating cutscene depth of field...");
        if (!SetCutsceneEffectsEnabled(desired.cutsceneDofEnabled)) {
            goto apply_failed;
        }
        CompleteApplyProgressStep();
    }

    g_appliedSettings = ReadSettings();
    SetDirty(false);
    FinishApplyProgress(true);
    SetStatus(L"Changes applied successfully.\r\n"
              L"Close HOTD Tweaks and launch either mode through Steam.");
    return true;

apply_failed:
    g_appliedSettings = ReadSettings();
    SetDirty(!SettingsEqual(ReadUiSettings(), g_appliedSettings));
    FinishApplyProgress(false);
    SetStatus(L"Could not complete every change. Review the failed step below and try again.");
    return false;
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
    SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 24);
    SendMessageW(combo, CB_SETITEMHEIGHT, 0, 24);
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
    COLORREF color,
    int right = 840)
{
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT rect = {x, y, right, y + 32};
    DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
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
    RoundRect(dc, 28, 108, client.right - 28, 466, 16, 16);
    RoundRect(dc, 28, 484, client.right - 28, 660, 16, 16);
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
        {L"In-game crosshair", L"Compact reversible patches update only the reticule data.", 190},
        {L"Post-FX fix", L"Corrects high-resolution cutscene bloom and DoF alignment.", 254},
        {L"Cutscene depth of field", L"Keep depth of field enabled, or bypass the effect entirely.", 318},
        {L"Disappearing zombie torso fix", L"Prevents missing torso geometry at increased resolutions.", 382},
    };
    for (const Row& row : rows) {
        DrawTextLine(dc, row.title, 52, row.y, g_font, RGB(30, 41, 54), 500);
        DrawTextLine(dc, row.description, 52, row.y + 24, g_subtitleFont, RGB(101, 113, 128), 500);
    }

    DrawTextLine(
        dc,
        g_dirty ? L"Unsaved changes" : L"Configuration status",
        52,
        492,
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
    case WM_CREATE: {
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

        RECT client = {};
        GetClientRect(hwnd, &client);
        const int comboWidth = 300;
        const int comboX = client.right - 32 - comboWidth;
        const int statusWidth = client.right - 104;

        g_resolutionCombo = AddCombo(hwnd, IDC_RESOLUTION, comboX, 127, comboWidth);
        SendMessageW(g_resolutionCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Off"));
        for (int scale = 1; scale <= kMaxResolutionScale; ++scale) {
            const std::wstring label = ScaleLabel(scale);
            SendMessageW(g_resolutionCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }

        g_crosshairCombo = AddCombo(hwnd, IDC_CROSSHAIR, comboX, 191, comboWidth);
        AddOnOffItems(g_crosshairCombo, L"Visible", L"Hidden");
        g_postFxCombo = AddCombo(hwnd, IDC_POSTFX, comboX, 255, comboWidth);
        AddOnOffItems(g_postFxCombo, L"On", L"Off");
        g_cutsceneDofCombo = AddCombo(hwnd, IDC_CUTSCENE_DOF, comboX, 319, comboWidth);
        AddOnOffItems(g_cutsceneDofCombo, L"On", L"Off");
        g_torsoFixCombo = AddCombo(hwnd, IDC_TORSO_FIX, comboX, 383, comboWidth);
        AddOnOffItems(g_torsoFixCombo, L"On", L"Off");

        g_status = CreateWindowW(
            L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            52, 516, statusWidth, 44, hwnd,
            reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATUS)), nullptr, nullptr);
        SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_subtitleFont), TRUE);

        g_progressText = CreateWindowW(
            L"STATIC", L"No pending changes.", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            52, 568, statusWidth, 20, hwnd,
            reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_PROGRESS_TEXT)), nullptr, nullptr);
        SendMessageW(g_progressText, WM_SETFONT, reinterpret_cast<WPARAM>(g_subtitleFont), TRUE);

        g_reticleWarning = CreateWindowW(
            L"STATIC",
            L"Do not close HOTD Tweaks while reticle files are being updated.",
            WS_CHILD | SS_LEFTNOWORDWRAP,
            52,
            594,
            statusWidth,
            20,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RETICLE_WARNING)),
            nullptr,
            nullptr);
        SendMessageW(g_reticleWarning, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

        g_progressBar = CreateWindowW(
            PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            52, 624, statusWidth, 16, hwnd,
            reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_PROGRESS)), nullptr, nullptr);
        SendMessageW(g_progressBar, PBM_SETRANGE32, 0, 1);
        SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);
        SetWindowTheme(g_progressBar, L"Explorer", nullptr);

        g_refreshButton = AddButton(hwnd, IDC_REFRESH, L"Reload", 32, 676, 110, 40);
        g_openLogButton = AddButton(hwnd, IDC_OPEN_LOG, L"Open Log", 154, 676, 120, 40);
        g_applyButton = AddButton(
            hwnd,
            IDC_APPLY,
            L"Apply Changes",
            client.right - 32 - 176,
            676,
            176,
            40,
            true);

        if (!IsGameRunning()) {
            ApplyCompatibilitySettings(TorsoFixState());
        }
        LoadSettingsIntoUi();
        return 0;
    }

    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        SetTextColor(
            reinterpret_cast<HDC>(wparam),
            reinterpret_cast<HWND>(lparam) == g_reticleWarning
                ? RGB(185, 63, 45)
                : RGB(91, 103, 117));
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
            case IDC_TORSO_FIX:
                if (LOWORD(wparam) == IDC_RESOLUTION ||
                    LOWORD(wparam) == IDC_POSTFX) {
                    UpdateCutsceneDofLabel();
                }
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
        if (g_applyInProgress) {
            MessageBeep(MB_ICONINFORMATION);
            return 0;
        }
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
    g_gameDir = FindGameDirectory();

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
            return SetCrosshairVisible(crosshairVisible) ? 0 : 1;
        }
    }
    if (arguments) {
        LocalFree(arguments);
    }

    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls = {
        sizeof(controls),
        ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS
    };
    InitCommonControlsEx(&controls);

    const wchar_t* className = L"HOTD_Tweaks_Window";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(kAppIconResource),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(kAppIconResource),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = className;

    RegisterClassExW(&wc);

    g_mainWindow = CreateWindowExW(
        WS_EX_APPWINDOW,
        className,
        L"HOTD Tweaks",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        884,
        772,
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
