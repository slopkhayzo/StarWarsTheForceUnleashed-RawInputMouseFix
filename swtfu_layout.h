// Executable layout of Star Wars: The Force Unleashed (Ultimate Sith Edition, Steam) used by
// the raw mouse fix.
//
// Shared by the plugin (reads the running image) and layout-profile-test (reads an exe from
// disk), so both apply exactly the same checks. The image is not relocatable (no
// DYNAMIC_BASE), so absolute addresses inside the signatures are stable.
//
// The Steam exe is wrapped in SteamStub 2.1: .text is encrypted on disk and decrypted when the
// game starts, so the signatures only match in the running game (or in the Steamless copy in
// research\bin\steam, which is used for the offline test).
//
// SHA-256 values only name the build in the log; the signatures decide whether the plugin may
// hook. Any mismatch fails closed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace swtfu {

constexpr std::uint32_t kImageBase = 0x00400000;
constexpr std::uint32_t kTimeDateStamp = 0x4B75EA93;
// Steam image with the SteamStub .bind section; the Steamless copy lacks it.
constexpr std::uint32_t kSteamSizeOfImage = 0x036A0000;
constexpr std::uint32_t kMinSizeOfImage = 0x0364A000;
constexpr char kHostExecutable[] = "SWTFU.exe";

struct KnownBuild {
    const char* sha256;  // upper-case hex
    const char* name;
};

constexpr KnownBuild kKnownBuilds[] = {
    {"1C156705548FCE9E33FA3423E75B5CBE36248319B96F4B4850BF2E8125692032",
     "Steam build 6522 (SteamStub 2.1)"},
    {"2D872CA97BA05C6E9777506549B514AC2DA7E74D5A0AA08DDD4F8299A95D7B8A",
     "Steam build 6522 unpacked with Steamless (analysis copy)"},
};

// ---- Input manager (DirectInput) --------------------------------------------------------
// void __thiscall Input::PollMouse(): Acquire if needed, GetDeviceState(0x14, this+0xE1C),
// then this->vtbl[0x48](float lX, float lY, float lZ, BYTE* buttons, 8); on failure it zeroes
// the state. Ends with a tail jump to the cursor show/hide helper 0x0069DA00.
constexpr std::uint32_t kPollMouseVa = 0x006A0130;
constexpr std::size_t kPollMousePatchLength = 9;  // push esi / mov esi,ecx / mov eax,[esi+0CF0h]
// int __thiscall Input::OnMouse(float x, float y, float z, BYTE* buttons, int count), ret 14h:
// vtable slot +0x48 of the input manager. Zeroes the axes while the window is not foreground
// (and, in window mode 1, while the cursor is outside the client area).
constexpr std::uint32_t kMouseHandlerVa = 0x006A3D60;
constexpr std::size_t kMouseHandlerPatchLength = 6;  // sub esp,18h / push esi / mov esi,ecx
constexpr std::uint32_t kMouseHandlerSlot = 0x48;

// Offsets in the input manager object.
constexpr std::uint32_t kInputWindow = 0x18;        // HWND
constexpr std::uint32_t kInputWindowMode = 0x2C;    // 0/1: non-exclusive, 2/3: exclusive mouse
constexpr std::uint32_t kInputMouseFloats = 0x114;  // float x at +0x114, y at +0x11C, z at +0x124
constexpr std::uint32_t kInputMouseFloatY = 0x11C;
constexpr std::uint32_t kInputMouseDevice = 0xCF0;  // IDirectInputDevice8A*
constexpr std::uint32_t kInputMouseAcquired = 0xCF4;  // bool
constexpr std::uint32_t kInputMouseState = 0xE1C;   // DIMOUSESTATE2 lX, lY, lZ, rgbButtons[8]

constexpr std::uint32_t kInputMapper = 0xCE0;       // PC action mapper (Aspyr layer)
constexpr std::uint32_t kInputSensitivity = 0xD18;  // float* sensitivity gain (options slider)

