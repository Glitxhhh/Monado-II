// Applet manager (appletOE), apm, and lm.
#include "../diag/diag.h"
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../settings.h"
#include "am.h"

#define AM_NO_MESSAGE MAKE_RESULT(128, 3)
#define AM_NO_DATA MAKE_RESULT(128, 2)

// [XCDE] LINUX-008: one per-boot owner, protected by g_klock. This native applet
// stays active/in focus; desktop focus, thread/fiber/render waits do not suspend
// it. Policy/notification configuration is retained, but no HOME/sleep/applet
// transition is synthesized. Suspended ticks remain zero under this model.
static struct {
    bool initialized, preselected_popped;
    KEvent *message_event, *suspended_event;
    uint32_t messages[16];
    unsigned message_count;
    uint8_t focus_policy[3], operation_mode;
    uint32_t performance_mode, idle_time_extension;
    bool out_of_focus_suspending, operation_notify, performance_notify, resume_notify;
    bool exit_locked, exit_requested, timeout_reported;
    uint64_t exit_requested_ns, suspended_ticks;
} am;
#define AM_EXIT_GRACE_NS UINT64_C(15000000000)

static KEvent *am_new_event_locked(void) {
    KEvent *event = calloc(1, sizeof *event);
    if (!event) FATAL("[am] cannot allocate lifecycle event\n");
    event->type = KOBJ_EVENT;
    return event;
}

static void am_enqueue_locked(uint32_t message) {
    if (am.message_count == sizeof am.messages / sizeof am.messages[0])
        FATAL("[am] lifecycle message queue overflow\n");
    am.messages[am.message_count++] = message;
    kernel_signal(am.message_event);
}

static void am_init_locked(void) {
    if (am.initialized) return;
    am.message_event = am_new_event_locked();
    am.suspended_event = am_new_event_locked();
    am.focus_policy[2] = 1; // default SuspendHomeSleep, not a native pause request
    am.operation_mode = getenv("XCDE_HANDHELD") ? 0 : 1;
    am.performance_mode = am.operation_mode;
    am.initialized = true;
    am_enqueue_locked(15); // initial FocusStateChanged: already InFocus
}

RT_ON_RESET(am_reset) {  // (reboot: the message queue starts over with FocusStateChanged)
    // Guest threads have stopped. Do not reset/free the old object's refs:
    // kernel_reset drops old handles separately, retaining kernel objects.
    pthread_mutex_lock(&g_klock);
    memset(&am, 0, sizeof am);
    pthread_mutex_unlock(&g_klock);
}

static void am_init_once(void) {
    pthread_mutex_lock(&g_klock);
    am_init_locked();
    pthread_mutex_unlock(&g_klock);
}

static uint32_t am_event_handle(bool suspended) {
    pthread_mutex_lock(&g_klock);
    am_init_locked();
    KEvent *event = suspended ? am.suspended_event : am.message_event;
    pthread_mutex_unlock(&g_klock);
    // Objects are retained, and reset requires stopped guest threads. The
    // allocator takes g_klock itself; each reply owns an independent handle.
    return kernel_new_handle(event);
}

void am_request_host_exit(uint64_t now_ns) {
    pthread_mutex_lock(&g_klock);
    am_init_locked();
    if (!am.exit_requested) {
        am.exit_requested = true;
        am.exit_requested_ns = now_ns;
        am_enqueue_locked(4); // ExitRequest; repeated native closes coalesce
        LOG("[am] native close requested (exit lock %s)\n", am.exit_locked ? "held" : "clear");
    }
    pthread_mutex_unlock(&g_klock);
}

bool am_host_exit_ready(uint64_t now_ns) {
    pthread_mutex_lock(&g_klock);
    // Querying before the service is opened must not change its initial state.
    bool expired = am.exit_requested && now_ns >= am.exit_requested_ns &&
        now_ns - am.exit_requested_ns >= AM_EXIT_GRACE_NS;
    bool ready = am.exit_requested && (!am.exit_locked || expired);
    if (expired && am.exit_locked && !am.timeout_reported) {
        am.timeout_reported = true;
        LOG("[am] native close: 15-second guest exit-lock deadline expired; cleanup not confirmed\n");
    }
    pthread_mutex_unlock(&g_klock);
    return ready;
}

