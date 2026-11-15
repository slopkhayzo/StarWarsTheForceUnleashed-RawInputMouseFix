// Star Wars: The Force Unleashed raw mouse input fix (ASI plugin, x86).
//
// The game reads the mouse once per frame through DirectInput 8 (immediate mode, raw counts).
// In keyboard-and-mouse mode Aspyr's action mapper turns the counts into a virtual right stick
// (counts per frame), and the gameplay camera turns that into one of six speed bands, eases the
// turn speed toward it per frame and applies a response curve: a joystick, not a mouse.
//
// The fix: the camera's speed bands see only the keyboard part of the virtual right stick; the
// mouse part turns the camera directly after the game's own turn (degrees = counts x
// sensitivity), and the game's camera collision prediction gets the matching turn speed.
// [Fix] DirectCamera / F8 switch between this and the game's own handling. Lock-on, Force Grip
// and the other pad-mapped axes are unchanged. Development builds (build-msvc.cmd dev) add a
// hardware-breakpoint trace of the code that reads the mouse values ([Trace] MouseReaders=1,
// F11 starts a new round).
// research\docs\swtfu-raw-mouse-input-fix-plan.md has the plan, swtfu-camera-path.md the path.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "swtfu_counts.h"
#include "swtfu_layout.h"

#ifndef SWTFU_RAW_MOUSE_VERSION
#define SWTFU_RAW_MOUSE_VERSION "1.0.0"
#endif

// Development tools (mouse reader trace): build-msvc.cmd dev defines SWTFU_DEV_TOOLS=1.
#ifndef SWTFU_DEV_TOOLS
#define SWTFU_DEV_TOOLS 0
#endif

namespace {

constexpr char kLogFilename[] = "swtfu-raw-mouse-fix.log";
constexpr char kConfigFilename[] = "swtfu-raw-mouse-fix.ini";
constexpr char kFallbackDirectory[] = "SWTFURawMouseFix";
constexpr LONGLONG kLogRotateBytes = 4 * 1024 * 1024;
constexpr DWORD kReportIntervalMs = 5000;
constexpr DWORD kCodeReadyTimeoutMs = 120000;
constexpr DWORD kCodeReadyPollMs = 50;
constexpr DWORD kStaleCountsMs = 250;
constexpr float kDefaultSensitivity = 50.0f;
constexpr float kMinSensitivity = 0.1f;
constexpr float kMaxSensitivity = 1000.0f;
constexpr float kMinMultiplier = 0.05f;
constexpr float kMaxMultiplier = 20.0f;
constexpr float kHotkeyMultiplierStep = 1.1f;

struct MouseState {  // DIMOUSESTATE2
    LONG x;
    LONG y;
    LONG z;
    BYTE buttons[8];
};

using PollMouseFn = int(__thiscall*)(void* input);
using MouseHandlerFn = int(__thiscall*)(void* input, float x, float y, float z, BYTE* buttons,
                                        int count);

HANDLE g_instanceMutex = nullptr;
volatile LONG g_initializationStarted = 0;
PollMouseFn g_originalPollMouse = nullptr;
MouseHandlerFn g_originalMouseHandler = nullptr;

// Configuration ([Fix], [Sensitivity], [Hotkeys]).
bool g_enabled = true;
bool g_diagnostics = false;
bool g_hotkeys = true;
volatile LONG g_directCamera = 1;
std::atomic<float> g_sensitivityX{kDefaultSensitivity};
std::atomic<float> g_sensitivityY{kDefaultSensitivity};
std::atomic<float> g_sensitivityMultiplier{1.0f};
int g_keyReload = VK_F6;
int g_keyToggle = VK_F8;
int g_keyMultiplierDown = VK_F9;
int g_keyMultiplierUp = VK_F10;
int g_keyTraceRound = VK_F11;

long long g_staleTicks = 0;  // mouse counts older than this are not applied (menus, pauses)

bool g_traceMouseReaders = false;
DWORD g_traceSeconds = 180;
std::uint32_t g_traceExtraAddress = 0;

// Poll statistics: written by the game's input thread, read and reset by the report thread.
std::atomic<std::uint32_t> g_polls{0};
std::atomic<std::uint32_t> g_failedPolls{0};  // GetDeviceState failed (e.g. not foreground)
std::atomic<std::uint32_t> g_movingPolls{0};
std::atomic<long long> g_absX{0};
std::atomic<long long> g_absY{0};
std::atomic<long long> g_sumX{0};
std::atomic<long long> g_sumY{0};
std::atomic<long> g_maxX{0};
std::atomic<long> g_maxY{0};
std::atomic<long long> g_intervalSumTicks{0};
std::atomic<long long> g_intervalMaxTicks{0};
std::atomic<long long> g_intervalMinTicks{LLONG_MAX};
long long g_lastPollTicks = 0;
// Handler statistics: what the game's own mouse handler received.
std::atomic<std::uint32_t> g_handlerCalls{0};
std::atomic<std::uint32_t> g_handlerMismatch{0};  // floats differ from the polled counts
std::atomic<long long> g_handlerAbsX{0};
std::atomic<long long> g_handlerAbsY{0};
std::atomic<std::uint32_t> g_handlerMode{0xFFFFFFFF};
std::atomic<void*> g_input{nullptr};
std::atomic<std::uint32_t> g_inputThread{0};
LARGE_INTEGER g_qpcFrequency{};

// ---------------------------------------------------------------------------------------
// Files and logging

// GetModuleFileName takes the loader lock, which the DRM start-up can hold for a long time.
// Both paths are therefore read once in DllMain (which already holds that lock).
char g_pluginDirectory[MAX_PATH]{};  // with a trailing backslash; empty if unknown
char g_hostPath[MAX_PATH]{};

void CaptureModulePaths(HMODULE self) {
    char path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = std::strrchr(path, '\\');
    if (length != 0 && length < MAX_PATH && slash != nullptr) {
        slash[1] = '\0';
        strcpy_s(g_pluginDirectory, path);
    }
    const DWORD hostLength = GetModuleFileNameA(nullptr, g_hostPath, MAX_PATH);
    if (hostLength == 0 || hostLength >= MAX_PATH) g_hostPath[0] = '\0';
}

bool BuildSiblingPath(const char* filename, char (&path)[MAX_PATH]) {
    if (g_pluginDirectory[0] == '\0') return false;
    strcpy_s(path, g_pluginDirectory);
    return strcat_s(path, filename) == 0;
}

bool BuildFallbackPath(const char* filename, char (&path)[MAX_PATH]) {
    char directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 32) return false;
    strcat_s(directory, "\\");
    strcat_s(directory, kFallbackDirectory);
    if (!CreateDirectoryA(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    strcpy_s(path, directory);
    strcat_s(path, "\\");
    return strcat_s(path, filename) == 0;
}

void RotateLogIfLarge(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return;
    const LONGLONG size = (static_cast<LONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (size < kLogRotateBytes) return;
    char old[MAX_PATH]{};
    strcpy_s(old, path);
    strcat_s(old, ".old");
    MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);
}

CRITICAL_SECTION g_logLock;

bool OpenLogFile(FILE*& file) {
    static bool rotated = false;
    char path[MAX_PATH]{};
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool built = attempt == 0 ? BuildSiblingPath(kLogFilename, path)
                                        : BuildFallbackPath(kLogFilename, path);
        if (!built) continue;
        if (!rotated) RotateLogIfLarge(path);
        if (fopen_s(&file, path, "a") == 0 && file != nullptr) {
            rotated = true;
            return true;
        }
    }
    return false;
}

void Log(const char* format, ...) {
    EnterCriticalSection(&g_logLock);
    FILE* file = nullptr;
    if (OpenLogFile(file)) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        std::fprintf(file, "%02u:%02u:%02u.%03u [%lu] ", time.wHour, time.wMinute, time.wSecond,
                     time.wMilliseconds, GetCurrentProcessId());
        va_list arguments;
        va_start(arguments, format);
        std::vfprintf(file, format, arguments);
        va_end(arguments);
        std::fputc('\n', file);
        std::fclose(file);
    }
    LeaveCriticalSection(&g_logLock);
}

// ---------------------------------------------------------------------------------------
// Configuration

float ReadPositiveFloat(const char* path, const char* section, const char* key, float fallback,
                        float low, float high) {
    char text[32]{};
    GetPrivateProfileStringA(section, key, "", text, sizeof(text), path);
    if (text[0] == '\0') return fallback;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    while (end != nullptr && (*end == ' ' || *end == '\t')) ++end;
    if (end == text || end == nullptr || *end != '\0' || !(value > 0.0)) {
        Log("config: %s=%s is not a positive number; using %.2f", key, text, fallback);
        return fallback;
    }
    const float limited = std::clamp(static_cast<float>(value), low, high);
    if (limited != static_cast<float>(value)) {
        Log("config: %s=%s is outside %.2f..%.2f; using %.2f", key, text, low, high, limited);
    }
    return limited;
}

