#include "pe_loader.h"

#include "hle/hle.h"
#include "log.h"
#include "stubs.h"

#include <algorithm>
#include <cstring>
#include <fstream>

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool pe_map(const std::wstring& path, LoadedImage& img) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        LOG("[pe] cannot open file");
        return false;
    }
    std::vector<uint8_t> file((size_t)f.tellg());
    f.seekg(0);
    f.read(reinterpret_cast<char*>(file.data()), (std::streamsize)file.size());

    if (file.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > file.size()) {
        LOG("[pe] not a PE file");
        return false;
    }
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        LOG("[pe] not an x64 PE32+ image");
        return false;
    }

    const auto& oh = nt->OptionalHeader;
    img.preferred_base = oh.ImageBase;
    img.size = oh.SizeOfImage;

    img.base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(oh.ImageBase), img.size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!img.base) {
        LOG("[pe] preferred base 0x%llx unavailable, relocating", (unsigned long long)oh.ImageBase);
        img.base = static_cast<uint8_t*>(VirtualAlloc(nullptr, img.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (!img.base) {
        LOG("[pe] VirtualAlloc of 0x%zx bytes failed (%lu)", img.size, GetLastError());
        return false;
    }

    // Headers + sections.
    memcpy(img.base, file.data(), std::min<size_t>(oh.SizeOfHeaders, file.size()));
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        size_t raw = sec->SizeOfRawData;
        if (sec->PointerToRawData + raw > file.size()) raw = file.size() - std::min<size_t>(file.size(), sec->PointerToRawData);
        if (sec->VirtualAddress + raw > img.size) raw = img.size - sec->VirtualAddress;
        if (raw) memcpy(img.base + sec->VirtualAddress, file.data() + sec->PointerToRawData, raw);
        // Remainder (virtual size > raw size) is already zero from VirtualAlloc.
    }

    img.nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(img.base + dos->e_lfanew);
    img.entry = img.base + oh.AddressOfEntryPoint;

    // Base relocations.
    int64_t delta = (int64_t)((uint64_t)img.base - img.preferred_base);
    if (delta != 0) {
        auto rd = img.dir(IMAGE_DIRECTORY_ENTRY_BASERELOC);
        if (!rd.Size) {
            LOG("[pe] image needs relocation but has no .reloc");
            return false;
        }
        uint8_t* p = img.base + rd.VirtualAddress;
        uint8_t* end = p + rd.Size;
        size_t fixups = 0;
        while (p < end) {
            auto block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(p);
            if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
            auto entries = reinterpret_cast<WORD*>(block + 1);
            size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            for (size_t i = 0; i < count; ++i) {
                WORD type = entries[i] >> 12, off = entries[i] & 0xFFF;
                if (type == IMAGE_REL_BASED_DIR64) {
                    *reinterpret_cast<uint64_t*>(img.base + block->VirtualAddress + off) += delta;
                    ++fixups;
                } else if (type != IMAGE_REL_BASED_ABSOLUTE) {
                    LOG("[pe] unsupported relocation type %u", type);
                }
            }
            p += block->SizeOfBlock;
        }
        LOG("[pe] applied %zu relocations (delta 0x%llx)", fixups, (unsigned long long)delta);
    }

    LOG("[pe] mapped at 0x%p, size 0x%zx, entry RVA 0x%x", img.base, img.size, oh.AddressOfEntryPoint);
    return true;
}

bool pe_resolve_imports(LoadedImage& img, const ImportOptions& opt, ImportStats& stats) {
    auto id = img.dir(IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (!id.Size) return true;

    for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(img.base + id.VirtualAddress); desc->Name; ++desc) {
        const char* dll = reinterpret_cast<const char*>(img.base + desc->Name);
        std::string dll_l = to_lower(dll);
        ++stats.dlls;

        bool force_stub = std::find(opt.stub_dlls.begin(), opt.stub_dlls.end(), dll_l) != opt.stub_dlls.end();
        HMODULE mod = nullptr;
        if (!force_stub) {
            // Never search the current directory: the game folder ships Xbox builds of some DLLs.
            mod = LoadLibraryExA(dll, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!mod) LOG("[imports] %s: not available on this PC, stubbing", dll);
        }

        auto name_thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            img.base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(img.base + desc->FirstThunk);

        int native = 0, hle = 0, stub = 0;
        for (; name_thunk->u1.AddressOfData; ++name_thunk, ++iat) {
            std::string fname;
            uint16_t ordinal = 0;
            FARPROC fn = nullptr;

            if (IMAGE_SNAP_BY_ORDINAL64(name_thunk->u1.Ordinal)) {
                ordinal = (uint16_t)IMAGE_ORDINAL64(name_thunk->u1.Ordinal);
                fname = "#" + std::to_string(ordinal);
                if (mod) fn = GetProcAddress(mod, MAKEINTRESOURCEA(ordinal));
            } else {
                auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(img.base + name_thunk->u1.AddressOfData);
                fname = reinterpret_cast<const char*>(ibn->Name);
                if (mod) fn = GetProcAddress(mod, fname.c_str());
            }

            void* target = nullptr;
            if (void* h = hle_lookup(dll_l.c_str(), fname.c_str())) {
                target = h;  // our own implementation wins over native/stub
                ++hle;
            } else if (fn) {
                target = reinterpret_cast<void*>(fn);
                ++native;
            } else {
                target = stub_create(dll, fname.c_str(), opt.stop_on_stub);
                ++stub;
            }
            if (!target) {
                LOG("[imports] failed to create stub for %s!%s", dll, fname.c_str());
                return false;
            }
            iat->u1.Function = reinterpret_cast<ULONGLONG>(target);
        }

        stats.resolved_native += native;
        stats.resolved_hle += hle;
        stats.stubbed += stub;
        LOG("[imports] %-48s native=%-4d hle=%-3d stub=%d", dll, native, hle, stub);
    }
    return true;
}

bool pe_register_unwind(LoadedImage& img) {
    auto ed = img.dir(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (!ed.Size) return true;
    auto table = reinterpret_cast<PRUNTIME_FUNCTION>(img.base + ed.VirtualAddress);
    DWORD count = ed.Size / sizeof(RUNTIME_FUNCTION);
    if (!RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(img.base))) {
        LOG("[pe] RtlAddFunctionTable failed");
        return false;
    }
    LOG("[pe] registered %lu unwind entries", count);
    return true;
}

void pe_protect_sections(LoadedImage& img) {
    DWORD old;
    VirtualProtect(img.base, img.nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);

    auto sec = IMAGE_FIRST_SECTION(img.nt);
    DWORD align = img.nt->OptionalHeader.SectionAlignment;
    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i, ++sec) {
        DWORD c = sec->Characteristics;
        bool x = c & IMAGE_SCN_MEM_EXECUTE, r = c & IMAGE_SCN_MEM_READ, w = c & IMAGE_SCN_MEM_WRITE;
        DWORD prot = x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                       : (w ? PAGE_READWRITE : PAGE_READONLY);
        (void)r;
        size_t vsize = std::max<size_t>(sec->Misc.VirtualSize, sec->SizeOfRawData);
        vsize = (vsize + align - 1) / align * align;
        if (vsize) VirtualProtect(img.base + sec->VirtualAddress, vsize, prot, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), img.base, img.size);
}
