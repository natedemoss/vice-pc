// Vice PC — milestone 1: map an Xbox Series (GDK) title executable into a
// Windows process, satisfy what Windows can, stub/log everything Xbox-only, and run it.

#include "hle/hle.h"
#include "log.h"
#include "pe_loader.h"
#include "stubs.h"
#include "tls.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

LoadedImage g_img;
void print_backtrace(CONTEXT ctx);

// DLLs that exist only on Xbox (or ship as Xbox builds inside the game package).
// Never load these from disk; every function is HLE'd or stubbed.
const char* kXboxOnlyDlls[] = {
    "xmem.dll",        "d3d12_xs.dll",       "xg_xs.dll",
    "chad.dll",        "xcrdapi.dll",        "xapu.dll",
    // gameinput.dll is NOT here: Windows ships GameInput with the same v0 interface,
    // and GameInputCreate is ordinal 1 on both platforms.
    "acphal.dll",      "xboxbvhbuilder.dll",
    "amd_fidelityfx_loader_xbox.dll",        "amd_fidelityfx_upscaler_xbox.dll",
    "bink2scarlett.dll", "libhttpclient.gdk.dll", "xcurl.dll",
};

void usage() {
    printf(
        "usage: vice-pc <game.exe> [options]\n"
        "  --no-run             map and resolve imports, print a report, do not execute\n"
        "  --stop-on-stub       stop at the first call into an unimplemented Xbox function\n"
        "  --break-on-int3      don't step over the title's __debugbreak() calls\n"
        "  --d3d-debug          enable the D3D12 debug layer\n"
        "  --trap-fastfail      turn the title's __fastfail calls into logged exceptions\n"
        "  --trace-d3d          log every D3D12 method the title calls (flushed per call)\n"
        "  --stub-dll <name>    also treat <name> as Xbox-only (repeatable)\n"
        "  --root <dir>         game package root (default: folder with MicrosoftGame.Config)\n"
        "  --log <file>         log file (default: vice-pc.log)\n");
}

bool g_skip_int3 = true;

// For title asserts: print any register / stack slot that points at a readable string
// (asset names, assert messages), so the log says what the title was looking for.
bool printable_at(const void* p, char* out, size_t cap) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    auto* s = static_cast<const char*>(p);
    size_t avail = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize - s;
    size_t i = 0;
    for (; i < cap - 1 && i < avail; ++i) {
        char c = s[i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7e) return false;
        out[i] = c;
    }
    out[i] = 0;
    return i >= 4;
}

void log_string_args(const CONTEXT& c) {
    const DWORD64 regs[] = {c.Rcx, c.Rdx, c.R8, c.R9, c.Rbx, c.Rsi, c.Rdi, c.R12, c.R13, c.R14, c.R15};
    const char* names[] = {"rcx", "rdx", "r8", "r9", "rbx", "rsi", "rdi", "r12", "r13", "r14", "r15"};
    char buf[160];
    for (int i = 0; i < 11; ++i)
        if (printable_at(reinterpret_cast<const void*>(regs[i]), buf, sizeof(buf))) LOG("        %s -> \"%s\"", names[i], buf);
    auto* stack = reinterpret_cast<const DWORD64*>(c.Rsp);
    for (int i = 0; i < 48; ++i)
        if (printable_at(reinterpret_cast<const void*>(stack[i]), buf, sizeof(buf)))
            LOG("        [rsp+%02x] -> \"%s\"", i * 8, buf);
}
bool g_trap_fastfail = false;

// VICEPC_BP=<rva>[,<rva>...]: one-shot breakpoints for diagnostics. The byte at each RVA is
// replaced with int3; on hit, registers / strings / backtrace are logged, the original
// byte is restored and execution continues.
struct OneShotBp { uint64_t rva; uint8_t orig; bool armed; int hits; };
volatile LONG g_bp_rearm = -1;  // breakpoint to re-insert after the single step (diagnostics only)
OneShotBp g_bps[8];
int g_nbps = 0;

