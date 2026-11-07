// [XCDE] LINUX-007: real Linux fatal-notice function, only the modal SDL call mocked.
// Section GC removes unrelated platform entry points; no game/GPU or display.
#include "../../runtime/platform/linux.c"

static unsigned dialog_calls;
static int dialog_arguments_ok = 1;

bool SDLCALL SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title,
                                    const char *message, SDL_Window *window) {
    ++dialog_calls;
    if (flags != SDL_MESSAGEBOX_ERROR || strcmp(title, "LAS Recomp") || window ||
        !strstr(message, "stopped because of an error") || !strstr(message, g_log_path))
        dialog_arguments_ok = 0;
    return true;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    if (!strcmp(argv[1], "unset")) {
        if (unsetenv("XCDE_NO_ERROR_DIALOG")) return 3;
    } else if (setenv("XCDE_NO_ERROR_DIALOG", argv[1], 1)) return 4;
    snprintf(g_log_path, sizeof g_log_path, "synthetic-state/logs/monado.log");
    if (!freopen(argv[2], "wx", stderr)) return 5;
    static char buffer[1024];
    if (setvbuf(stderr, buffer, _IOFBF, sizeof buffer)) return 6;
    const char marker[] = "synthetic fatal details remain in the log\n";
    if (fputs(marker, stderr) < 0) return 7;
    plat_fatal_notice();
    struct stat st;
    if (fstat(fileno(stderr), &st) || st.st_size != sizeof marker - 1) return 8;
    if (dialog_calls != (unsigned)strtoul(argv[3], NULL, 10) || !dialog_arguments_ok) return 9;
    // A fatal caller still exits unsuccessfully after the notification returns.
    return 37;
}