// Key names: F1..F24, or a virtual-key code in hex (0x75) or decimal. "none" disables the key.
int ReadKey(const char* path, const char* key, int fallback) {
    char text[32]{};
    GetPrivateProfileStringA("Hotkeys", key, "", text, sizeof(text), path);
    if (text[0] == '\0') return fallback;
    if (_stricmp(text, "none") == 0) return 0;
    if ((text[0] == 'F' || text[0] == 'f') && text[1] >= '1' && text[1] <= '9') {
        const int number = std::atoi(text + 1);
        if (number >= 1 && number <= 24) return VK_F1 + number - 1;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 0);
    if (end != text && value > 0 && value < 256) return static_cast<int>(value);
    Log("config: [Hotkeys] %s=%s is not a key; using the default", key, text);
    return fallback;
}

void LoadConfig() {
    char path[MAX_PATH]{};
    if (!BuildSiblingPath(kConfigFilename, path) ||
        GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        Log("config: %s not found beside the plugin; using defaults", kConfigFilename);
        return;
    }
    g_enabled = GetPrivateProfileIntA("Fix", "Enabled", 1, path) != 0;
    g_diagnostics = GetPrivateProfileIntA("Fix", "Diagnostics", 0, path) != 0;
    g_hotkeys = GetPrivateProfileIntA("Fix", "Hotkeys", 1, path) != 0;
    InterlockedExchange(&g_directCamera, GetPrivateProfileIntA("Fix", "DirectCamera", 1, path) != 0 ? 1 : 0);
    g_sensitivityX.store(ReadPositiveFloat(path, "Sensitivity", "SensitivityX", kDefaultSensitivity,
                                           kMinSensitivity, kMaxSensitivity));
    g_sensitivityY.store(ReadPositiveFloat(path, "Sensitivity", "SensitivityY", kDefaultSensitivity,
                                           kMinSensitivity, kMaxSensitivity));
    g_sensitivityMultiplier.store(ReadPositiveFloat(path, "Sensitivity", "SensitivityMultiplier", 1.0f,
                                                    kMinMultiplier, kMaxMultiplier));
    g_keyReload = ReadKey(path, "ReloadIni", VK_F6);
    g_keyToggle = ReadKey(path, "ToggleFix", VK_F8);
    g_keyMultiplierDown = ReadKey(path, "MultiplierDown", VK_F9);
    g_keyMultiplierUp = ReadKey(path, "MultiplierUp", VK_F10);
    Log("config: Enabled=%d DirectCamera=%ld SensitivityX=%.2f SensitivityY=%.2f "
        "SensitivityMultiplier=%.2f Hotkeys=%d (reload %02X toggle %02X down %02X up %02X) Diagnostics=%d",
        g_enabled ? 1 : 0, g_directCamera, g_sensitivityX.load(), g_sensitivityY.load(),
        g_sensitivityMultiplier.load(), g_hotkeys ? 1 : 0, g_keyReload, g_keyToggle,
        g_keyMultiplierDown, g_keyMultiplierUp, g_diagnostics ? 1 : 0);
#if SWTFU_DEV_TOOLS
    char extra[32]{};
    g_traceMouseReaders = GetPrivateProfileIntA("Trace", "MouseReaders", 0, path) != 0;
    g_traceSeconds = GetPrivateProfileIntA("Trace", "Seconds", 180, path);
    g_keyTraceRound = ReadKey(path, "TraceRound", VK_F11);
    GetPrivateProfileStringA("Trace", "ExtraAddress", "", extra, sizeof(extra), path);
    g_traceExtraAddress = static_cast<std::uint32_t>(std::strtoul(extra, nullptr, 16));
    if (g_traceMouseReaders) {
        Log("config: Trace MouseReaders=1 Seconds=%lu ExtraAddress=%08X", g_traceSeconds,
            static_cast<unsigned>(g_traceExtraAddress));
    }
#endif
}

// ---------------------------------------------------------------------------------------
// Executable identification and layout validation

bool HostIsGame() {
    if (g_hostPath[0] == '\0') return false;
    const char* name = std::strrchr(g_hostPath, '\\');
    name = name != nullptr ? name + 1 : g_hostPath;
    return _stricmp(name, swtfu::kHostExecutable) == 0;
}

bool ReadOwnMemory(std::uint32_t va, void* destination, std::size_t length) {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(va), destination,
                             length, &bytesRead) != FALSE &&
           bytesRead == length;
}

bool InspectImage(char* failure, std::size_t failureSize) {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    if (base != swtfu::kImageBase) {
        std::snprintf(failure, failureSize, "image base %08X, expected %08X",
                      static_cast<unsigned>(base), static_cast<unsigned>(swtfu::kImageBase));
        return false;
    }
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!ReadOwnMemory(swtfu::kImageBase, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !ReadOwnMemory(swtfu::kImageBase + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        std::snprintf(failure, failureSize, "not an x86 PE image");
        return false;
    }
    if (nt.OptionalHeader.SizeOfImage < swtfu::kMinSizeOfImage) {
        std::snprintf(failure, failureSize, "image size %08lX, at least %08X expected",
                      nt.OptionalHeader.SizeOfImage, static_cast<unsigned>(swtfu::kMinSizeOfImage));
        return false;
    }
    const char* layout = "unknown layout, signatures decide";
    if (nt.FileHeader.TimeDateStamp == swtfu::kTimeDateStamp) {
        if (nt.OptionalHeader.SizeOfImage == swtfu::kSteamSizeOfImage) layout = "Steam build (SteamStub)";
        if (nt.OptionalHeader.SizeOfImage == swtfu::kMinSizeOfImage) layout = "Steam build without SteamStub";
    }
    Log("executable: timestamp=%08lX image_size=%08lX LAA=%d (%s)", nt.FileHeader.TimeDateStamp,
        nt.OptionalHeader.SizeOfImage,
        (nt.FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0 ? 1 : 0, layout);
    return true;
}

// SteamStub decrypts the game code after the process starts, after the loader has loaded this
// plugin. Until then the signatures cannot match, so they are checked again until they do or
// the time runs out. A foreign patch does not go away by waiting: that fails at once.
bool WaitForValidLayout(char* failure, std::size_t failureSize) {
    auto reader = [](std::uint32_t va, void* destination, std::size_t length) {
        return ReadOwnMemory(va, destination, length);
    };
    if (swtfu::ValidateLayout(reader, failure, failureSize)) return true;
    Log("game code not ready (%s); checking again for up to %lu s (SteamStub decrypts it at startup)",
        failure, kCodeReadyTimeoutMs / 1000);
    const DWORD started = GetTickCount();
    while (GetTickCount() - started < kCodeReadyTimeoutMs) {
        Sleep(kCodeReadyPollMs);
        if (swtfu::ValidateLayout(reader, failure, failureSize)) {
            Log("game code ready after %lu ms", GetTickCount() - started);
            return true;
        }
        if (std::strncmp(failure, "foreign patch", 13) == 0) return false;
    }
    return false;
}

bool HashFileSha256(const char* path, char (&hex)[65]) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool ok = false;
    std::uint8_t digest[32]{};
    static std::uint8_t buffer[1 << 20];
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            ok = true;
            for (;;) {
                DWORD read = 0;
                if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
                    ok = false;
                    break;
                }
                if (read == 0) break;
                if (BCryptHashData(hash, buffer, read, 0) != 0) {
                    ok = false;
                    break;
                }
            }
            ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
        }
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;
    for (int index = 0; index < 32; ++index) std::snprintf(hex + index * 2, 3, "%02X", digest[index]);
    return true;
}

bool EndsWithInsensitive(const wchar_t* text, const wchar_t* suffix) {
    const std::size_t length = std::wcslen(text);
    const std::size_t suffixLength = std::wcslen(suffix);
    return length >= suffixLength && _wcsicmp(text + length - suffixLength, suffix) == 0;
}

// Names the build and the plugins loaded beside this one. Both need the loader lock, which the
// DRM start-up can hold, so this runs on its own thread after the hooks are in place.
DWORD WINAPI LogEnvironmentThread(LPVOID) {
    char hex[65]{};
    if (g_hostPath[0] != '\0' && HashFileSha256(g_hostPath, hex)) {
        const char* name = swtfu::KnownBuildName(hex);
        Log("executable SHA-256: %s (%s)", hex, name != nullptr ? name : "unknown build");
    } else {
        Log("executable SHA-256: unavailable");
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        // Other plugins and proxy DLLs in the game folder (names only).
        const bool plugin = EndsWithInsensitive(entry.szModule, L".asi");
        const bool proxy = _wcsicmp(entry.szModule, L"dinput8.dll") == 0 ||
                           _wcsicmp(entry.szModule, L"winmm.dll") == 0 ||
                           _wcsicmp(entry.szModule, L"version.dll") == 0 ||
                           _wcsicmp(entry.szModule, L"d3d9.dll") == 0;
        if (plugin || proxy) Log("module: %ls (%ls)", entry.szModule, entry.szExePath);
    }
    CloseHandle(snapshot);
    return 0;
}