void arm_breakpoints(LoadedImage& img) {
    char v[256] = "";
    if (!GetEnvironmentVariableA("VICEPC_BP", v, sizeof(v))) return;
    for (char* p = v; *p && g_nbps < 8;) {
        uint64_t rva = strtoull(p, &p, 16);
        if (rva && rva < img.size) {
            DWORD old;
            VirtualProtect(img.base + rva, 1, PAGE_EXECUTE_READWRITE, &old);
            g_bps[g_nbps] = {rva, img.base[rva], true, 0};
            img.base[rva] = 0xCC;
            VirtualProtect(img.base + rva, 1, old, &old);
            LOG("[bp] armed game+0x%llx", (unsigned long long)rva);
            ++g_nbps;
        }
        while (*p == ',' || *p == ' ') ++p;
    }
}

// --trap-fastfail: __fastfail (int 29h) ends the process without running any exception
// handler. Rewrite the title's "mov ecx, code; int 29h" sequences to "mov ecx, code; ud2"
// so the vectored handler can log the reason code and a backtrace. Diagnostic only.
int patch_fastfails(LoadedImage& img) {
    int n = 0;
    auto* sec = IMAGE_FIRST_SECTION(img.nt);
    for (int s = 0; s < img.nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = img.base + sec->VirtualAddress;
        size_t len = sec->Misc.VirtualSize;
        for (size_t i = 0; i + 7 <= len; ++i) {
            if (p[i] == 0xB9 && p[i + 2] == 0 && p[i + 3] == 0 && p[i + 4] == 0 && p[i + 5] == 0xCD &&
                p[i + 6] == 0x29) {
                p[i + 5] = 0x0F;
                p[i + 6] = 0x0B;
                ++n;
            }
        }
    }
    return n;
}

