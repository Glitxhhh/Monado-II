// [XCDE] LINUX-006: only expose modes with an implemented platform provider.
#pragma once
#include "../settings.h"
static const char *const menu_upscale_names[] = {
    "Bilineal (suave)", "AMD FSR 1.0 (nítido)",
#if !defined(__linux__) || defined(XCDE_LINUX_FSR3)
    "AMD FSR 3.1 (temporal)",
#endif
#if !defined(__linux__) || defined(XCDE_LINUX_DLSS)
    "NVIDIA DLSS (temporal, RTX)",
#endif
#if defined(_WIN32) || (defined(__linux__) && defined(XCDE_LINUX_FSR4))
    "AMD FSR4 Lite (experimental)",
#endif
};
static const int menu_upscale_values[] = {
    UPSCALE_BILINEAR, UPSCALE_FSR1,
#if !defined(__linux__) || defined(XCDE_LINUX_FSR3)
    UPSCALE_FSR3,
#endif
#if !defined(__linux__) || defined(XCDE_LINUX_DLSS)
    UPSCALE_DLSS,
#endif
#if defined(_WIN32) || (defined(__linux__) && defined(XCDE_LINUX_FSR4))
    UPSCALE_FSR4_LITE,
#endif
};
enum { MENU_UPSCALER_COUNT = sizeof menu_upscale_values / sizeof menu_upscale_values[0] };