bool AcquireInstanceGuard() {
    char name[64]{};
    std::snprintf(name, sizeof(name), "Local\\SWTFURawMouseFix-%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexA(nullptr, FALSE, name);
    if (mutex == nullptr) {
        Log("initialization failed: instance guard error=%lu", GetLastError());
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        Log("initialization refused: another copy of the fix is already loaded");
        return false;
    }
    g_instanceMutex = mutex;
    return true;
}

bool GameHasForeground() {
    DWORD processId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &processId);
    return processId == GetCurrentProcessId();
}

// ---------------------------------------------------------------------------------------
// Passive mouse observation

void StoreMax(std::atomic<long>& slot, long value) {
    if (value > slot.load(std::memory_order_relaxed)) slot.store(value, std::memory_order_relaxed);
}

int __fastcall HookPollMouse(void* input, void* /*edx*/) {
    const int result = g_originalPollMouse(input);
    auto* bytes = static_cast<std::uint8_t*>(input);
    g_input.store(input, std::memory_order_relaxed);
    g_inputThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (bytes[swtfu::kInputMouseAcquired] == 0) {
        g_failedPolls.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const auto& state = *reinterpret_cast<const MouseState*>(bytes + swtfu::kInputMouseState);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_lastPollTicks != 0) {
        const long long interval = now.QuadPart - g_lastPollTicks;
        g_intervalSumTicks.fetch_add(interval, std::memory_order_relaxed);
        if (interval > g_intervalMaxTicks.load(std::memory_order_relaxed)) g_intervalMaxTicks.store(interval);
        if (interval < g_intervalMinTicks.load(std::memory_order_relaxed)) g_intervalMinTicks.store(interval);
    }
    g_lastPollTicks = now.QuadPart;
    g_polls.fetch_add(1, std::memory_order_relaxed);
    if (state.x == 0 && state.y == 0) return result;
    const long ax = std::labs(state.x);
    const long ay = std::labs(state.y);
    g_movingPolls.fetch_add(1, std::memory_order_relaxed);
    g_absX.fetch_add(ax, std::memory_order_relaxed);
    g_absY.fetch_add(ay, std::memory_order_relaxed);
    g_sumX.fetch_add(state.x, std::memory_order_relaxed);
    g_sumY.fetch_add(state.y, std::memory_order_relaxed);
    StoreMax(g_maxX, ax);
    StoreMax(g_maxY, ay);
    return result;
}

int __fastcall HookMouseHandler(void* input, void* /*edx*/, float x, float y, float z, BYTE* buttons,
                                int count) {
    auto* bytes = static_cast<std::uint8_t*>(input);
    const auto& state = *reinterpret_cast<const MouseState*>(bytes + swtfu::kInputMouseState);
    g_handlerCalls.fetch_add(1, std::memory_order_relaxed);
    g_handlerMode.store(*reinterpret_cast<std::uint32_t*>(bytes + swtfu::kInputWindowMode),
                        std::memory_order_relaxed);
    if (x != static_cast<float>(state.x) || y != static_cast<float>(state.y)) {
        g_handlerMismatch.fetch_add(1, std::memory_order_relaxed);
    }
    g_handlerAbsX.fetch_add(static_cast<long long>(std::fabs(x)), std::memory_order_relaxed);
    g_handlerAbsY.fetch_add(static_cast<long long>(std::fabs(y)), std::memory_order_relaxed);
    return g_originalMouseHandler(input, x, y, z, buttons, count);
}

// ---------------------------------------------------------------------------------------
// Hooking

constexpr std::size_t kMaxThreads = 512;

struct SuspendedThreads {
    HANDLE handles[kMaxThreads]{};
    std::size_t count = 0;
};

void SuspendOtherThreads(SuspendedThreads& threads) {
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        if (threads.count == kMaxThreads) break;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                   FALSE, entry.th32ThreadID);
        if (thread == nullptr) continue;
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            CloseHandle(thread);
            continue;
        }
        threads.handles[threads.count++] = thread;
    }
    CloseHandle(snapshot);
}

void ResumeThreads(SuspendedThreads& threads) {
    for (std::size_t index = 0; index < threads.count; ++index) {
        ResumeThread(threads.handles[index]);
        CloseHandle(threads.handles[index]);
    }
    threads.count = 0;
}

// A thread suspended inside the bytes being replaced is moved to the same instruction in the
// trampoline, which holds an exact copy of those whole instructions.
void MoveThreadsOutOfPatch(SuspendedThreads& threads, std::uintptr_t target, std::size_t length,
                           std::uintptr_t trampoline) {
    for (std::size_t index = 0; index < threads.count; ++index) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(threads.handles[index], &context)) continue;
        if (context.Eip >= target && context.Eip < target + length) {
            context.Eip = static_cast<DWORD>(trampoline + (context.Eip - target));
            SetThreadContext(threads.handles[index], &context);
            Log("hook: moved a thread out of the patched bytes");
        }
    }
}

// Our patched bytes, checked again every few seconds: another plugin that loads after us and
// writes over a hook site would otherwise go unnoticed (the signatures are checked only before
// our own hooks are written).
struct PatchRecord {
    std::uint32_t va;
    std::uint8_t bytes[5];
    const char* name;
    bool reported;
};
PatchRecord g_patches[8]{};
std::size_t g_patchCount = 0;

void RememberPatch(std::uint32_t va, const char* name) {
    if (g_patchCount == sizeof(g_patches) / sizeof(g_patches[0])) return;
    PatchRecord& record = g_patches[g_patchCount++];
    record.va = va;
    record.name = name;
    std::memcpy(record.bytes, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va)), 5);
}

void CheckPatches() {
    for (std::size_t index = 0; index < g_patchCount; ++index) {
        PatchRecord& record = g_patches[index];
        std::uint8_t now[5]{};
        if (!ReadOwnMemory(record.va, now, sizeof(now))) continue;
        const bool intact = std::memcmp(now, record.bytes, sizeof(now)) == 0;
        if (!intact && !record.reported) {
            Log("hook changed by someone else: %s at %08X now %02X %02X %02X %02X %02X (ours %02X %02X "
                "%02X %02X %02X); another plugin patched it after us",
                record.name, static_cast<unsigned>(record.va), now[0], now[1], now[2], now[3], now[4],
                record.bytes[0], record.bytes[1], record.bytes[2], record.bytes[3], record.bytes[4]);
        }
        if (intact && record.reported) {
            Log("hook restored: %s at %08X", record.name, static_cast<unsigned>(record.va));
        }
        record.reported = !intact;
    }
}

// Detours `length` bytes of whole, position-independent instructions at `va` to `replacement`.
// The signature check right before guarantees the bytes are the game's own.
template <typename Function>
bool InstallDetour(std::uint32_t va, std::size_t length, const void* replacement,
                   Function& original, const char* name) {
    auto* target = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(va));
    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, length + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) {
        Log("hook failed: %s trampoline allocation error=%lu", name, GetLastError());
        return false;
    }
    SuspendedThreads threads;
    SuspendOtherThreads(threads);
    std::memcpy(trampoline, target, length);
    trampoline[length] = 0xE9;
    *reinterpret_cast<std::int32_t*>(trampoline + length + 1) =
        static_cast<std::int32_t>((target + length) - (trampoline + length + 5));
    FlushInstructionCache(GetCurrentProcess(), trampoline, length + 5);
    DWORD oldProtection = 0;
    bool ok = VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &oldProtection) != FALSE;
    if (ok) {
        MoveThreadsOutOfPatch(threads, va, length, reinterpret_cast<std::uintptr_t>(trampoline));
        original = reinterpret_cast<Function>(trampoline);
        target[0] = 0xE9;
        *reinterpret_cast<std::int32_t*>(target + 1) = static_cast<std::int32_t>(
            reinterpret_cast<const std::uint8_t*>(replacement) - (target + 5));
        for (std::size_t index = 5; index < length; ++index) target[index] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), target, length);
        DWORD ignored = 0;
        VirtualProtect(target, length, oldProtection, &ignored);
    }
    ResumeThreads(threads);
    if (!ok) {
        Log("hook failed: %s VirtualProtect error=%lu", name, GetLastError());
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    RememberPatch(va, name);
    Log("hook installed: %s at %08X", name, static_cast<unsigned>(va));
    return true;
}

// Points the call instruction at `va` (E8 rel32, currently calling `expected`) at `replacement`.
bool PatchCall(std::uint32_t va, std::uint32_t expected, const void* replacement, const char* name) {
    auto* site = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(va));
    const std::uint32_t current = va + 5 + *reinterpret_cast<const std::int32_t*>(site + 1);
    if (site[0] != 0xE8 || current != expected) {
        Log("hook failed: %s at %08X calls %08X, expected %08X", name, static_cast<unsigned>(va),
            static_cast<unsigned>(current), static_cast<unsigned>(expected));
        return false;
    }
    SuspendedThreads threads;
    SuspendOtherThreads(threads);
    DWORD oldProtection = 0;
    const bool ok = VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtection) != FALSE;
    if (ok) {
        *reinterpret_cast<std::int32_t*>(site + 1) = static_cast<std::int32_t>(
            reinterpret_cast<const std::uint8_t*>(replacement) - (site + 5));
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        DWORD ignored = 0;
        VirtualProtect(site, 5, oldProtection, &ignored);
    }
    ResumeThreads(threads);
    if (!ok) {
        Log("hook failed: %s VirtualProtect error=%lu", name, GetLastError());
        return false;
    }
    RememberPatch(va, name);
    Log("hook installed: %s at %08X", name, static_cast<unsigned>(va));
    return true;
}