// ---- PC action mapper -------------------------------------------------------------------
// Per frame in keyboard-and-mouse mode: 0x006B8F70 copies the keys, 0x006A8BD0 splits the
// mouse counts into four half-axes of absolute counts, then 0x006B8D80 evaluates the bindings
// into four virtual stick floats (left X/Y, right X/Y, in raw counts per frame).
constexpr std::uint32_t kMapperEvalVa = 0x006B8D80;  // void __fastcall(mapper)
constexpr std::size_t kMapperEvalPatchLength = 6;    // push ecx/ebx/ebp/esi / mov esi,ecx
// float __thiscall(mapper, binding* positive, binding* negative), ret 8. Reads only. Its
// caller assumes it leaves ecx unchanged (0x006B8E1F calls it without reloading ecx).
constexpr std::uint32_t kAxisEvalVa = 0x006A8CD0;
constexpr std::uint32_t kMapperRightStickX = 0x8;  // float, written by kMapperEvalVa
constexpr std::uint32_t kMapperRightStickY = 0xC;
constexpr std::uint32_t kMapperBindings = 0x58;  // binding table
constexpr std::uint32_t kMapperSettings = 0x64;  // bytes: +0 keyboard-and-mouse mode, +1/+2 invert X/Y
constexpr std::uint32_t kMapperMouseHalfAxes = 0x170;  // 4 floats (+0x170..+0x17C)
constexpr std::uint32_t kBindRightXPositive = 0xF0;    // binding pairs in the table
constexpr std::uint32_t kBindRightXNegative = 0xFC;
constexpr std::uint32_t kBindRightYPositive = 0x108;
constexpr std::uint32_t kBindRightYNegative = 0x114;

// ---- Gameplay camera (keyboard-and-mouse mode) -------------------------------------------
// 0x00C34CE0 / 0x00C34EA0 turn the mapper's right stick (input accessors 0x0069DF60/70) into a
// stepped speed value. Their accessor calls are redirected so they see only the keyboard part.
constexpr std::uint32_t kOrbitStickCallVa = 0x00C34CE8;  // call 0x0069DF60
constexpr std::uint32_t kPitchStickCallVa = 0x00C34EA8;  // call 0x0069DF70
constexpr std::uint32_t kRightStickXAccessorVa = 0x0069DF60;
constexpr std::uint32_t kRightStickYAccessorVa = 0x0069DF70;
// void __fastcall CameraTurn(player): eases the turn speed and writes the camera orbit/pitch.
constexpr std::uint32_t kCameraTurnVa = 0x007AE6D0;
constexpr std::size_t kCameraTurnPatchLength = 6;  // push ebp / mov ebp,esp / and esp,-16
constexpr std::uint32_t kActiveCameraVa = 0x00C35100;  // camera* __fastcall(player)
constexpr std::uint32_t kWrapAngleVa = 0x0046ADE0;     // float __cdecl(float degrees): [0, 360)
constexpr std::uint32_t kSetOrbitVa = 0x0046A9B0;  // __thiscall(camera+0x270, float, camera+0x10)
constexpr std::uint32_t kSetPitchVa = 0x0046A9D0;
constexpr std::uint32_t kGameGlobals = 0x01240440;  // pointer: +0 input manager, +0x90 camera

// Player controller (the camera turn's `this`).
constexpr std::uint32_t kPlayerOtherCameraOwner = 0x58E4;  // byte: camera not driven by the player
constexpr std::uint32_t kPlayerTurnBlocked = 0x5454;       // byte
constexpr std::uint32_t kPlayerTurnState = 0x370;          // 1 = turned by the player this frame
constexpr std::uint32_t kPlayerCurvePad = 0x5738;          // float response-curve weight
constexpr std::uint32_t kPlayerCurveMouse = 0x573C;
constexpr std::uint32_t kPlayerPitchSpeedBase = 0x5604;
constexpr std::uint32_t kPlayerPitchSpeed = 0x5608;
constexpr std::uint32_t kPlayerOrbitLimitBase = 0x5610;
constexpr std::uint32_t kPlayerOrbitSpeed = 0x5614;
constexpr std::uint32_t kPlayerOrbitSpeedMax = 0x5618;