static void am_validate_flags(IpcReq *req, unsigned count) {
    if (req->data_size < count)
        FATAL("[am] malformed ISelfController cmd %u: %u payload bytes, need %u\n", req->cmd, req->data_size, count);
    for (unsigned i = 0; i < count; ++i)
        if (req->data[i] > 1)
            FATAL("[am] malformed ISelfController cmd %u: flag %u is not boolean\n", req->cmd, i);
}

#ifdef __linux__
static void am_validate_idle_request(IpcReq *req, uint32_t bytes) {
    if (req->data_size != bytes || req->has_pid || req->ncopy || req->nmove ||
        req->nx || req->na || req->nb || req->nc || req->ndomain_in)
        FATAL("[am] malformed idle-time cmd %u: expected %u payload bytes and no descriptors\n", req->cmd, bytes);
}
#endif

static uint32_t new_event(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

#define UNHANDLED(iface) diag_unimplemented("ipc", "[am] %s cmd %u", iface, req->cmd)

static void common_state_getter(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = am_event_handle(false); break;  // GetEventHandle
    case 1:  // ReceiveMessage
        pthread_mutex_lock(&g_klock);
        am_init_locked();
        if (am.message_count) {
            OUT(uint32_t, 0) = am.messages[0];
            memmove(am.messages, am.messages + 1, --am.message_count * sizeof(uint32_t));
            if (!am.message_count) am.message_event->signaled = false;
            resp_size(resp, 4);
        } else {
            resp->result = AM_NO_MESSAGE;
        }
        pthread_mutex_unlock(&g_klock);
        break;
    // [XCDE] T146 (test): XCDE_HANDHELD=1 reports handheld mode and a 1280x720 display (UI anchored to the bottom /
    // right edges lands off screen in docked mode: is the layout computed for the reported display size?)
    case 5: case 6: // Boot configuration is snapshotted; desktop fullscreen does not change it.
        pthread_mutex_lock(&g_klock);
        am_init_locked();
        if (req->cmd == 5) { OUT(uint8_t, 0) = am.operation_mode; resp_size(resp, 1); }
        else { OUT(uint32_t, 0) = am.performance_mode; resp_size(resp, 4); }
        pthread_mutex_unlock(&g_klock);
        break;
    case 8: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;   // GetBootMode
    case 9: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;   // GetCurrentFocusState: in focus
    case 13: case 61: resp->copy_handles[resp->ncopy++] = new_event(); break;
    case 60: {  // GetDefaultDisplayResolution
        int w = getenv("LARECOMP_DISPLAY_W") ? atoi(getenv("LARECOMP_DISPLAY_W")) : getenv("XCDE_HANDHELD") ? 1280 : 1920;
        int h = getenv("XCDE_HANDHELD") ? 720 : 1080;
        LOG("[am] GetDefaultDisplayResolution -> %dx%d\n", w, h);
        OUT(int32_t, 0) = w; OUT(int32_t, 4) = h; resp_size(resp, 8); break;
    }
    default: UNHANDLED("ICommonStateGetter");
    }
}