// ---------------------------------------------------------------------------------------
// Direct mouse camera
//
// After every mapper evaluation the virtual right stick (counts per frame, with the game's
// invert and binding signs) is evaluated again by the game's own axis function with the mouse
// half-axes zeroed: that is the keyboard part, the rest is the mouse part. The camera's speed
// bands then read only the keyboard part, and after the game's own turn the mouse part is added
// to the camera's orbit and pitch: degrees = counts x sensitivity / 1000, no bands, no easing.
//
// The game was built with whole-program optimization: some callers assume registers survive
// calls that the standard convention does not promise (0x006B8E1F reuses ecx). Every entry
// from the game therefore goes through a thunk that keeps all general and xmm registers.

using AxisEvalFn = float(__thiscall*)(void* mapper, const void* positive, const void* negative);
using ActiveCameraFn = std::uint8_t*(__thiscall*)(void* player);
using WrapAngleFn = float(__cdecl*)(float degrees);
using SetAngleFn = void(__thiscall*)(void* setter, float degrees, void* angles);

constexpr float kDegreesPerCountPerSensitivity = 0.001f;

void* g_originalMapperEval = nullptr;
void* g_originalCameraTurn = nullptr;
bool g_cameraHooks = false;  // all camera hooks in place

swtfu::PendingCounts g_mouseStick;  // mouse part of the right stick, not yet applied
std::atomic<float> g_keyboardStickX{0.0f};
std::atomic<float> g_keyboardStickY{0.0f};
std::atomic<bool> g_keyboardStickValid{false};
std::atomic<std::uint8_t*> g_player{nullptr};
std::atomic<std::uint8_t*> g_camera{nullptr};

// Camera statistics, reset by the report thread.
std::atomic<std::uint32_t> g_mapperEvals{0};
std::atomic<std::uint32_t> g_mapperCleared{0};   // the mapper zeroed its sticks this frame
std::atomic<std::uint32_t> g_keyboardFrames{0};  // keys bound to the right stick were held
std::atomic<std::uint32_t> g_turnCalls{0};
std::atomic<std::uint32_t> g_turnMoving{0};      // with mouse counts
std::atomic<std::uint32_t> g_turnNotDriving{0};  // counts dropped: the player does not drive the camera
std::atomic<std::uint32_t> g_turnStale{0};       // counts dropped: too old
std::atomic<long long> g_turnCountsX{0};
std::atomic<long long> g_turnCountsY{0};
std::atomic<long long> g_turnOrbitMicro{0};  // |orbit change| in micro-degrees
std::atomic<long long> g_turnPitchMicro{0};
std::atomic<std::uint32_t> g_sameSignX{0};  // orbit moved with the sign of the counts
std::atomic<std::uint32_t> g_oppositeSignX{0};
std::atomic<std::uint32_t> g_sameSignY{0};
std::atomic<std::uint32_t> g_oppositeSignY{0};
// What other code did to the angles between one camera turn and the next (pitch limits).
std::atomic<std::uint32_t> g_pitchChangedOutside{0};
std::atomic<std::uint32_t> g_orbitChangedOutside{0};
std::atomic<float> g_outsidePitchWritten{0.0f};
std::atomic<float> g_outsidePitchFound{0.0f};
std::atomic<float> g_pitchMin{1e9f};
std::atomic<float> g_pitchMax{-1e9f};

bool DirectCameraActive() {
    return g_cameraHooks && InterlockedCompareExchange(&g_directCamera, 0, 0) != 0;
}

std::uint8_t* GameInput() {
    auto* globals = *reinterpret_cast<std::uint8_t**>(static_cast<std::uintptr_t>(swtfu::kGameGlobals));
    return globals != nullptr ? *reinterpret_cast<std::uint8_t**>(globals) : nullptr;
}

std::uint8_t* MapperOf(std::uint8_t* input) {
    return input != nullptr ? *reinterpret_cast<std::uint8_t**>(input + swtfu::kInputMapper) : nullptr;
}

bool KeyboardMouseMode() {
    std::uint8_t* mapper = MapperOf(GameInput());
    if (mapper == nullptr) return false;
    const auto* settings = *reinterpret_cast<std::uint8_t**>(mapper + swtfu::kMapperSettings);
    return settings != nullptr && settings[0] != 0;
}

float& FloatAt(std::uint8_t* base, std::uint32_t offset) {
    return *reinterpret_cast<float*>(base + offset);
}

long long Micro(float degrees) { return static_cast<long long>(std::fabs(degrees) * 1e6f); }

float WrappedDifference(float after, float before) {
    float difference = after - before;
    if (difference > 180.0f) difference -= 360.0f;
    if (difference < -180.0f) difference += 360.0f;
    return difference;
}

void CountSign(long long counts, float change, std::atomic<std::uint32_t>& same,
               std::atomic<std::uint32_t>& opposite) {
    if (counts == 0 || change == 0.0f) return;
    ((counts > 0) == (change > 0.0f) ? same : opposite).fetch_add(1, std::memory_order_relaxed);
}

// Game thread, after the game's mapper evaluation (keyboard-and-mouse mode only).
void __cdecl AfterMapperEval(std::uint8_t* mapper) {
    g_mapperEvals.fetch_add(1, std::memory_order_relaxed);
    auto* bindings = *reinterpret_cast<std::uint8_t**>(mapper + swtfu::kMapperBindings);
    if (bindings == nullptr) return;
    const auto evaluate = reinterpret_cast<AxisEvalFn>(static_cast<std::uintptr_t>(swtfu::kAxisEvalVa));
    const float fullX = evaluate(mapper, bindings + swtfu::kBindRightXPositive,
                                 bindings + swtfu::kBindRightXNegative);
    const float fullY = evaluate(mapper, bindings + swtfu::kBindRightYPositive,
                                 bindings + swtfu::kBindRightYNegative);
    float* halves = reinterpret_cast<float*>(mapper + swtfu::kMapperMouseHalfAxes);
    float saved[4];
    std::memcpy(saved, halves, sizeof(saved));
    std::memset(halves, 0, sizeof(saved));
    const float keyX = evaluate(mapper, bindings + swtfu::kBindRightXPositive,
                                bindings + swtfu::kBindRightXNegative);
    const float keyY = evaluate(mapper, bindings + swtfu::kBindRightYPositive,
                                bindings + swtfu::kBindRightYNegative);
    std::memcpy(halves, saved, sizeof(saved));

    // The mapper zeroes its sticks for a frame when its mode changes: nothing to apply then.
    if (FloatAt(mapper, swtfu::kMapperRightStickX) != fullX ||
        FloatAt(mapper, swtfu::kMapperRightStickY) != fullY) {
        g_mapperCleared.fetch_add(1, std::memory_order_relaxed);
        g_keyboardStickX.store(0.0f);
        g_keyboardStickY.store(0.0f);
        g_keyboardStickValid.store(true);
        return;
    }
    g_keyboardStickX.store(keyX);
    g_keyboardStickY.store(keyY);
    g_keyboardStickValid.store(true);
    if (keyX != 0.0f || keyY != 0.0f) g_keyboardFrames.fetch_add(1, std::memory_order_relaxed);
    const long mouseX = std::lround(fullX - keyX);
    const long mouseY = std::lround(fullY - keyY);
    if (mouseX != 0 || mouseY != 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        g_mouseStick.Add(mouseX, mouseY, now.QuadPart);
    }
}

// Game thread: replaces the right-stick accessor calls of the camera's speed-band readers.
float __cdecl CameraStick(std::uint8_t* input, int axis) {
    std::uint8_t* mapper = MapperOf(input);
    const float full = FloatAt(mapper, axis == 0 ? swtfu::kMapperRightStickX : swtfu::kMapperRightStickY);
    if (!DirectCameraActive() || !g_keyboardStickValid.load()) return full;
    return axis == 0 ? g_keyboardStickX.load() : g_keyboardStickY.load();
}

// Camera turn state of the current frame (game thread only).
struct TurnFrame {
    std::uint8_t* camera = nullptr;
    float orbit = 0.0f;
    float pitch = 0.0f;
};
TurnFrame g_frame;
std::uint8_t* g_writtenCamera = nullptr;  // angles written by the previous direct turn
float g_writtenOrbit = 0.0f;
float g_writtenPitch = 0.0f;
// The camera update after the turn reads the player's orbit speed to know which way the camera
// is turning (predictive collision: 0x0046F2C0/0x00472080 probe on that side and pull the
// camera in; 0x004726E0 keeps the last direction at state +0x4360 while |speed| >= 0.02). The
// direct turn therefore leaves the speed the game would have needed for it (orbit -= speed x
// dt) after the turn, and takes it back before the next turn so the game's easing never
// continues it.
std::uint8_t* g_speedPlayer = nullptr;
std::atomic<std::uint32_t> g_speedFrames{0};

