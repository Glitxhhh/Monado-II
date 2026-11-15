// SPDX-License-Identifier: GPL-2.0-or-later
// Expand the Lite prepass's half-input/float-accumulator dot on Windows.
// Keep the captured assets intact; only the in-memory shader module changes.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace Fsr411 {
inline bool PortableDot(std::vector<uint32_t>& words) {
    if (words.size() < 5 || words[0] != 0x07230203u) return false;
    uint32_t fp32 = 0, vec2 = 0;
    bool needed = false;
    for (size_t i = 5; i < words.size();) {
        uint32_t n = words[i] >> 16, op = words[i] & 65535;
        if (!n || i + n > words.size()) return false;
        if (op == 22 && n == 3 && words[i + 2] == 32) fp32 = words[i + 1];
        if (op == 6916) { if (n != 6) return false; needed = true; }
        i += n;
    }
    if (!needed) return true;
    if (!fp32) return false;
    for (size_t i = 5; i < words.size(); i += words[i] >> 16)
        if ((words[i] & 65535) == 23 && (words[i] >> 16) == 4 &&
            words[i + 2] == fp32 && words[i + 3] == 2) vec2 = words[i + 1];
    // The pinned Lite shaders already declare float2; refuse unknown layouts.
    if (!vec2) return false;
    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    uint32_t next = words[3];
    for (size_t i = 5; i < words.size();) {
        uint32_t n = words[i] >> 16, op = words[i] & 65535;
        const uint32_t* w = words.data() + i;
        bool extension = op == 10 && n > 1 &&
            (n - 1) * 4 == 36 &&
            std::memcmp(w + 1, "SPV_VALVE_mixed_float_dot_product", 34) == 0;
        if ((op == 17 && n == 2 && w[1] == 6912) || extension) {
            // CapabilityDotProductFloat16AccFloat32VALVE and its extension.
        } else if (op == 6916) {
            if (w[1] != fp32) return false;
            uint32_t a = next++, b = next++, dot = next++;
            out.insert(out.end(), {(4u << 16) | 115u, vec2, a, w[3],
                                   (4u << 16) | 115u, vec2, b, w[4],
                                   (5u << 16) | 148u, fp32, dot, a, b,
                                   (5u << 16) | 129u, fp32, w[2], dot, w[5]});
        } else out.insert(out.end(), w, w + n);
        i += n;
    }
    out[3] = next;
    words.swap(out);
    return true;
}
}
