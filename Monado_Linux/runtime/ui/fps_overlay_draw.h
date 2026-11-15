// [XCDE] LINUX-094: authored output-space FPS display, independent of game HUD.
#pragma once
#include "imgui.h"
#include <float.h>
#include <math.h>
#include <stdio.h>

namespace fps_overlay_detail {
struct Layout {
    bool visible = false;
    float font_size = 0;
    ImVec2 minimum{}, maximum{}, text_position{};
    char text[128]{};
};

// [XCDE] LINUX-097: rendered game FPS is independent from accepted output
// submissions. External driver FG cannot be measured from the game counter.
struct Rates {
    bool game_ready = false;
    double game = 0;
    bool generation = false, external = false, output_ready = false;
    bool external_measured = false;
    double output = 0;
    bool generated_ready = false;
    double generated = 0;
};
inline bool valid_rate(bool ready, double value) {
    return ready && isfinite(value) && value >= 0 && value <= 1000000000.;
}

// Foreground primitives create no interactive window and consume no input.
// A fixed high-resolution font atlas supports readable physical-size text at
// normal window sizes and 4K, without rebuilding GPU resources during resizing.
inline Layout draw(bool enabled, const Rates &rates, float width, float height) {
    Layout result;
    if (!enabled || !isfinite(width) || !isfinite(height) || width < 4 || height < 4) return result;
    char game[24], output[24], generated[24];
    if (valid_rate(rates.game_ready, rates.game)) snprintf(game, sizeof game, "%.0f", rates.game);
    else snprintf(game, sizeof game, "--");
    if (!rates.generation) snprintf(result.text, sizeof result.text, "%s FPS", game);
    else if (rates.external && rates.external_measured) {
        if (valid_rate(rates.output_ready, rates.output)) snprintf(output, sizeof output, "%.0f", rates.output);
        else snprintf(output, sizeof output, "--");
        snprintf(result.text,sizeof result.text,"Game %s FPS\nOutput %s FPS (driver submitted)\nGenerated unavailable",game,output);
    }
    else if (rates.external) snprintf(result.text, sizeof result.text, "Game %s FPS\nOutput external (unavailable)", game);
    else {
        if (valid_rate(rates.output_ready, rates.output)) snprintf(output, sizeof output, "%.0f", rates.output);
        else snprintf(output, sizeof output, "--");
        // Count classified successful midpoint submissions. Repeated real
        // frames and native frames are never inferred to be generated output.
        if (valid_rate(rates.generated_ready, rates.generated) &&
                valid_rate(rates.output_ready, rates.output) && rates.generated <= rates.output)
            snprintf(generated, sizeof generated, "%.0f", rates.generated);
        else snprintf(generated, sizeof generated, "--");
        snprintf(result.text, sizeof result.text, "Game %s FPS\nOutput %s FPS (submitted)\nGenerated %s FPS", game, output, generated);
    }
    ImFont *font = ImGui::GetFont();
    float font_size = fmaxf(16.0f, fminf(48.0f, height * (20.0f / 1080.0f)));
    float margin = fminf(font_size * 0.6f, fminf(width, height) * 0.1f);
    ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0, result.text);
    float padding_x = font_size * 0.45f, padding_y = font_size * 0.25f;
    float scale = fminf(1.0f, fminf((width - 2 * margin) / (text_size.x + 2 * padding_x),
                                   (height - 2 * margin) / (text_size.y + 2 * padding_y)));
    if (!isfinite(scale) || scale <= 0) return result;
    font_size *= scale; padding_x *= scale; padding_y *= scale;
    text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0, result.text);
    result.visible = true; result.font_size = font_size;
    result.minimum = ImVec2(margin, margin);
    result.maximum = ImVec2(margin + text_size.x + 2 * padding_x, margin + text_size.y + 2 * padding_y);
    result.text_position = ImVec2(margin + padding_x, margin + padding_y);
    ImDrawList *list = ImGui::GetForegroundDrawList();
    list->AddRectFilled(result.minimum, result.maximum, IM_COL32(0, 0, 0, 185), font_size * 0.25f);
    list->AddText(font, font_size, result.text_position, IM_COL32(255, 255, 255, 255), result.text);
    return result;
}
// Retain the ordinary single-counter call used by authored layout fixtures.
inline Layout draw(bool enabled, bool ready, double fps, float width, float height) {
    Rates rates; rates.game_ready = ready; rates.game = fps;
    return draw(enabled, rates, width, height);
}
}