void __cdecl BeforeCameraTurn(std::uint8_t* player) {
    // Only the same player: after a level change the previous one may be gone.
    if (g_speedPlayer == player) FloatAt(player, swtfu::kPlayerOrbitSpeed) = 0.0f;
    g_speedPlayer = nullptr;
    std::uint8_t* camera =
        reinterpret_cast<ActiveCameraFn>(static_cast<std::uintptr_t>(swtfu::kActiveCameraVa))(player);
    g_frame = TurnFrame{};
    if (camera == nullptr) return;
    g_frame.camera = camera;
    g_frame.orbit = FloatAt(camera, swtfu::kCameraOrbit);
    g_frame.pitch = FloatAt(camera, swtfu::kCameraPitch);
    if (g_writtenCamera == camera) {
        if (g_frame.pitch != g_writtenPitch) {
            g_pitchChangedOutside.fetch_add(1, std::memory_order_relaxed);
            g_outsidePitchWritten.store(g_writtenPitch);
            g_outsidePitchFound.store(g_frame.pitch);
        }
        if (g_frame.orbit != g_writtenOrbit) g_orbitChangedOutside.fetch_add(1, std::memory_order_relaxed);
    }
    g_writtenCamera = nullptr;
    if (g_frame.pitch < g_pitchMin.load()) g_pitchMin.store(g_frame.pitch);
    if (g_frame.pitch > g_pitchMax.load()) g_pitchMax.store(g_frame.pitch);
}

