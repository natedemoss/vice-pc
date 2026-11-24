// Vice PC — milestone 1: map an Xbox Series (GDK) title executable into a
// Windows process, satisfy what Windows can, stub/log everything Xbox-only, and run it.

#include "hle/hle.h"
#include "log.h"
#include "pe_loader.h"
#include "stubs.h"
#include "tls.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

LoadedImage g_img;

// DLLs that exist only on Xbox (or ship as Xbox builds inside the game package).
// Never load these from disk; every function is HLE'd or stubbed.
const char* kXboxOnlyDlls[] = {
    "xmem.dll",        "d3d12_xs.dll",       "xg_xs.dll",
    "chad.dll",        "xcrdapi.dll",        "xapu.dll",
    "acphal.dll",      "gameinput.dll",      "xboxbvhbuilder.dll",
    "amd_fidelityfx_loader_xbox.dll",        "amd_fidelityfx_upscaler_xbox.dll",
    "bink2scarlett.dll", "libhttpclient.gdk.dll", "xcurl.dll",
};

void usage() {
    printf(
        "usage: vice-pc <game.exe> [options]\n"
        "  --no-run             map and resolve imports, print a report, do not execute\n"
        "  --stop-on-stub       stop at the first call into an unimplemented Xbox function\n"
        "  --stub-dll <name>    also treat <name> as Xbox-only (repeatable)\n"
        "  --root <dir>         game package root (default: folder with MicrosoftGame.Config)\n"
        "  --log <file>         log file (default: vice-pc.log)\n");
}

LONG CALLBACK vectored_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Ignore C++ throws, debugger prints and thread naming: games use these normally.
    if (code == 0xE06D7363 || code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C ||
        code == 0x406D1388 || code == DBG_CONTROL_C) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1) < 20) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        if (g_img.contains(addr)) {
            LOG("[exception] 0x%08lx at game+0x%llx (first chance)", code, (unsigned long long)g_img.rva_of(addr));
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
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    LOG("[crash] unhandled exception 0x%08lx at %s0x%llx", ep->ExceptionRecord->ExceptionCode,
        g_img.contains(addr) ? "game+" : "", g_img.contains(addr) ? (unsigned long long)g_img.rva_of(addr)
                                                                   : (unsigned long long)addr);
    auto* c = ep->ContextRecord;
    LOG("        rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    LOG("        rsi=%016llx rdi=%016llx rsp=%016llx rbp=%016llx", c->Rsi, c->Rdi, c->Rsp, c->Rbp);
    LOG("        r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx", c->R8, c->R9, c->R10, c->R11);
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
    hle_set_image_base(g_img.base);

    ImportStats st;
    if (!pe_resolve_imports(g_img, opt, st)) return 2;
    LOG("[imports] %d DLLs: %d native, %d HLE, %d stubbed", st.dlls, st.resolved_native, st.resolved_hle, st.stubbed);

    if (!pe_register_unwind(g_img)) return 2;
    if (!tls_setup(g_img)) return 2;
    pe_protect_sections(g_img);

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