static void self_controller(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: LOG("[am] application requested exit\n"); exit(0);
    case 1: case 2: // LockExit / UnlockExit: no payload; guest cleanup continues while locked.
        pthread_mutex_lock(&g_klock);
        am_init_locked();
        am.exit_locked = req->cmd == 1;
        pthread_mutex_unlock(&g_klock);
        break;
    case 9: resp->copy_handles[resp->ncopy++] = new_event(); break;
    case 11: case 12: case 13: case 14: case 16: {
        unsigned count = req->cmd == 13 ? 3 : 1;
        am_validate_flags(req, count);
        pthread_mutex_lock(&g_klock);
        am_init_locked();
        switch (req->cmd) {
        case 11: am.operation_notify = req->data[0] != 0; break;
        case 12: am.performance_notify = req->data[0] != 0; break;
        case 13: memcpy(am.focus_policy, req->data, 3); break;
        case 14: am.resume_notify = req->data[0] != 0; break;
        case 16: am.out_of_focus_suspending = req->data[0] != 0; break;
        }
        LOG("[am] active-only policy cmd %u: %u%s", req->cmd, req->data[0], count == 3 ? "," : "\n");
        if (count == 3) LOG("%u,%u\n", req->data[1], req->data[2]);
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 40: OUT(uint64_t, 0) = 1; resp_size(resp, 8); break;  // CreateManagedDisplayLayer
#ifdef __linux__
    case 62: { // [XCDE] LINUX-010: SetIdleTimeDetectionExtension (None/Extended/ExtendedUnsafe).
        am_validate_idle_request(req, 4);
        uint32_t mode;
        memcpy(&mode, req->data, sizeof mode);
        if (mode > 2) FATAL("[am] invalid idle-time extension %u; expected 0..2\n", mode);
        pthread_mutex_lock(&g_klock);
        am_init_locked();
        am.idle_time_extension = mode;
        pthread_mutex_unlock(&g_klock);
        // SDL already requests display-idle inhibition for the native game's
        // lifetime. Retain the guest enum for queries without inventing console
        // sleep timers or calling SDL from a guest thread. This is not a promise
        // that the desktop compositor/OS honors inhibition or disables suspend.
        LOG("[am] idle-time extension %u retained; native SDL lifetime idle policy unchanged\n", mode);
        break;
    }
    case 63: // GetIdleTimeDetectionExtension: shared applet state, no input.
        am_validate_idle_request(req, 0);
        pthread_mutex_lock(&g_klock);
        am_init_locked();
        OUT(uint32_t, 0) = am.idle_time_extension;
        pthread_mutex_unlock(&g_klock);
        resp_size(resp, 4);
        break;
#endif
    case 69: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 90:  // GetAccumulatedSuspendedTickValue (19.2 MHz system ticks)
        pthread_mutex_lock(&g_klock);
        am_init_locked();
        OUT(uint64_t, 0) = am.suspended_ticks;
        pthread_mutex_unlock(&g_klock);
        resp_size(resp, 8);
        break;
    case 91:  // GetAccumulatedSuspendedTickChangedEvent (client autoclear)
        resp->copy_handles[resp->ncopy++] = am_event_handle(true);
        break;
    default: UNHANDLED("ISelfController");
    }
}

static void window_controller(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: OUT(uint64_t, 0) = 0x1234; resp_size(resp, 8); break;  // GetAppletResourceUserId
    case 10:  // AcquireForegroundRights: no input/output, idempotent.
        // [XCDE] LINUX-007: the single active application already owns applet
        // foreground (CommonStateGetter reports InFocus before this call).
        // Reacquisition does not change focus or post another notification.
        // This is guest applet ownership, not desktop keyboard/window focus.
        break;
    default: UNHANDLED("IWindowController");
    }
}

static void generic_ok(Service *s, IpcReq *req, IpcResp *resp) {
    diag_unimplemented("ipc", "[am] %s cmd %u", s->name, req->cmd);
    resp_size(resp, 0x10);
}

// ------------------------------------------------------------------ IStorage / IStorageAccessor
typedef struct { uint8_t *data; uint64_t size; } AmStorage;

static void storage_accessor(Service *s, IpcReq *req, IpcResp *resp) {
    AmStorage *st = s->state;
    switch (req->cmd) {
    case 0: OUT(int64_t, 0) = (int64_t)st->size; resp_size(resp, 8); break;  // GetSize
    case 10: {  // Write(offset, A)
        int64_t off = IN(int64_t, 0);
        IpcBuf *a = in_buf(req, 0);
        uint64_t n = a->size;
        if ((uint64_t)off + n > st->size) n = st->size - (uint64_t)off;
        memcpy(st->data + off, (void *)(uintptr_t)a->addr, n);
        break;
    }
    case 11: {  // Read(offset, B)
        int64_t off = IN(int64_t, 0);
        IpcBuf *b = out_buf(req, 0);
        uint64_t n = b->size;
        if ((uint64_t)off + n > st->size) n = st->size - (uint64_t)off;
        memcpy((void *)(uintptr_t)b->addr, st->data + off, n);
        break;
    }
    default: diag_unimplemented("ipc", "[am] IStorageAccessor cmd %u", req->cmd);
    }
}

static void am_storage(Service *s, IpcReq *req, IpcResp *resp) {
    if (req->cmd == 0) ipc_return_object(resp, service_new("IStorageAccessor", storage_accessor, s->state));
    else diag_unimplemented("ipc", "[am] IStorage cmd %u", req->cmd);
}