void __cdecl AfterCameraTurn(std::uint8_t* player) {
    g_turnCalls.fetch_add(1, std::memory_order_relaxed);
    g_player.store(player);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    long long dx = 0;
    long long dy = 0;
    if (!g_mouseStick.Drain(now.QuadPart, g_staleTicks, dx, dy)) {
        g_turnStale.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint8_t* camera = g_frame.camera;
    if (camera == nullptr || !KeyboardMouseMode()) return;
    g_camera.store(camera);
    if (player[swtfu::kPlayerOtherCameraOwner] != 0 || player[swtfu::kPlayerTurnBlocked] != 0) {
        if (dx != 0 || dy != 0) g_turnNotDriving.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (dx != 0 || dy != 0) {
        g_turnMoving.fetch_add(1, std::memory_order_relaxed);
        g_turnCountsX.fetch_add(dx < 0 ? -dx : dx, std::memory_order_relaxed);
        g_turnCountsY.fetch_add(dy < 0 ? -dy : dy, std::memory_order_relaxed);
    }
    float orbit = FloatAt(camera, swtfu::kCameraOrbit);
    float pitch = FloatAt(camera, swtfu::kCameraPitch);

    if (DirectCameraActive() && (dx != 0 || dy != 0)) {
        // The game's turn moves orbit with the sign of the stick and pitch with the sign of the
        // stick unless the camera's pitch invert is set (0x007AE6D0).
        const float scale = g_sensitivityMultiplier.load() * kDegreesPerCountPerSensitivity;
        const float orbitDelta = static_cast<float>(dx) * g_sensitivityX.load() * scale;
        float pitchDelta = static_cast<float>(dy) * g_sensitivityY.load() * scale;
        if (camera[swtfu::kCameraInvertPitch] != 0) pitchDelta = -pitchDelta;
        const auto wrap = reinterpret_cast<WrapAngleFn>(static_cast<std::uintptr_t>(swtfu::kWrapAngleVa));
        void* setter = camera + swtfu::kCameraSetterThis;
        void* angles = camera + swtfu::kCameraAngles;
        if (dx != 0) {
            orbit = wrap(orbit + orbitDelta);
            reinterpret_cast<SetAngleFn>(static_cast<std::uintptr_t>(swtfu::kSetOrbitVa))(setter, orbit, angles);
            const float frameTime = *reinterpret_cast<const float*>(
                static_cast<std::uintptr_t>(swtfu::kTurnTimeScaleVa));
            if (frameTime > 0.0f) {
                const float limit = 2.0f * std::max(FloatAt(player, swtfu::kPlayerOrbitSpeedMax), 1.0f);
                FloatAt(player, swtfu::kPlayerOrbitSpeed) = std::clamp(-orbitDelta / frameTime, -limit, limit);
                g_speedPlayer = player;
                g_speedFrames.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (dy != 0) {
            pitch = wrap(pitch + pitchDelta);
            reinterpret_cast<SetAngleFn>(static_cast<std::uintptr_t>(swtfu::kSetPitchVa))(setter, pitch, angles);
        }
        g_writtenCamera = camera;
        g_writtenOrbit = FloatAt(camera, swtfu::kCameraOrbit);
        g_writtenPitch = FloatAt(camera, swtfu::kCameraPitch);
    }
    // Both modes, every frame the player drives the camera: how far it turned (the game's own
    // turn keeps going for a few frames after the mouse stops; ours does not).
    const float orbitChange = WrappedDifference(FloatAt(camera, swtfu::kCameraOrbit), g_frame.orbit);
    const float pitchChange = WrappedDifference(FloatAt(camera, swtfu::kCameraPitch), g_frame.pitch);
    g_turnOrbitMicro.fetch_add(Micro(orbitChange), std::memory_order_relaxed);
    g_turnPitchMicro.fetch_add(Micro(pitchChange), std::memory_order_relaxed);
    CountSign(dx, orbitChange, g_sameSignX, g_oppositeSignX);
    CountSign(dy, pitchChange, g_sameSignY, g_oppositeSignY);
}

#define SWTFU_SAVE_XMM                                                                         \
    __asm sub esp, 128 __asm movdqu xmmword ptr[esp], xmm0 __asm movdqu xmmword ptr[esp + 16], \
        xmm1 __asm movdqu xmmword ptr[esp + 32], xmm2 __asm movdqu xmmword ptr[esp + 48], xmm3 \
            __asm movdqu xmmword ptr[esp + 64], xmm4 __asm movdqu xmmword ptr[esp + 80], xmm5  \
                __asm movdqu xmmword ptr[esp + 96], xmm6 __asm movdqu xmmword ptr[esp + 112], xmm7
#define SWTFU_RESTORE_XMM                                                                         \
    __asm movdqu xmm0, xmmword ptr[esp] __asm movdqu xmm1, xmmword ptr[esp + 16] __asm movdqu     \
        xmm2, xmmword ptr[esp + 32] __asm movdqu xmm3, xmmword ptr[esp + 48] __asm movdqu xmm4,   \
        xmmword ptr[esp + 64] __asm movdqu xmm5, xmmword ptr[esp + 80] __asm movdqu xmm6,         \
        xmmword ptr[esp + 96] __asm movdqu xmm7, xmmword ptr[esp + 112] __asm add esp, 128

// Detour of the mapper evaluation (ecx = mapper).
__declspec(naked) void MapperEvalThunk() {
    __asm {
        push ecx
        call dword ptr [g_originalMapperEval]
        pushad
    }
    SWTFU_SAVE_XMM
    __asm {
        push dword ptr [esp + 160]  // the mapper pushed on entry
        call AfterMapperEval
        add esp, 4
    }
    SWTFU_RESTORE_XMM
    __asm {
        popad
        lea esp, [esp + 4]
        ret
    }
}

// Replace `call 0x0069DF60` / `call 0x0069DF70` (ecx = input manager, result in st0).
__declspec(naked) void OrbitStickThunk() {
    __asm pushad
    SWTFU_SAVE_XMM
    __asm {
        push 0
        push ecx
        call CameraStick
        add esp, 8
    }
    SWTFU_RESTORE_XMM
    __asm {
        popad
        ret
    }
}

__declspec(naked) void PitchStickThunk() {
    __asm pushad
    SWTFU_SAVE_XMM
    __asm {
        push 1
        push ecx
        call CameraStick
        add esp, 8
    }
    SWTFU_RESTORE_XMM
    __asm {
        popad
        ret
    }
}

// Detour of the camera turn (ecx = player controller).
__declspec(naked) void CameraTurnThunk() {
    __asm {
        push ecx
        pushad
    }
    SWTFU_SAVE_XMM
    __asm {
        push dword ptr [esp + 160]
        call BeforeCameraTurn
        add esp, 4
    }
    SWTFU_RESTORE_XMM
    __asm {
        popad
        call dword ptr [g_originalCameraTurn]
        pushad
    }
    SWTFU_SAVE_XMM
    __asm {
        push dword ptr [esp + 160]
        call AfterCameraTurn
        add esp, 4
    }
    SWTFU_RESTORE_XMM
    __asm {
        popad
        lea esp, [esp + 4]
        ret
    }
}

bool InstallCameraHooks() {
    if (!InstallDetour(swtfu::kCameraTurnVa, swtfu::kCameraTurnPatchLength,
                       reinterpret_cast<const void*>(&CameraTurnThunk), g_originalCameraTurn,
                       "camera turn") ||
        !InstallDetour(swtfu::kMapperEvalVa, swtfu::kMapperEvalPatchLength,
                       reinterpret_cast<const void*>(&MapperEvalThunk), g_originalMapperEval,
                       "mapper evaluation") ||
        !PatchCall(swtfu::kOrbitStickCallVa, swtfu::kRightStickXAccessorVa,
                   reinterpret_cast<const void*>(&OrbitStickThunk), "camera orbit stick") ||
        !PatchCall(swtfu::kPitchStickCallVa, swtfu::kRightStickYAccessorVa,
                   reinterpret_cast<const void*>(&PitchStickThunk), "camera pitch stick")) {
        return false;
    }
    g_cameraHooks = true;
    return true;
}

void DiscardMouseStick() {
    long long x = 0;
    long long y = 0;
    g_mouseStick.Drain(0, 0, x, y);
}

#if SWTFU_DEV_TOOLS
// ---------------------------------------------------------------------------------------
// Mouse reader trace (development only)
//
// Hardware data breakpoints (DR0-DR3, read/write, 4 bytes). Slot 0 watches the mouse X float
// the handler stores (input+0x114), slot 1 the Y float (input+0x11C), slot 2 the raw lX of the
// DIMOUSESTATE2 (input+0xE1C, also passed to the action mapper 0x006B8F70), slot 3
// [Trace] ExtraAddress if set. Every distinct accessing instruction is logged once per round,
// with its registers and the return addresses found on its stack.

constexpr std::size_t kTraceEntries = 512;
constexpr std::size_t kTraceStack = 8;
const char* const kTraceSlotNames[4] = {"mouse X float", "mouse Y float", "DIMOUSESTATE2 lX",
                                        "extra address"};

struct TraceEntry {
    volatile LONG state;  // 0 free, 1 being filled, 2 ready
    std::uint32_t slot;
    std::uint32_t eip;
    volatile LONG hits;
    LONG hitsAtArm;
    LONG logged;
    std::uint32_t regs[8];  // eax ecx edx ebx esp ebp esi edi
    std::uint32_t callers[kTraceStack];
};

TraceEntry g_trace[kTraceEntries]{};
std::uint32_t g_watch[4]{};
volatile LONG g_traceArmed = 0;
volatile LONG g_tableFull = 0;
volatile LONG g_traceExceptions = 0;
std::uint32_t g_textBegin = 0;
std::uint32_t g_textEnd = 0;

bool LooksLikeReturnAddress(std::uint32_t value) {
    if (value < g_textBegin + 8 || value >= g_textEnd) return false;
    const auto* code = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(value));
    return code[-5] == 0xE8 || code[-2] == 0xFF || code[-3] == 0xFF || code[-6] == 0xFF;
}

// Plain C helper so __try can be used (no C++ unwinding in this function).
void ScanCallers(std::uint32_t esp, std::uint32_t* callers) {
    std::size_t found = 0;
    __try {
        const auto* stack = reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(esp));
        for (std::size_t index = 0; index < 128 && found < kTraceStack; ++index) {
            if (LooksLikeReturnAddress(stack[index])) callers[found++] = stack[index];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// An entry is one call path: the accessing instruction plus its first three return
// addresses. A shared reader (the mapped-axis getter 0x6A6DA0) thus gets one entry per
// caller instead of only the first caller's stack.
constexpr std::size_t kTraceKeyCallers = 3;

void RecordTraceHit(std::uint32_t slot, const CONTEXT& context) {
    std::uint32_t callers[kTraceStack]{};
    ScanCallers(context.Esp, callers);
    for (std::size_t index = 0; index < kTraceEntries; ++index) {
        TraceEntry& entry = g_trace[index];
        if (entry.state == 2) {
            if (entry.slot == slot && entry.eip == context.Eip &&
                std::memcmp(entry.callers, callers, kTraceKeyCallers * sizeof(callers[0])) == 0) {
                InterlockedIncrement(&entry.hits);
                return;
            }
            continue;
        }
        if (entry.state == 1) continue;
        if (InterlockedCompareExchange(&entry.state, 1, 0) != 0) continue;
        entry.slot = slot;
        entry.eip = context.Eip;
        entry.hits = 1;
        entry.hitsAtArm = 0;
        entry.logged = 0;
        const std::uint32_t regs[8] = {context.Eax, context.Ecx, context.Edx, context.Ebx,
                                       context.Esp, context.Ebp, context.Esi, context.Edi};
        std::memcpy(entry.regs, regs, sizeof(regs));
        std::memcpy(entry.callers, callers, sizeof(callers));
        InterlockedExchange(&entry.state, 2);
        return;
    }
    InterlockedExchange(&g_tableFull, 1);
}

volatile LONG g_traceSpurious = 0;
volatile LONG g_traceSpuriousEip = 0;
volatile LONG g_traceSpuriousDr6 = 0;

bool UsesOurWatches(const CONTEXT& context) {
    if ((context.Dr7 & 0xFF) == 0) return false;
    const DWORD drs[4] = {context.Dr0, context.Dr1, context.Dr2, context.Dr3};
    for (std::uint32_t slot = 0; slot < 4; ++slot) {
        if (g_watch[slot] != 0 && drs[slot] == g_watch[slot]) return true;
    }
    return false;
}

LONG CALLBACK TraceExceptionHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    const DWORD triggered = context.Dr6 & 0xF;
    if (triggered == 0) {
        // A data-breakpoint trap can arrive with none of B0-B3 set in the reported Dr6 (seen
        // under WOW64: an unhandled single step ended the game after ~110k hits). If the thread
        // carries our watches and this is not a trap-flag single step (BS, bit 14), it is ours:
        // a data breakpoint is a trap, so continuing simply runs on.
        if ((context.Dr6 & 0x4000) != 0 || !UsesOurWatches(context)) return EXCEPTION_CONTINUE_SEARCH;
        InterlockedIncrement(&g_traceSpurious);
        InterlockedExchange(&g_traceSpuriousEip, static_cast<LONG>(context.Eip));
        InterlockedExchange(&g_traceSpuriousDr6, static_cast<LONG>(context.Dr6));
        context.Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    InterlockedIncrement(&g_traceExceptions);
    for (std::uint32_t slot = 0; slot < 4; ++slot) {
        if ((triggered & (1u << slot)) != 0 && g_watch[slot] != 0) RecordTraceHit(slot, context);
    }
    context.Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Writes the current watches (or none) into the debug registers of every other thread.
void ApplyWatches(bool armed) {
    DWORD dr7 = 0;
    if (armed) {
        for (std::uint32_t slot = 0; slot < 4; ++slot) {
            if (g_watch[slot] == 0) continue;
            dr7 |= 1u << (slot * 2);       // local enable
            dr7 |= 3u << (16 + slot * 4);  // break on read or write
            dr7 |= 3u << (18 + slot * 4);  // 4 bytes
        }
    }
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                   FALSE, entry.th32ThreadID);
        if (thread == nullptr) continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                context.Dr0 = armed ? g_watch[0] : 0;
                context.Dr1 = armed ? g_watch[1] : 0;
                context.Dr2 = armed ? g_watch[2] : 0;
                context.Dr3 = armed ? g_watch[3] : 0;
                context.Dr7 = dr7;
                SetThreadContext(thread, &context);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

void ReportTrace(bool all) {
    for (std::size_t index = 0; index < kTraceEntries; ++index) {
        TraceEntry& entry = g_trace[index];
        if (entry.state != 2) continue;
        const LONG hits = entry.hits - entry.hitsAtArm;
        if (hits <= 0 || (entry.logged != 0 && !all)) continue;
        Log("trace %s eip=%08X hits=%ld eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X "
            "esi=%08X edi=%08X callers %08X %08X %08X %08X %08X %08X %08X %08X",
            kTraceSlotNames[entry.slot], entry.eip, hits, entry.regs[0], entry.regs[1],
            entry.regs[2], entry.regs[3], entry.regs[4], entry.regs[5], entry.regs[6],
            entry.regs[7], entry.callers[0], entry.callers[1], entry.callers[2], entry.callers[3],
            entry.callers[4], entry.callers[5], entry.callers[6], entry.callers[7]);
        entry.logged = 1;
    }
    if (InterlockedExchange(&g_tableFull, 0) != 0) Log("trace: table full, some readers not recorded");
    const LONG exceptions = InterlockedExchange(&g_traceExceptions, 0);
    if (exceptions != 0) Log("trace: %ld breakpoint hits since the last report", exceptions);
    const LONG spurious = InterlockedExchange(&g_traceSpurious, 0);
    if (spurious != 0) {
        Log("trace: %ld single steps without a breakpoint bit in Dr6 handled as ours (last eip=%08lX "
            "dr6=%08lX)", spurious, static_cast<unsigned long>(g_traceSpuriousEip),
            static_cast<unsigned long>(g_traceSpuriousDr6));
    }
}

// Last-chance logger (development builds): a crash leaves its code, address and debug
// registers in the log, then the previous filter (the game's or Windows') runs as before.
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

LONG WINAPI CrashLogger(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    const CONTEXT& context = *info->ContextRecord;
    Log("CRASH: exception %08lX at %08lX (thread %lu) eflags=%08lX dr0-3=%08lX %08lX %08lX %08lX "
        "dr6=%08lX dr7=%08lX watches=%08X %08X %08X %08X armed=%ld",
        static_cast<unsigned long>(record.ExceptionCode),
        static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(record.ExceptionAddress)),
        static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long>(context.EFlags),
        static_cast<unsigned long>(context.Dr0), static_cast<unsigned long>(context.Dr1),
        static_cast<unsigned long>(context.Dr2), static_cast<unsigned long>(context.Dr3),
        static_cast<unsigned long>(context.Dr6), static_cast<unsigned long>(context.Dr7), g_watch[0],
        g_watch[1], g_watch[2], g_watch[3], static_cast<long>(g_traceArmed));
    return g_previousFilter != nullptr ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void RearmTrace(unsigned round) {
    for (std::size_t index = 0; index < kTraceEntries; ++index) {
        TraceEntry& entry = g_trace[index];
        if (entry.state != 2) continue;
        entry.hitsAtArm = entry.hits;
        entry.logged = 0;
    }
    InterlockedExchange(&g_traceArmed, 1);
    Log("trace armed (round %u, %lu s): accesses seen from now on are logged", round, g_traceSeconds);
}

bool StartTrace() {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    ReadOwnMemory(swtfu::kImageBase, &dos, sizeof(dos));
    ReadOwnMemory(swtfu::kImageBase + dos.e_lfanew, &nt, sizeof(nt));
    g_textBegin = swtfu::kImageBase + nt.OptionalHeader.BaseOfCode;
    g_textEnd = g_textBegin + nt.OptionalHeader.SizeOfCode;
    g_watch[3] = g_traceExtraAddress;
    if (AddVectoredExceptionHandler(1, TraceExceptionHandler) == nullptr) {
        Log("trace disabled: AddVectoredExceptionHandler failed");
        return false;
    }
    g_previousFilter = SetUnhandledExceptionFilter(CrashLogger);
    Log("trace ready: waiting for the input manager; F11 in game starts a new round");
    return true;
}

// Called every 50 ms by the worker thread.
void ServiceTrace(DWORD now) {
    static bool started = false;
    static unsigned round = 0;
    static DWORD armedAt = 0;
    static DWORD lastApply = 0;
    static bool keyWasDown = false;
    if (!started) {
        void* input = g_input.load(std::memory_order_relaxed);
        if (input == nullptr) return;
        const auto base = reinterpret_cast<std::uint32_t>(input);
        g_watch[0] = base + swtfu::kInputMouseFloats;
        g_watch[1] = base + swtfu::kInputMouseFloatY;
        g_watch[2] = base + swtfu::kInputMouseState;
        started = true;
        armedAt = now;
        RearmTrace(++round);
    }
    const bool key = g_keyTraceRound != 0 && (GetAsyncKeyState(g_keyTraceRound) & 0x8000) != 0;
    if (key && !keyWasDown && GameHasForeground()) {
        ReportTrace(false);
        armedAt = now;
        RearmTrace(++round);
    }
    keyWasDown = key;

    const bool armed = InterlockedCompareExchange(&g_traceArmed, 0, 0) != 0;
    if (armed && now - armedAt >= g_traceSeconds * 1000) {
        InterlockedExchange(&g_traceArmed, 0);
        ApplyWatches(false);
        ReportTrace(false);
        Log("trace round %u finished; press F11 in game to trace again", round);
        return;
    }
    if (!armed) return;
    if (now - lastApply >= 1000) {
        ApplyWatches(true);  // also covers threads created since the last pass
        lastApply = now;
        ReportTrace(false);
    }
}
#endif

// ---------------------------------------------------------------------------------------
// Reporting and hotkeys

void ReportStatistics(DWORD elapsedMs) {
    const std::uint32_t polls = g_polls.exchange(0);
    const std::uint32_t failed = g_failedPolls.exchange(0);
    const std::uint32_t moving = g_movingPolls.exchange(0);
    const long long absX = g_absX.exchange(0);
    const long long absY = g_absY.exchange(0);
    const long long sumX = g_sumX.exchange(0);
    const long long sumY = g_sumY.exchange(0);
    const long maxX = g_maxX.exchange(0);
    const long maxY = g_maxY.exchange(0);
    const long long sumTicks = g_intervalSumTicks.exchange(0);
    const long long maxTicks = g_intervalMaxTicks.exchange(0);
    const long long minTicks = g_intervalMinTicks.exchange(LLONG_MAX);
    const std::uint32_t handlerCalls = g_handlerCalls.exchange(0);
    const std::uint32_t handlerMismatch = g_handlerMismatch.exchange(0);
    const long long handlerAbsX = g_handlerAbsX.exchange(0);
    const long long handlerAbsY = g_handlerAbsY.exchange(0);
    if (polls == 0 && failed == 0) return;

    const double toMs = 1000.0 / static_cast<double>(g_qpcFrequency.QuadPart);
    const double averageMs = polls > 1 ? sumTicks * toMs / (polls - 1) : 0.0;
    Log("input %lus: polls=%lu (%.1f/s) failed=%lu foreground=%u moving=%lu |x|=%lld |y|=%lld "
        "sum x=%lld y=%lld max x=%ld y=%ld interval ms min/avg/max=%.2f/%.2f/%.2f | handler calls=%lu "
        "mode=%lu |x|=%lld |y|=%lld differing=%lu thread=%lu",
        elapsedMs / 1000, polls, polls * 1000.0 / elapsedMs, failed, GameHasForeground() ? 1u : 0u,
        moving, absX, absY, sumX, sumY, maxX, maxY,
        minTicks == LLONG_MAX ? 0.0 : minTicks * toMs, averageMs, maxTicks * toMs, handlerCalls,
        static_cast<unsigned long>(g_handlerMode.load()), handlerAbsX, handlerAbsY, handlerMismatch,
        static_cast<unsigned long>(g_inputThread.load()));
}

float ReadFloat(std::uint32_t va) {
    float value = 0.0f;
    return ReadOwnMemory(va, &value, sizeof(value)) ? value : -999.0f;
}

std::uint32_t ReadU32(std::uint32_t va) {
    std::uint32_t value = 0;
    return ReadOwnMemory(va, &value, sizeof(value)) ? value : 0xFFFFFFFF;
}

std::uint32_t ReadU8(std::uint32_t va) {
    std::uint8_t value = 0;
    return ReadOwnMemory(va, &value, sizeof(value)) ? value : 0xFF;
}

void ReportCamera(DWORD elapsedMs) {
    const std::uint32_t evals = g_mapperEvals.exchange(0);
    const std::uint32_t cleared = g_mapperCleared.exchange(0);
    const std::uint32_t keyboard = g_keyboardFrames.exchange(0);
    const std::uint32_t turns = g_turnCalls.exchange(0);
    const std::uint32_t moving = g_turnMoving.exchange(0);
    const std::uint32_t notDriving = g_turnNotDriving.exchange(0);
    const std::uint32_t stale = g_turnStale.exchange(0);
    const long long countsX = g_turnCountsX.exchange(0);
    const long long countsY = g_turnCountsY.exchange(0);
    const double orbit = g_turnOrbitMicro.exchange(0) / 1e6;
    const double pitch = g_turnPitchMicro.exchange(0) / 1e6;
    const std::uint32_t sameX = g_sameSignX.exchange(0);
    const std::uint32_t oppositeX = g_oppositeSignX.exchange(0);
    const std::uint32_t sameY = g_sameSignY.exchange(0);
    const std::uint32_t oppositeY = g_oppositeSignY.exchange(0);
    const std::uint32_t pitchOutside = g_pitchChangedOutside.exchange(0);
    const std::uint32_t orbitOutside = g_orbitChangedOutside.exchange(0);
    const float pitchMin = g_pitchMin.exchange(1e9f);
    const float pitchMax = g_pitchMax.exchange(-1e9f);
    const std::uint32_t speedFrames = g_speedFrames.exchange(0);
    if (evals == 0 && turns == 0) return;
    Log("camera %lus (%s): mapper evals=%lu cleared=%lu keyboard-stick frames=%lu | turns=%lu moving=%lu "
        "speed-set=%lu "
        "not-driving=%lu stale=%lu counts |x|=%lld |y|=%lld turned |orbit|=%.2f |pitch|=%.2f deg "
        "(%.4f/%.4f deg per count) sign orbit +%lu/-%lu pitch +%lu/-%lu | pitch range %.2f..%.2f, "
        "changed between turns pitch=%lu (last %.2f -> %.2f) orbit=%lu",
        elapsedMs / 1000, DirectCameraActive() ? "direct" : "game's own", evals, cleared, keyboard,
        turns, moving, speedFrames, notDriving, stale, countsX, countsY, orbit, pitch,
        countsX != 0 ? orbit / countsX : 0.0, countsY != 0 ? pitch / countsY : 0.0, sameX, oppositeX,
        sameY, oppositeY, pitchMin, pitchMax, pitchOutside, g_outsidePitchWritten.load(),
        g_outsidePitchFound.load(), orbitOutside);
}

// The camera turn's tuning values, which only exist at run time (camera-path notes 6.5).
void LogCameraTuning() {
    const auto player = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_player.load()));
    const auto camera = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_camera.load()));
    const auto input = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(GameInput()));
    if (player == 0 || input == 0) return;
    const std::uint32_t mapper = ReadU32(input + swtfu::kInputMapper);
    const std::uint32_t settings = ReadU32(mapper + swtfu::kMapperSettings);
    float tuning[6]{};
    ReadOwnMemory(swtfu::kTurnTuningVa, tuning, sizeof(tuning));
    Log("camera tuning: player %08X curve pad/mouse %.3f/%.3f orbit max %.3f speed %.3f limit base %.3f | "
        "pitch base %.3f speed %.3f | owner %lu blocked %lu state %lu | camera %08X orbit %.2f pitch %.2f "
        "invert %lu mode %lu | ease pitch %.3f/%.3f orbit %.3f/%.3f exponent %.2f weight %.2f time %.5f | "
        "gain %.3f mapper kbm %lu invert x %lu y %lu",
        player, ReadFloat(player + swtfu::kPlayerCurvePad), ReadFloat(player + swtfu::kPlayerCurveMouse),
        ReadFloat(player + swtfu::kPlayerOrbitSpeedMax), ReadFloat(player + swtfu::kPlayerOrbitSpeed),
        ReadFloat(player + swtfu::kPlayerOrbitLimitBase), ReadFloat(player + swtfu::kPlayerPitchSpeedBase),
        ReadFloat(player + swtfu::kPlayerPitchSpeed), ReadU8(player + swtfu::kPlayerOtherCameraOwner),
        ReadU8(player + swtfu::kPlayerTurnBlocked), ReadU32(player + swtfu::kPlayerTurnState), camera,
        camera != 0 ? ReadFloat(camera + swtfu::kCameraOrbit) : 0.0f,
        camera != 0 ? ReadFloat(camera + swtfu::kCameraPitch) : 0.0f,
        camera != 0 ? ReadU8(camera + swtfu::kCameraInvertPitch) : 0u,
        camera != 0 ? ReadU32(camera + swtfu::kCameraMode) : 0u, tuning[0], tuning[1], tuning[2],
        tuning[3], tuning[4], tuning[5], ReadFloat(swtfu::kTurnTimeScaleVa),
        ReadFloat(ReadU32(input + swtfu::kInputSensitivity)), ReadU8(settings), ReadU8(settings + 1),
        ReadU8(settings + 2));
}

void LogSensitivity(const char* source) {
    Log("%s: SensitivityX=%.2f SensitivityY=%.2f SensitivityMultiplier=%.3f", source,
        g_sensitivityX.load(), g_sensitivityY.load(), g_sensitivityMultiplier.load());
}

bool KeyDown(int key) { return key != 0 && (GetAsyncKeyState(key) & 0x8000) != 0; }

void PollHotkeys() {
    static bool wasDown[4]{};
    const int keys[4] = {g_keyReload, g_keyToggle, g_keyMultiplierDown, g_keyMultiplierUp};
    bool down[4]{};
    for (int index = 0; index < 4; ++index) down[index] = KeyDown(keys[index]);
    if (g_hotkeys && GameHasForeground()) {
        if (down[0] && !wasDown[0]) {
            LoadConfig();
            LogSensitivity("reload");
        }
        if (down[1] && !wasDown[1]) {
            const LONG enabled = InterlockedCompareExchange(&g_directCamera, 0, 0) == 0 ? 1 : 0;
            InterlockedExchange(&g_directCamera, enabled);
            DiscardMouseStick();
            Log("toggle: direct mouse camera %s", enabled ? "ON" : "OFF (game's own handling)");
        }
        if ((down[2] && !wasDown[2]) || (down[3] && !wasDown[3])) {
            const float factor = down[3] ? kHotkeyMultiplierStep : 1.0f / kHotkeyMultiplierStep;
            g_sensitivityMultiplier.store(
                std::clamp(g_sensitivityMultiplier.load() * factor, kMinMultiplier, kMaxMultiplier));
            LogSensitivity(down[3] ? "multiplier up (this session; set it in the ini to keep it)"
                                   : "multiplier down (this session; set it in the ini to keep it)");
        }
    }
    for (int index = 0; index < 4; ++index) wasDown[index] = down[index];
}

void LogInputObject(void* input) {
    const auto* bytes = static_cast<const std::uint8_t*>(input);
    const std::uint32_t vtable = *reinterpret_cast<const std::uint32_t*>(bytes);
    std::uint32_t handler = 0;
    ReadOwnMemory(vtable + swtfu::kMouseHandlerSlot, &handler, sizeof(handler));
    Log("input manager at %p: vtable %08X, slot +0x48 %08X (%s), window mode %lu, mouse device %08X",
        input, static_cast<unsigned>(vtable), static_cast<unsigned>(handler),
        handler == swtfu::kMouseHandlerVa ? "the hooked handler" : "NOT the hooked handler",
        static_cast<unsigned long>(*reinterpret_cast<const std::uint32_t*>(bytes + swtfu::kInputWindowMode)),
        static_cast<unsigned>(*reinterpret_cast<const std::uint32_t*>(bytes + swtfu::kInputMouseDevice)));
}

HANDLE g_initThread = nullptr;

DWORD WINAPI Initialize(LPVOID) {
    if (InterlockedCompareExchange(&g_initializationStarted, 1, 0) != 0) return 1;
    // From here on this thread runs our code and will write hooks into the game: the module
    // must stay loaded. (A loader that unloads us before this point is handled in DllMain.)
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&Initialize), &pinned);
    QueryPerformanceFrequency(&g_qpcFrequency);
    g_staleTicks = g_qpcFrequency.QuadPart * kStaleCountsMs / 1000;
    Log("SWTFU Raw Mouse Fix %s loading%s", SWTFU_RAW_MOUSE_VERSION,
        SWTFU_DEV_TOOLS ? " (development build)" : "");
    if (!HostIsGame()) {
        Log("Host executable is not %s; the fix stays inactive", swtfu::kHostExecutable);
        return 1;
    }
    if (!AcquireInstanceGuard()) return 1;

    char failure[256]{};
    if (!InspectImage(failure, sizeof(failure)) || !WaitForValidLayout(failure, sizeof(failure))) {
        Log("Executable layout does not match the supported layout: %s; the fix stays inactive",
            failure);
        return 1;
    }
    Log("layout validated: %u code signatures, %u data signatures",
        static_cast<unsigned>(sizeof(swtfu::kCodeSignatures) / sizeof(swtfu::kCodeSignatures[0])),
        static_cast<unsigned>(sizeof(swtfu::kDataSignatures) / sizeof(swtfu::kDataSignatures[0])));
    LoadConfig();
    if (!g_enabled) {
        Log("disabled by configuration (Enabled=0); the game runs unmodified");
        return 0;
    }
    if (!InstallDetour(swtfu::kPollMouseVa, swtfu::kPollMousePatchLength,
                       reinterpret_cast<const void*>(&HookPollMouse), g_originalPollMouse,
                       "mouse poll") ||
        !InstallDetour(swtfu::kMouseHandlerVa, swtfu::kMouseHandlerPatchLength,
                       reinterpret_cast<const void*>(&HookMouseHandler), g_originalMouseHandler,
                       "mouse handler")) {
        return 1;
    }
    if (InstallCameraHooks()) {
        Log("initialized: direct mouse camera %s (F8 switches to the game's own handling and back)",
            DirectCameraActive() ? "ON" : "OFF");
    } else {
        Log("initialized without the direct mouse camera (a camera hook failed); observation only");
    }
    HANDLE environment = CreateThread(nullptr, 0, LogEnvironmentThread, nullptr, 0, nullptr);
    if (environment != nullptr) CloseHandle(environment);
