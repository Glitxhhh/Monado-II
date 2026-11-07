// hwopus: the system's hardware Opus decoder, over libopus (FR-041, ADR 0008).  [XCDE]
// Wwise (inside the game) hands it Switch-format packets: 4-byte big-endian payload size, 4-byte final range, payload.
#include "../diag/diag.h"
#include <opus/opus.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

typedef struct { OpusDecoder *dec; int rate, channels; } Decoder;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void decoder_close(Service *s) {
    Decoder *d = s->state;
    if (d) { if (d->dec) opus_decoder_destroy(d->dec); free(d); s->state = NULL; }
}

// one packet -> interleaved s16; returns result code, fills consumed (bytes of the packet incl. header) and samples
static int decode(Decoder *d, IpcReq *req, int32_t *consumed, int32_t *samples) {
    IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
    *consumed = 0, *samples = 0;
    if (!d || !d->dec || !in || !out || in->size < 8) return 1;
    const uint8_t *pkt = (const uint8_t *)(uintptr_t)in->addr;
    uint32_t payload = be32(pkt);
    if (payload + 8 > in->size) return 1;
    int max_frame = (int)(out->size / (2u * (unsigned)d->channels));
    int n = opus_decode(d->dec, pkt + 8, (opus_int32)payload, (opus_int16 *)(uintptr_t)out->addr, max_frame, 0);
    if (n < 0) { LOG("[hwopus] opus_decode failed: %d\n", n); return 1; }
    *consumed = (int32_t)(payload + 8);
    *samples = n;
    return 0;
}

static void decoder(Service *s, IpcReq *req, IpcResp *resp) {
    Decoder *d = s->state;
    int32_t consumed, samples;
    switch (req->cmd) {
    case 0:  // DecodeInterleavedOld
        if (decode(d, req, &consumed, &samples)) { resp->result = MAKE_RESULT(111, 3); break; }
        OUT(int32_t, 0) = consumed, OUT(int32_t, 4) = samples;
        resp_size(resp, 8);
        break;
    case 4:  // DecodeInterleavedWithPerfOld
    case 6:  // DecodeInterleavedWithPerfAndResetOld(reset_context)
    case 8:  // DecodeInterleaved(reset_context) (what SDK 9.3.1's nn::codec uses)
        if (req->cmd != 4 && IN(uint8_t, 0) && d && d->dec) opus_decoder_ctl(d->dec, OPUS_RESET_STATE);
        if (decode(d, req, &consumed, &samples)) { resp->result = MAKE_RESULT(111, 3); break; }
        OUT(int32_t, 0) = consumed, OUT(int32_t, 4) = samples;
        OUT(uint64_t, 8) = 0;  // time taken (perf)
        resp_size(resp, 16);
        break;
    case 1: break;  // SetContext
    default: diag_unimplemented("ipc", "[hwopus] IHardwareOpusDecoder cmd %u", req->cmd);
    }
}

static uint32_t work_size(uint32_t channels) {
    int sz = opus_decoder_get_size((int)channels);
    return (uint32_t)((sz + 0x400 + 0xFFF) & ~0xFFF);
}

SERVICE("hwopus", hwopus) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // OpenHardwareOpusDecoder(params{rate, channels}, work_buffer_size) -> IHardwareOpusDecoder
        Decoder *d = calloc(1, sizeof *d);
        d->rate = (int)IN(uint32_t, 0), d->channels = (int)IN(uint32_t, 4);
        int err = 0;
        d->dec = opus_decoder_create(d->rate, d->channels, &err);
        if (!d->dec) LOG("[hwopus] opus_decoder_create(%d, %d) failed: %d\n", d->rate, d->channels, err);
        Service *dec = service_new("IHardwareOpusDecoder", decoder, d);
        dec->on_close = decoder_close;
        ipc_return_object(resp, dec);
        break;
    }
    case 1:  // GetWorkBufferSize(params) -> u32
        OUT(uint32_t, 0) = work_size(IN(uint32_t, 4));
        resp_size(resp, 4);
        break;
    case 3:  // GetWorkBufferSizeForMultiStream
        OUT(uint32_t, 0) = work_size(2);
        resp_size(resp, 4);
        break;
    default: diag_unimplemented("ipc", "[hwopus] cmd %u", req->cmd); resp_size(resp, 0x10);
    }
}
