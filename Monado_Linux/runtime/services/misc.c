// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include "../diag/diag.h"
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    diag_unimplemented("ipc", "[svc] %s cmd %u", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
// [XCDE] LINUX-008: native client lifecycle only. There is no native parental
// policy provider, console policy database or enforcement backend. Initialize
// binds a live client to that explicit capability; it does not disable policy.
// The legacy policy queries below remain unaudited stubs, not part of this fix.
typedef struct PctlState {
    Service *owner;
    struct PctlState *next;
    uint64_t guest_pid;
    bool initialized;
    enum { PCTL_PROVIDER_UNBOUND, PCTL_PROVIDER_UNAVAILABLE } provider;
} PctlState;
static PctlState *pctl_clients; // g_klock; reset runs after guest threads stop

static void pctl_initialize_locked(PctlState *state) {
    if (state->initialized) return; // idempotent native client initialization
    state->provider = PCTL_PROVIDER_UNAVAILABLE;
    state->initialized = true;
    LOG("[pctl] native client initialized for guest PID %llu: parental-policy provider unavailable; enforcement unsupported\n",
        (unsigned long long)state->guest_pid);
}

static void pctl_close(Service *s) {
    pthread_mutex_lock(&g_klock);
    PctlState *state = s->state;
    if (state) {
        PctlState **link = &pctl_clients;
        while (*link && *link != state) link = &(*link)->next;
        if (*link) *link = state->next;
        s->state = NULL;
        free(state);
    }
    pthread_mutex_unlock(&g_klock);
}

RT_ON_RESET(pctl_reset) {
    // Kernel reset retains Service/KSession objects. Detach their state before
    // freeing it, so a later final-close callback cannot free it twice.
    pthread_mutex_lock(&g_klock);
    while (pctl_clients) {
        PctlState *state = pctl_clients;
        pctl_clients = state->next;
        state->owner->state = NULL;
        free(state);
    }
    pthread_mutex_unlock(&g_klock);
}

static void pctl_check_request(IpcReq *req, uint32_t bytes, bool pid) {
    if (req->data_size != bytes || req->has_pid != pid || req->ncopy || req->nmove ||
        req->nx || req->na || req->nb || req->nc || req->ndomain_in)
        FATAL("[pctl] malformed cmd %u: expected %u payload bytes, send-PID=%u and no other descriptors\n",
              req->cmd, bytes, pid);
}

static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    pthread_mutex_lock(&g_klock);
    PctlState *state = s->state;
    if (!state) FATAL("[pctl] closed or reset client\n");
    if (req->cmd == 1) {
        pctl_check_request(req, 0, false);
        pctl_initialize_locked(state);
        pthread_mutex_unlock(&g_klock);
        return; // CMIF Result only; no data, objects or handles
    }
    bool initialized = state->initialized;
    pthread_mutex_unlock(&g_klock);
    switch (req->cmd) {
    case 1031: case 1035: case 1018:
        if (!initialized) FATAL("[pctl] query before client initialization\n");
        OUT(uint8_t, 0) = 0; resp_size(resp, 1); break; // inherited, unaudited policy stubs
    case 1032:
        if (!initialized) FATAL("[pctl] query before client initialization\n");
        OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: diag_unimplemented("ipc", "[pctl] IParentalControlService cmd %u", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd != 0 && req->cmd != 1) {
        diag_unimplemented("ipc", "[pctl] IParentalControlServiceFactory cmd %u", req->cmd);
        return;
    }
    pctl_check_request(req, 8, true);
    uint64_t reserved;
    memcpy(&reserved, req->data, sizeof reserved);
    if (reserved) FATAL("[pctl] nonzero factory reserved input\n");
    PctlState *state = calloc(1, sizeof *state);
    if (!state) FATAL("[pctl] cannot allocate native client\n");
    Service *child = service_new("IParentalControlService", pctl_service, state);
    child->on_close = pctl_close;
    state->owner = child;
    // IPC currently preserves the guest's PID placeholder rather than kernel
    // substitution. This runtime has one guest process: the same synthetic
    // identity returned by GetProcessId, never an arbitrary descriptor/host PID.
    state->guest_pid = 0x81;
    pthread_mutex_lock(&g_klock);
    state->next = pctl_clients;
    pctl_clients = state;
    if (req->cmd == 0) pctl_initialize_locked(state); // legacy CreateService
    pthread_mutex_unlock(&g_klock);
    ipc_return_object(resp, child); // cmd1: CreateServiceWithoutInitialize
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

#ifdef __linux__
// [XCDE] LINUX-010: the native port does not collect console play reports.
// SaveReportWithUserOld2 has Uid + u64, send-PID, an X name and an A report
// buffer. Validate the IPC envelope, then deny before inspecting either input.
// PermissionDenied is Prepo module129/description90, not a successful Save.
// This is our native policy, not an emulation of Horizon's offline settings.
SERVICE("prepo:u", prepo_u) {
    (void)s;
    const uint32_t permission_denied = (90u << 9) | 129u;
    if (req->cmd != 10103) {
        diag_unimplemented("ipc", "[prepo:u] cmd %u", req->cmd);
        resp->result = permission_denied; // never fabricate success outside strict mode
        return;
    }
    if (req->data_size != 24 || !req->has_pid || req->nx != 1 || req->na != 1 ||
        req->nb || req->nc || req->ncopy || req->nmove || req->ndomain_in)
        FATAL("[prepo:u] malformed SaveReportWithUserOld2 envelope: expected 24 bytes, send-PID, X1/A1 only\n");
    LOG("[prepo:u] SaveReportWithUserOld2 denied: native play-report collection disabled (PermissionDenied 0xB481)\n");
    resp->result = permission_denied;
}
#endif

// ------------------------------------------------------------------ aoc:u (no DLC)
SERVICE("aoc:u", aoc_u) {
    (void)s;
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;   // CountAddOnContent
    case 3: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;   // ListAddOnContent
    case 5: OUT(uint64_t, 0) = 0x01006BB00C6F1000ull; resp_size(resp, 8); break;  // GetAddOnContentBaseId
    case 8: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    default: ok_logger(s, req, resp);
    }
}