#if SWTFU_DEV_TOOLS
    const bool tracing = g_traceMouseReaders && StartTrace();
#endif

    void* reportedInput = nullptr;
    DWORD lastReport = GetTickCount();
    for (;;) {
        Sleep(50);
        void* input = g_input.load(std::memory_order_relaxed);
        if (input != reportedInput) {
            LogInputObject(input);
            reportedInput = input;
        }
        const DWORD now = GetTickCount();
        PollHotkeys();
#if SWTFU_DEV_TOOLS
        if (tracing) ServiceTrace(now);
#endif
        if (now - lastReport >= kReportIntervalMs) {
            CheckPatches();
            if (g_diagnostics) {
                ReportStatistics(now - lastReport);
                ReportCamera(now - lastReport);
                LogCameraTuning();
            }
            lastReport = now;
        }
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_logLock);
        CaptureModulePaths(instance);
        DisableThreadLibraryCalls(instance);
        g_initThread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // FreeLibrary right after LoadLibrary (a loader that loads every .asi and keeps only
        // some): the start-up thread cannot have run yet, because this unload holds the loader
        // lock it needs to start. Claim it so it never runs our code, end it, and leave
        // nothing behind. Once Initialize runs, the module is pinned and this is not reached.
        if (InterlockedCompareExchange(&g_initializationStarted, 2, 0) == 0) {
            if (g_initThread != nullptr) TerminateThread(g_initThread, 0);
            DeleteCriticalSection(&g_logLock);
        }
        if (g_initThread != nullptr) CloseHandle(g_initThread);
        g_initThread = nullptr;
    }
    return TRUE;
}
