// Crash report with a minidump written by a separate process (T028, FR-062).
//
// A crashing process cannot reliably write its own dump (its heap and locks may be corrupt), so the exception filter
//  1. writes a text report next to the dump (exception, host registers, every guest thread),
//  2. starts a second copy of this executable as "xcde --minidump <pid> <tid> <EXCEPTION_POINTERS address> <file>" and waits,
//  3. the helper opens the crashed process and calls MiniDumpWriteDump on it (ClientPointers = TRUE).
// Files go to crash/ (git-ignored: a dump holds process memory and local paths; never attach it anywhere unreviewed).
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <dbghelp.h>

#include "../kernel.h"
#include "../rt.h"
#include "diag.h"

int diag_minidump_helper(int argc, char **argv) {
    // argv: xcde --minidump <pid> <tid> <ptr> <file>
    if (argc < 6) return 2;
    DWORD pid = (DWORD)strtoul(argv[2], NULL, 10), tid = (DWORD)strtoul(argv[3], NULL, 10);
    uint64_t ptr = strtoull(argv[4], NULL, 0);
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    HANDLE file = CreateFileA(argv[5], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!proc || file == INVALID_HANDLE_VALUE) return 3;
    MINIDUMP_EXCEPTION_INFORMATION ei = {.ThreadId = tid, .ExceptionPointers = (PEXCEPTION_POINTERS)(uintptr_t)ptr, .ClientPointers = TRUE};
    MINIDUMP_TYPE type = MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules;
    BOOL ok = MiniDumpWriteDump(proc, pid, file, type, ptr ? &ei : NULL, NULL, NULL);
    CloseHandle(file);
    CloseHandle(proc);
    return ok ? 0 : 4;
}

// Called from the unhandled-exception filter of the crashing thread. Returns after the dump (or after giving up).
void diag_crash_report(EXCEPTION_POINTERS *ep) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return;  // a second crash while reporting: skip
    if (getenv("XCDE_NO_MINIDUMP")) return;
    CreateDirectoryA("crash", NULL);
    time_t t = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", localtime(&t));
    char txt[96], dmp[96];
    snprintf(txt, sizeof txt, "crash/crash_%s.txt", stamp);
    snprintf(dmp, sizeof dmp, "crash/crash_%s.dmp", stamp);

    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    FILE *f = fopen(txt, "w");
    if (f) {
        fprintf(f, "xcde crash report\nexception %08lx at host pc %p", er->ExceptionCode, er->ExceptionAddress);
        if (er->NumberParameters >= 2) fprintf(f, ", %s address %llx", er->ExceptionInformation[0] == 1 ? "write to" : er->ExceptionInformation[0] == 8 ? "execute" : "read of",
                                               (unsigned long long)er->ExceptionInformation[1]);
        fprintf(f, "\nlast presented frame %llu\n", (unsigned long long)diag_frame());
        CONTEXT *c = ep->ContextRecord;
        fprintf(f, "rip %016llx rsp %016llx rbp %016llx\nrax %016llx rbx %016llx rcx %016llx rdx %016llx\n", (unsigned long long)c->Rip,
                (unsigned long long)c->Rsp, (unsigned long long)c->Rbp, (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
                (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
        fprintf(f, "\nguest threads:\n");
        kernel_dump_threads(f);
        fprintf(f, "\nminidump: %s\n", dmp);
        fclose(f);
    }

    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, sizeof exe)) return;
    char cmd[MAX_PATH + 256];
    snprintf(cmd, sizeof cmd, "\"%s\" --minidump %lu %lu 0x%llx \"%s\"", exe, GetCurrentProcessId(), GetCurrentThreadId(),
             (unsigned long long)(uintptr_t)ep, dmp);
    STARTUPINFOA si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[crash] cannot start the minidump helper (error %lu)\n", GetLastError());
        return;
    }
    DWORD w = WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    fprintf(stderr, "[crash] report %s, minidump %s (%s, helper exit %lu)\n", txt, dmp, w == WAIT_OBJECT_0 ? "written" : "timed out", code);
}
#else
#include "diag.h"
int diag_minidump_helper(int argc, char **argv) { (void)argc, (void)argv; return 2; }
#endif
