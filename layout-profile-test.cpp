// Validates swtfu_layout.h against an SWTFU.exe on disk, mapped like the loader maps it.
//   layout-profile-test.exe <exe> <label>                   the layout must be accepted, and a
//                                                            corrupted byte of each code signature
//                                                            rejected (the Steamless copy)
//   layout-profile-test.exe <exe> <label> --expect-reject   the layout must be rejected (the
//                                                            Steam exe, whose code is encrypted on
//                                                            disk: the fix must wait, not hook)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "swtfu_layout.h"

namespace {

bool LoadImage(const wchar_t* path, std::vector<std::uint8_t>& image) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::vector<std::uint8_t> raw;
    DWORD read = 0;
    bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart < (1LL << 30);
    if (ok) {
        raw.resize(static_cast<std::size_t>(size.QuadPart));
        ok = ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr) != FALSE &&
             read == raw.size();
    }
    CloseHandle(file);
    if (!ok || raw.size() < sizeof(IMAGE_DOS_HEADER)) return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > raw.size()) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.ImageBase != swtfu::kImageBase ||
        nt->OptionalHeader.SizeOfImage < swtfu::kMinSizeOfImage) {
        return false;
    }
    image.assign(nt->OptionalHeader.SizeOfImage, 0);
    std::memcpy(image.data(), raw.data(), std::min<std::size_t>(nt->OptionalHeader.SizeOfHeaders, raw.size()));
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        const std::size_t count = std::min<std::size_t>(section->SizeOfRawData, section->Misc.VirtualSize);
        if (section->PointerToRawData + count > raw.size() || section->VirtualAddress + count > image.size()) {
            return false;
        }
        std::memcpy(image.data() + section->VirtualAddress, raw.data() + section->PointerToRawData, count);
    }
    return true;
}

}  // namespace

int wmain(int argumentCount, wchar_t** arguments) {
    const bool expectReject = argumentCount == 4 && std::wcscmp(arguments[3], L"--expect-reject") == 0;
    if (argumentCount != 3 && !expectReject) {
        std::fwprintf(stderr, L"Usage: layout-profile-test.exe <SWTFU.exe> <label> [--expect-reject]\n");
        return 2;
    }
    std::vector<std::uint8_t> image;
    if (!LoadImage(arguments[1], image)) {
        std::fwprintf(stderr, L"[%ls] could not map %ls as an x86 image at %08X\n", arguments[2],
                      arguments[1], static_cast<unsigned>(swtfu::kImageBase));
        return 3;
    }
    auto reader = [&image](std::uint32_t va, void* destination, std::size_t length) {
        if (va < swtfu::kImageBase || va - swtfu::kImageBase + length > image.size()) return false;
        std::memcpy(destination, image.data() + (va - swtfu::kImageBase), length);
        return true;
    };

    char failure[256]{};
    const bool accepted = swtfu::ValidateLayout(reader, failure, sizeof(failure));
    if (expectReject) {
        if (accepted) {
            std::fwprintf(stderr, L"[%ls] layout accepted, but rejection was expected\n", arguments[2]);
            return 6;
        }
        std::printf("[%ls] layout rejected as expected: %s\n", arguments[2], failure);
        return 0;
    }
    if (!accepted) {
        std::fprintf(stderr, "[%ls] layout rejected: %s\n", arguments[2], failure);
        return 4;
    }

    // Negative checks: flip one byte of every code signature in turn; each must be rejected. A
    // jump written over a hook site must be reported as a foreign patch.
    for (const swtfu::CodeSignature& signature : swtfu::kCodeSignatures) {
        std::uint8_t& byte = image[signature.va - swtfu::kImageBase];
        byte ^= 0xFF;
        const bool corruptedAccepted = swtfu::ValidateLayout(reader, failure, sizeof(failure));
        byte ^= 0xFF;
        if (corruptedAccepted) {
            std::fprintf(stderr, "[%ls] corrupted signature was accepted: %s\n", arguments[2],
                         signature.name);
            return 5;
        }
        if (!signature.hookSite) continue;
        std::uint8_t saved[5];
        std::memcpy(saved, &byte, 5);
        const std::uint8_t jump[5] = {0xE9, 0x11, 0x22, 0x33, 0x44};
        std::memcpy(&byte, jump, 5);
        const bool jumpAccepted = swtfu::ValidateLayout(reader, failure, sizeof(failure));
        std::memcpy(&byte, saved, 5);
        if (jumpAccepted || std::strncmp(failure, "foreign patch", 13) != 0) {
            std::fprintf(stderr, "[%ls] a jump over %s was not reported as a foreign patch: %s\n",
                         arguments[2], signature.name, failure);
            return 7;
        }
    }
    std::wprintf(L"[%ls] layout accepted; corrupted signatures rejected; foreign patches recognized.\n",
                 arguments[2]);
    return 0;
}