// Camera.
constexpr std::uint32_t kCameraAngles = 0x10;  // setter argument
constexpr std::uint32_t kCameraOrbit = 0x7C;   // float degrees
constexpr std::uint32_t kCameraPitch = 0x80;   // float degrees
constexpr std::uint32_t kCameraSetterThis = 0x270;
constexpr std::uint32_t kCameraInvertPitch = 0x4812;  // byte
constexpr std::uint32_t kCameraMode = 0x4884;         // 1: the player does not drive the camera

// Tuning globals of the camera turn (read for the log only).
constexpr std::uint32_t kTurnTuningVa = 0x010F52A0;  // 6 floats: pitch up/down, orbit up/down, exponent, weight
constexpr std::uint32_t kTurnTimeScaleVa = 0x01244290;  // float, set at run time

struct CodeSignature {
    const char* name;
    std::uint32_t va;
    const char* pattern;
    bool hookSite;  // a detour is written here (used to tell a foreign patch from another build)
};

constexpr CodeSignature kCodeSignatures[] = {
    {"mouse poll (GetDeviceState, handler call)", kPollMouseVa,
     "56 8B F1 8B 86 F0 0C 00 00 85 C0 0F 84 AD 00 00 00 80 BE F4 0C 00 00 00 75 10 8B 08 8B "
     "51 1C 50 FF D2 85 C0 0F 8C 94 00 00 00 8B 86 F0 0C 00 00 57 8D BE 1C 0E 00 00 57 C6 86 "
     "F4 0C 00 00 01 8B 08 8B 51 24 6A 14 50 FF D2 85 C0 7C 43 F3 0F 2A 86 24 0E 00 00 8B 06 "
     "8B 50 48 6A 08 8D 8E 28 0E 00 00 51 83 EC 0C F3 0F 11 44 24 08 F3 0F 2A 86 20 0E 00 00 "
     "F3 0F 11 44 24 04 F3 0F 2A 07 8B CE F3 0F 11 04 24 FF D2 5F 8B CE 5E E9 40 D8 FF FF",
     true},
    {"keyboard and mouse poll calls in the input update", 0x006A2CE4,
     "8B CE E8 A5 AC FF FF 8B CE E8 3E D4 FF FF", false},
    {"mouse handler (input vtable +0x48)", kMouseHandlerVa,
     "83 EC 18 56 8B F1 8B 46 2C 83 F8 02 0F 84 4E 01 00 00 83 F8 03 0F 84 4A", true},
    {"mouse device creation (GUID_SysMouse, c_dfDIMouse2)", 0x006A80C7,
     "8B 07 6A 00 8D AE F0 0C 00 00 55 C6 86 10 0D 00 00 00 8B 08 8B 51 0C 68 5C 02 FE 00 50 "
     "FF D2 85 C0 7C 72 8B 6D 00 8B 45 00 8B 48 2C 68 8C F2 FD 00 55 FF D1",
     false},
    // Mapper evaluation: button bytes, then the four virtual axes (right stick at +8/+0xC from
    // the binding pairs +0xF0/+0xFC and +0x108/+0x114).
    {"mapper evaluation (binding table, button bytes)", kMapperEvalVa,
     "51 53 55 56 8B F1 8D 6E 10 57 8B DD 33 FF 8B FF 8B 46 58 8B 14 38 03 C7 32 C9 83 EA 00 74 "
     "11 83 EA 01 75 13 8B 40 08 8A 8C 30 68 01 00 00 EB 07 8B 48 08 8A 4C 31 68 88 0B 83 C7 0C "
     "83 C3 01 81 FF C0 00 00 00 7C C9",
     true},
    {"mapper evaluation (four virtual axes)", 0x006B8DC7,
     "8B 7E 58 8D 97 CC 00 00 00 52 8D 87 C0 00 00 00 50 8B CE E8 F1 FE FE FF D9 1E 8D 8F E4 00 "
     "00 00 51 8D 97 D8 00 00 00 52 8B CE E8 DA FE FE FF D9 5E 04 8D 87 FC 00 00 00 50 8D 8F F0 "
     "00 00 00 51 8B CE E8 C2 FE FE FF D9 5E 08 8D 97 14 01 00 00 52 81 C7 08 01 00 00 57 E8 AC "
     "FE FE FF D9",
     false},
    {"mapper key copy, half-axes, evaluation calls", 0x006B8F8D,
     "50 8B CE E8 3B FC FE FF E8 E6 FD FF FF 5E C2", false},
    {"axis evaluation (entry)", kAxisEvalVa,
     "83 EC 0C 8B 44 24 10 8B 54 24 14 0F 57 C0 56 F3 0F 11 44 24", false},
    {"axis evaluation (return)", 0x006A8DF6, "D9 44 24 04 5E 83 C4 0C C2 08 00", false},
    {"keyboard-and-mouse mode accessor", 0x0069DF20, "8B 89 E0 0C 00 00 E9 E5 AB 00 00", false},
    {"keyboard-and-mouse mode flag", 0x006A8B10, "8B 41 64 8A 00 C3", false},
    {"sensitivity gain accessor", 0x0069DF30, "8B 81 18 0D 00 00 D9 00 C3", false},
    {"right stick X accessor", kRightStickXAccessorVa, "8B 81 E0 0C 00 00 D9 40 08 C3", false},
    {"right stick Y accessor", kRightStickYAccessorVa, "8B 81 E0 0C 00 00 D9 40 0C C3", false},
    {"camera orbit stick reader", 0x00C34CE0, "51 A1 40 04 24 01 8B 08 E8 73 92 A6 FF", false},
    {"camera pitch stick reader", 0x00C34EA0, "51 A1 40 04 24 01 8B 08 E8 C3 90 A6 FF", false},
    {"camera input (keyboard-and-mouse branch)", 0x00C362AC,
     "E8 6F 7C A6 FF 84 C0 74 1B 8B CB E8 24 EA FF FF D9 5C 24 08 8B CB E8 D9 EB FF FF D9 5C 24 04",
     false},
    {"camera turn (entry)", kCameraTurnVa, "55 8B EC 83 E4 F0 6A FF 68 B8 68 DD 00", true},
    {"camera turn (player drives the camera)", 0x007AEA06,
     "E8 C5 9E FF FF 38 9E E4 58 00 00 0F 85 2E 02 00 00 38 9E 54 54 00 00 0F 85 16 02 00 00 8B "
     "CE 89 BE 70 03 00 00 E8 D0 66 48 00",
     false},
    {"camera turn (orbit write)", 0x007AEADF,
     "F3 0F 10 15 90 42 24 01 F3 0F 10 4F 7C F3 0F 5A C0 0F 5A D2 F2 0F 59 C2 0F 5A C9 F2 0F 5C "
     "C8 51 66 0F 5A C1 F3 0F 11 04 24 E8 D3 C2 CB FF D9 5C 24 38 D9 44 24 38 83 C4 04 8D 4F 10 "
     "51 51 8D 8F 70 02 00 00 D9 1C 24 E8 85 BE CB FF",
     false},
    {"camera turn (pitch write)", 0x007AEBE1,
     "F3 0F 10 0D 90 42 24 01 F3 0F 5A C0 0F 5A C9 F2 0F 59 C1 F3 0F 10 8F 80 00 00 00 0F 5A C9 "
     "F2 0F 58 C1 51 66 0F 5A C0 F3 0F 11 04 24 E8 CE C1 CB FF D9 5C 24 38 D9 44 24 38 83 C4 04 "
     "8D 57 10 52 51 8D 8F 70 02 00 00 D9 1C 24 E8 A0 BD CB FF",
     false},
    {"camera orbit setter", kSetOrbitVa,
     "0F 57 C0 8B 44 24 08 F3 0F 11 81 C4 42 00 00 F3 0F 10 44 24 04 F3 0F 11 40 6C C2 08 00",
     false},
    {"camera pitch setter", kSetPitchVa,
     "0F 57 C0 8B 44 24 08 F3 0F 11 81 C4 42 00 00 F3 0F 10 44 24 04 F3 0F 11 40 70 C2 08 00",
     false},
    {"angle wrap", kWrapAngleVa, "83 EC 08 D9 44 24 0C DD 14 24 DD 05 90 44 F5 00", false},
    {"active camera of a player", kActiveCameraVa,
     "8B 41 18 33 D2 85 C0 7C 13 83 F8 01 8B 0D 40 04 24 01 7D 15 8B 84 81 90 00 00 00 C3 A1 40 "
     "04 24 01 8B 84 90 90 00 00 00 C3 8B 84 91 90 00 00 00",
     false},
    {"player update: camera input, camera turn", 0x007C2300, "E8 8B 3F 47 00 8B CE E8 C4 C3 FE FF",
     false},
};