LONG CALLBACK vectored_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Titles use __debugbreak() as "tell the debugger" on non-fatal failures and then
    // carry on. With no debugger attached that would kill the process, so log it and
    // step over the 1-byte int3.
    if (code == EXCEPTION_BREAKPOINT && g_img.contains(ep->ExceptionRecord->ExceptionAddress)) {
        uint64_t rva = g_img.rva_of(ep->ExceptionRecord->ExceptionAddress);
        for (int i = 0; i < g_nbps; ++i) {
            if (g_bps[i].armed && g_bps[i].rva == rva) {
                const CONTEXT& c = *ep->ContextRecord;
                int hit = ++g_bps[i].hits;
                if (hit <= 400) {
                    char mem[200] = "";
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (VirtualQuery(reinterpret_cast<void*>(c.R8), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
                        auto* b = reinterpret_cast<const uint8_t*>(c.R8);
                        size_t len = 0;
                        for (int k = 0; k < 32; ++k) len += snprintf(mem + len, sizeof(mem) - len, "%02x", b[k]);
                    }
                    LOG("[bp] hit #%d game+0x%llx (thread %lu) rcx=%llx rdx=%llx r8=%llx r9=%llx [r8]=%s", hit,
                        (unsigned long long)rva, GetCurrentThreadId(), c.Rcx, c.Rdx, c.R8, c.R9, mem);
                    if (hit == 1) { log_string_args(c); print_backtrace(c); }
                    log_flush();
                }
                // re-arm after one single step
                ep->ContextRecord->EFlags |= 0x100;
                InterlockedExchange(&g_bp_rearm, i);
                DWORD old;
                VirtualProtect(g_img.base + rva, 1, PAGE_EXECUTE_READWRITE, &old);
                g_img.base[rva] = g_bps[i].orig;
                VirtualProtect(g_img.base + rva, 1, old, &old);
                FlushInstructionCache(GetCurrentProcess(), g_img.base + rva, 1);
                return EXCEPTION_CONTINUE_EXECUTION;  // Rip already points at the restored instruction
            }
        }
    }
    if (code == EXCEPTION_BREAKPOINT && g_skip_int3 && g_img.contains(ep->ExceptionRecord->ExceptionAddress)) {
        static std::atomic<int> skipped{0};
        int n = skipped.fetch_add(1);
        if (n < 16) {
            LOG("[int3] debug break at game+0x%llx, skipping", (unsigned long long)g_img.rva_of(ep->ExceptionRecord->ExceptionAddress));
            if (n < 4) print_backtrace(*ep->ContextRecord);
            log_string_args(*ep->ContextRecord);
        }
        ep->ContextRecord->Rip += 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_bp_rearm >= 0 && !(ep->ContextRecord->Dr6 & 0xF)) {
        LONG i = InterlockedExchange(&g_bp_rearm, -1);
        if (i >= 0) {
            DWORD old;
            uint8_t* at = g_img.base + g_bps[i].rva;
            VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old);
            *at = 0xCC;
            VirtualProtect(at, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, 1);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 0xF)) {
        static std::atomic<int> hits{0};
        if (hits.fetch_add(1) < 8) {
            LOG("[watch] write to watched object (Dr6=0x%llx) at %s0x%llx", (unsigned long long)ep->ContextRecord->Dr6,
                g_img.contains(ep->ExceptionRecord->ExceptionAddress) ? "game+" : "",
                g_img.contains(ep->ExceptionRecord->ExceptionAddress)
                    ? (unsigned long long)g_img.rva_of(ep->ExceptionRecord->ExceptionAddress)
                    : (unsigned long long)ep->ExceptionRecord->ExceptionAddress);
            print_backtrace(*ep->ContextRecord);
            log_flush();
        }
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Ignore C++ throws, debugger prints and thread naming: games use these normally.
    if (code == 0xE06D7363 || code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C ||
        code == 0x406D1388 || code == DBG_CONTROL_C) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1) < 20) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        if (g_img.contains(addr)) {
            LOG("[exception] 0x%08lx at game+0x%llx (first chance, thread %lu, tls[0]=%p)", code, (unsigned long long)g_img.rva_of(addr), GetCurrentThreadId(), reinterpret_cast<void**>(__readgsqword(0x58))[0]);
            LOG("            dr0=%llx dr7=%llx", (unsigned long long)ep->ContextRecord->Dr0, (unsigned long long)ep->ContextRecord->Dr7);
        } else {
            LOG("[exception] 0x%08lx at 0x%p (first chance)", code, addr);
        }
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            LOG("            %s address 0x%llx",
                ep->ExceptionRecord->ExceptionInformation[0] == 8   ? "execute"
                : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write"
                                                                    : "read",
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
        }
        // Heap corruption fails fast and never reaches the unhandled filter, so trace it here.
        // Breakpoints are the title's asserts / fatal-error paths: trace those too.
        if (code == EXCEPTION_ILLEGAL_INSTRUCTION && g_trap_fastfail && g_img.contains(addr)) {
            LOG("[fastfail] title __fastfail(%llu)", (unsigned long long)(ep->ContextRecord->Rcx & 0xFFFFFFFF));
        }
        if (code == 0xC0000374 || code == EXCEPTION_BREAKPOINT || code == EXCEPTION_ACCESS_VIOLATION ||
            code == EXCEPTION_ILLEGAL_INSTRUCTION) {
            print_backtrace(*ep->ContextRecord);
            log_flush();
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void log_address(int frame, DWORD64 pc) {
    if (g_img.contains(reinterpret_cast<void*>(pc))) {
        LOG("        #%-2d game+0x%llx", frame, (unsigned long long)g_img.rva_of(reinterpret_cast<void*>(pc)));
        return;
    }
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &mod)) {
        GetModuleFileNameA(mod, name, sizeof(name));
        const char* base = strrchr(name, '\\');
        LOG("        #%-2d %s+0x%llx", frame, base ? base + 1 : name, (unsigned long long)(pc - (DWORD64)mod));
    } else {
        LOG("        #%-2d 0x%llx", frame, (unsigned long long)pc);
    }
}

// Walks the stack with the unwind tables (ours are registered via RtlAddFunctionTable).
void print_backtrace(CONTEXT ctx) {
    LOG("        backtrace:");
    for (int frame = 0; frame < 24 && ctx.Rip; ++frame) {
        log_address(frame, ctx.Rip);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (!fn) {
            // Leaf function: return address is at [rsp].
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
    }
}

}  // namespace

// Backtrace of the calling thread (used by HLE fail-fast traps, see hle/crt.cpp).
void hle_log_backtrace(const char* why) {
    LOG("[fatal] %s", why);
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    print_backtrace(ctx);
    stub_print_summary();
    log_flush();
}

