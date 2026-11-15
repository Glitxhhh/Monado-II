// Bounded render-owner evidence plus a thread-safe immutable public status.
#pragma once
#include <stdatomic.h>
#include <stdint.h>
typedef enum TmpProviderStatus {
    TMP_STATUS_UNSEEN,TMP_STATUS_SPATIAL,TMP_STATUS_COMPONENT,TMP_STATUS_INPUT,
    TMP_STATUS_CONTEXT,TMP_STATUS_RECORDING,TMP_STATUS_DISPATCH,
    TMP_STATUS_FSR3,TMP_STATUS_DLSS,TMP_STATUS_FSR4
} TmpProviderStatus;
static atomic_int tmp_public_status=TMP_STATUS_UNSEEN;
static struct {
    const char *history,*result;
    int provider;
    uint32_t depth_w,depth_h;
    uint32_t previous_depth_w,previous_depth_h;
    unsigned sdk_result;
} tmp_evidence={.history="unseen",.result="not-evaluated"};
