// [XCDE] LINUX-103: one settings contract for launcher and in-game quality UI.
#pragma once
#include "imgui.h"
#include "../quality_policy.h"
static inline bool monado_quality_controls_linux(Settings *s,bool english) {
    Settings before=*s;
    int preset=s->quality_preset;
    const char *presets[]={english?"Balanced":"Equilibrado",english?"Performance":"Rendimiento",english?"Low hardware":"Equipo modesto",english?"Custom":"Personalizado"};
    if(ImGui::Combo(english?"Quality preset":"Perfil de calidad",&preset,presets,QUALITY_COUNT)) {
        if(preset==QUALITY_CUSTOM)s->quality_preset=preset;else xcde_quality_apply(s,preset);
    }
    ImGui::TextWrapped("%s",english?"Presets change resolution limits and depth of field only. Your upscaler, frame generation, FPS and gameplay options are preserved.":"Los perfiles cambian límites de resolución y profundidad de campo. Conservan escalado, generación, FPS y opciones del juego.");
    if(s->res_mode==RES_DYNAMIC) {
        ImGui::InputInt(english?"Dynamic minimum height (0: automatic)":"Altura mínima dinámica (0: automática)",&s->dynamic_min_height,90);
        ImGui::InputInt(english?"Dynamic maximum height (0: output)":"Altura máxima dinámica (0: salida)",&s->dynamic_max_height,90);
        xcde_quality_bounds(s);
    }
    if(s->upscaler==UPSCALE_FSR1 || s->upscaler==UPSCALE_FSR3 || s->upscaler>UPSCALE_DLSS) {
        bool defaults=s->upscale_sharpness<0.f;
        if(ImGui::Checkbox(english?"Default upscaler sharpening":"Nitidez predeterminada",&defaults))s->upscale_sharpness=defaults?-1.f:xcde_sharpness(s);
        ImGui::BeginDisabled(defaults);
        ImGui::SliderFloat(english?"Sharpening":"Nitidez",&s->upscale_sharpness,0.f,1.f,"%.2f");
        ImGui::EndDisabled();
    }
    if(ImGui::CollapsingHeader(english?"Additional quality options":"Opciones de calidad adicionales")) {
        bool taa=!s->taa_off;
        if(ImGui::Checkbox(english?"Game temporal AA (restart)":"AA temporal del juego (reiniciar)",&taa))s->taa_off=!taa;
        ImGui::TextWrapped("%s",english?"Temporal reconstruction automatically disables the game's AA on its jittered input. Original preserves authored or modded values.":"La reconstrucción temporal desactiva el AA del juego en su entrada. Original conserva los valores del juego y mods.");
        for(int i=0;i<GFX_COUNT;i++) {
            if(i==GFX_EFFECTS_RES)continue; // retired: full effects resolution breaks native occlusion
            const char *values[5];int count=gfx_count(i);
            for(int j=0;j<count;j++)values[j]=gfx_display_lang(i,j,english);
            int choice=gfx_normalize(i,s->graphics.value[i]);
            if(ImGui::Combo(gfx_label(i,english),&choice,values,count))s->graphics.value[i]=choice;
        }
        int grass=grass_lod_valid(s->grass_lod);
        if(ImGui::Combo(english?"Grass distance (restart)":"Distancia de hierba (reiniciar)",&grass,english?grass_lod_labels:grass_lod_labels_es,GRASS_PRESET_COUNT))s->grass_lod=grass;
        ImGui::TextWrapped("%s",english?"Higher grass and detail ranges cost GPU time and memory. Shadow sizes, masks, grass and prop LOD require a game restart.":"Más hierba y detalle cuestan GPU y memoria. Sombras, máscaras, hierba y LOD requieren reiniciar el juego.");
        if(ImGui::Button(english?"Restore original extra quality options":"Restaurar opciones adicionales originales")) {
            memset(&s->graphics,0,sizeof s->graphics);s->taa_off=false;s->grass_lod=GRASS_ORIGINAL;s->upscale_sharpness=-1.f;
        }
    }
    // Compare the explicit controls rather than padding or unrelated settings.
    bool custom=before.dynamic_min_height!=s->dynamic_min_height || before.dynamic_max_height!=s->dynamic_max_height ||
        before.upscale_sharpness!=s->upscale_sharpness || before.taa_off!=s->taa_off || before.grass_lod!=s->grass_lod ||
        gfx_pack(&before.graphics)!=gfx_pack(&s->graphics);
    if(custom && before.quality_preset==s->quality_preset)s->quality_preset=QUALITY_CUSTOM;
    return custom || before.quality_preset!=s->quality_preset || before.res_mode!=s->res_mode || before.dof_off!=s->dof_off;
}