namespace {

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    LOG("[crash] unhandled exception 0x%08lx at %s0x%llx", ep->ExceptionRecord->ExceptionCode,
        g_img.contains(addr) ? "game+" : "", g_img.contains(addr) ? (unsigned long long)g_img.rva_of(addr)
                                                                   : (unsigned long long)addr);
    auto* c = ep->ContextRecord;
    LOG("        rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    LOG("        rsi=%016llx rdi=%016llx rsp=%016llx rbp=%016llx", c->Rsi, c->Rdi, c->Rsp, c->Rbp);
    LOG("        r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx", c->R8, c->R9, c->R10, c->R11);
    LOG("        r12=%016llx r13=%016llx r14=%016llx r15=%016llx", c->R12, c->R13, c->R14, c->R15);
    print_backtrace(*c);
    stub_print_summary();
    log_flush();
    return EXCEPTION_EXECUTE_HANDLER;
}

fs::path find_package_root(const fs::path& exe) {
    for (fs::path dir = exe.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / "MicrosoftGame.Config")) return dir;
        if (dir == dir.root_path()) break;
    }
    return exe.parent_path();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring exe_path, root_arg, log_path = L"vice-pc.log";
    bool no_run = false;
    ImportOptions opt;
    for (auto d : kXboxOnlyDlls) opt.stub_dlls.emplace_back(d);

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--no-run") no_run = true;
        else if (a == L"--stop-on-stub") opt.stop_on_stub = true;
        else if (a == L"--break-on-int3") g_skip_int3 = false;
        else if (a == L"--d3d-debug") SetEnvironmentVariableW(L"VICEPC_D3D_DEBUG", L"1");
        else if (a == L"--trap-fastfail") g_trap_fastfail = true;
        else if (a == L"--trace-d3d") SetEnvironmentVariableW(L"VICEPC_TRACE_D3D", L"1");
        else if (a == L"--stub-dll" && i + 1 < argc) {
            std::wstring w = argv[++i];
            std::string s;
            for (wchar_t wc : w) s.push_back(static_cast<char>(wc));  // DLL names are ASCII
            for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
            opt.stub_dlls.push_back(s);
        } else if (a == L"--root" && i + 1 < argc) root_arg = argv[++i];
        else if (a == L"--log" && i + 1 < argc) log_path = argv[++i];
        else if (a == L"--help" || a == L"-h") { usage(); return 0; }
        else if (exe_path.empty() && a.rfind(L"--", 0) != 0) exe_path = a;
        else { usage(); return 1; }
    }
    if (exe_path.empty()) { usage(); return 1; }

    log_init(log_path.c_str());
    LOG("Vice PC (milestone 1)");

    fs::path exe = fs::absolute(exe_path);
    fs::path root = root_arg.empty() ? find_package_root(exe) : fs::absolute(root_arg);
    LOG("game exe : %ls", exe.c_str());
    LOG("pkg root : %ls", root.c_str());

    if (!pe_map(exe.wstring(), g_img)) return 2;
    stub_set_image_range(g_img.base, g_img.size);
    stub_set_stop_mode(opt.stop_on_stub);
    hle_set_image_base(g_img.base, g_img.size);
    hle_set_game_path(exe.wstring());
    hle_set_package_root(root.wstring());

    ImportStats st;
    if (!pe_resolve_imports(g_img, opt, st)) return 2;
    LOG("[imports] %d DLLs: %d native, %d HLE, %d stubbed", st.dlls, st.resolved_native, st.resolved_hle, st.stubbed);

    if (!pe_register_unwind(g_img)) return 2;
    if (!tls_setup(g_img)) return 2;
    if (g_trap_fastfail) LOG("[ldr] --trap-fastfail: %d __fastfail sites patched", patch_fastfails(g_img));
    pe_protect_sections(g_img);
    arm_breakpoints(g_img);

    if (no_run) {
        LOG("--no-run: image is mapped and linked; not executing.");
        return 0;
    }

    AddVectoredExceptionHandler(1, vectored_handler);
    SetUnhandledExceptionFilter(unhandled_filter);
    SetCurrentDirectoryW(root.c_str());

    LOG("[run] TLS callbacks (process attach)");
    tls_run_process_attach();
    LOG("[run] jumping to entry point game+0x%llx", (unsigned long long)g_img.rva_of(g_img.entry));
    log_flush();

    using EntryFn = int (*)();
    int rc = reinterpret_cast<EntryFn>(g_img.entry)();

    // Normally the game's CRT calls exit() and we never get here.
    LOG("[run] entry point returned %d", rc);
    return rc;
}