static Service *new_am_storage(const void *data, uint64_t size) {
    AmStorage *st = calloc(1, sizeof *st);
    st->data = calloc(1, size);
    st->size = size;
    if (data) memcpy(st->data, data, size);
    return service_new("IStorage", am_storage, st);
}

extern const uint8_t g_user_id[16];

static void application_functions(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: { // PopLaunchParameter(kind)
        pthread_mutex_lock(&g_klock);
        am_init_locked();
        bool pop = IN(uint32_t, 0) == 2 && !am.preselected_popped;
        if (pop) am.preselected_popped = true;
        pthread_mutex_unlock(&g_klock);
        if (pop) {  // PreselectedUser
            uint8_t p[0x88] = {0};
            *(uint32_t *)p = 0xC79497CA;
            *(uint32_t *)(p + 4) = 1;
            memcpy(p + 8, g_user_id, 16);
            ipc_return_object(resp, new_am_storage(p, sizeof p));
        } else {
            resp->result = AM_NO_DATA;
        }
        break;
    }
    case 20: OUT(uint64_t, 0) = 0; resp_size(resp, 8); break;  // EnsureSaveData
    case 21: {  // GetDesiredLanguage: the game's text language (settings: runtime/settings.c, "language")
        char lang[8] = {0};
        snprintf(lang, sizeof lang, "%s", settings_language());
        memcpy(resp->data, lang, 8);
        resp_size(resp, 8);
        break;
    }
    case 23: memcpy(resp->data, "1.0.0", 6); resp_size(resp, 0x10); break;
    case 40: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // NotifyRunning
    case 130: case 140: resp->copy_handles[resp->ncopy++] = new_event(); break;
    default: UNHANDLED("IApplicationFunctions");
    }
}

static void application_proxy(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    static const struct { int cmd; const char *name; ServiceHandler h; } subs[] = {
        {0, "ICommonStateGetter", common_state_getter},
        {1, "ISelfController", self_controller},
        {2, "IWindowController", window_controller},
        {3, "IAudioController", generic_ok},
        {4, "IDisplayController", generic_ok},
        {10, "IProcessWindingController", generic_ok},
        {11, "ILibraryAppletCreator", generic_ok},
        {20, "IApplicationFunctions", application_functions},
        {1000, "IDebugFunctions", generic_ok},
    };
    for (unsigned i = 0; i < sizeof subs / sizeof *subs; i++) {
        if (subs[i].cmd == (int)req->cmd) {
            ipc_return_object(resp, service_new(subs[i].name, subs[i].h, NULL));
            return;
        }
    }
    UNHANDLED("IApplicationProxy");
}

SERVICE("appletOE", applet_oe) {
    (void)s;
    am_init_once();
    if (req->cmd == 0) ipc_return_object(resp, service_new("IApplicationProxy", application_proxy, NULL));
    else UNHANDLED("appletOE");
}

// ------------------------------------------------------------------ apm
static void apm_session(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 1) { OUT(uint32_t, 0) = 0x92220008; resp_size(resp, 4); }  // GetPerformanceConfiguration
}

SERVICE("apm", apm) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("apm:ISession", apm_session, NULL));
    else if (req->cmd == 1) { OUT(uint32_t, 0) = 1; resp_size(resp, 4); }
}

// ------------------------------------------------------------------ lm (guest log output)
static void lm_logger(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s; (void)resp;
    if (req->cmd != 0) return;
    IpcBuf *b = in_buf(req, 0);
    if (!b || !b->size) return;
    // Log packets are TLV chunks after a 0x18-byte header; print the string chunks.
    const uint8_t *p = (const uint8_t *)(uintptr_t)b->addr, *end = p + b->size;
    p += 0x18;
    char line[1024];
    int n = 0;
    while (p + 2 <= end) {
        uint8_t key = p[0], len = p[1];
        p += 2;
        if (p + len > end) break;
        if (key == 2 || key == 4 || key == 5 || key == 6 || key == 7) {  // message, file, function, module, thread
            n += snprintf(line + n, sizeof line - n, "%s%.*s", n ? " | " : "", len, (const char *)p);
            if (n >= (int)sizeof line) break;
        }
        p += len;
    }
    if (n) LOG("[lm] %s\n", line);
}

SERVICE("lm", lm) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("lm:ILogger", lm_logger, NULL));
}