constexpr CodeSignature kDataSignatures[] = {
    {"GUID_SysMouse", 0x00FE025C, "60 2B 1D 6F A0 D5 CF 11 BF C7 44 45 53 54 00 00", false},
    {"c_dfDIMouse2 header", 0x00FDF28C, "18 00 00 00 10 00 00 00 02 00 00 00 14 00 00 00 0B 00 00 00",
     false},
};

// Parses "AA ?? BB" into bytes and a mask. Returns the byte count, 0 on a malformed pattern.
inline std::size_t ParsePattern(const char* pattern, std::uint8_t* bytes, bool* mask,
                                std::size_t capacity) {
    std::size_t count = 0;
    for (const char* p = pattern; *p != '\0';) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (count == capacity || p[1] == '\0') return 0;
        if (p[0] == '?' && p[1] == '?') {
            bytes[count] = 0;
            mask[count] = false;
        } else {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            const int high = nibble(p[0]);
            const int low = nibble(p[1]);
            if (high < 0 || low < 0) return 0;
            bytes[count] = static_cast<std::uint8_t>(high * 16 + low);
            mask[count] = true;
        }
        ++count;
        p += 2;
    }
    return count;
}

// Reader: bool(std::uint32_t va, void* destination, std::size_t length).
template <typename Reader>
bool PatternMatches(Reader& read, const CodeSignature& signature, char* failure,
                    std::size_t failureSize) {
    std::uint8_t expected[512]{};
    bool mask[512]{};
    std::uint8_t actual[512]{};
    const std::size_t length = ParsePattern(signature.pattern, expected, mask, sizeof(expected));
    if (length == 0) {
        std::snprintf(failure, failureSize, "malformed pattern: %s", signature.name);
        return false;
    }
    if (!read(signature.va, actual, length)) {
        std::snprintf(failure, failureSize, "unreadable: %s at %08X", signature.name,
                      static_cast<unsigned>(signature.va));
        return false;
    }
    for (std::size_t index = 0; index < length; ++index) {
        if (mask[index] && actual[index] != expected[index]) {
            // A jump written over the first bytes of a hook site, with the rest intact, is
            // another plugin's detour rather than a different build.
            const bool jumpAtStart = actual[0] == 0xE9 || actual[0] == 0xE8 ||
                                     (actual[0] == 0xFF && actual[1] == 0x25) || actual[0] == 0x68;
            bool restMatches = signature.hookSite && jumpAtStart;
            for (std::size_t rest = 16; restMatches && rest < length; ++rest) {
                if (mask[rest] && actual[rest] != expected[rest]) restMatches = false;
            }
            std::snprintf(failure, failureSize,
                          "%s: %s at %08X (+%u: %02X, expected %02X)",
                          restMatches ? "foreign patch (another plugin hooked this function)"
                                      : "signature mismatch",
                          signature.name, static_cast<unsigned>(signature.va),
                          static_cast<unsigned>(index), actual[index], expected[index]);
            return false;
        }
    }
    return true;
}

// Checks every signature the fix depends on. On failure, writes the first problem into
// `failure` and returns false.
template <typename Reader>
bool ValidateLayout(Reader& read, char* failure, std::size_t failureSize) {
    for (const CodeSignature& signature : kCodeSignatures) {
        if (!PatternMatches(read, signature, failure, failureSize)) return false;
    }
    for (const CodeSignature& signature : kDataSignatures) {
        if (!PatternMatches(read, signature, failure, failureSize)) return false;
    }
    return true;
}

inline const char* KnownBuildName(const char* sha256) {
    for (const KnownBuild& build : kKnownBuilds) {
        if (std::strcmp(build.sha256, sha256) == 0) return build.name;
    }
    return nullptr;
}

}  // namespace swtfu
