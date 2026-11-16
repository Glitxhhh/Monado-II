// Authored hidden fixture: exact launcher art/fonts + ImGui SDL renderer.
// No window, game, cache extraction, input selection or profile is opened.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "imgui.h"
#include "imgui_impl_sdlrenderer3.h"
#include "../../tools/launcher/theme.h"
#include <cassert>
#include <cstdio>

int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    SDL_SetMainReady();
    assert(SDL_Init(0));
    SDL_Surface *surface=SDL_CreateSurface(960,640,SDL_PIXELFORMAT_RGBA32);
    assert(surface);
    SDL_Renderer *renderer=SDL_CreateSoftwareRenderer(surface);
    assert(renderer);
    ImGui::CreateContext();
    ImGuiIO &io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    io.DisplaySize=ImVec2(960,640);io.DeltaTime=1.f/60.f;
    assert(launcher_theme::fonts(io,1));
    launcher_theme::style(1);
    SDL_Texture *art=launcher_theme::artwork(renderer);
    assert(art&&ImGui_ImplSDLRenderer3_Init(renderer));
    ImGui_ImplSDLRenderer3_NewFrame();ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Authored hidden branded surface",nullptr,ImGuiWindowFlags_NoDecoration);
    const char *labels[5]={"Build","Settings","Providers","Cancel","Exit"};
    auto layout=launcher_theme::layout(ImVec2(0,0),io.DisplaySize,1,labels,false);
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)art,ImVec2(0,0),io.DisplaySize);
    launcher_theme::backdrop(layout);launcher_theme::branding(layout,"Windows Experimental — authored hidden fixture");
    ImGui::SetCursorPos(ImVec2(24,180));ImGui::TextUnformatted(MONADO_WINDOWS_BASELINE_LABEL);
    ImGui::End();ImGui::Render();
    assert(ImGui::GetDrawData()->TotalVtxCount>0);
    assert(SDL_SetRenderDrawColor(renderer,0,0,0,255)&&SDL_RenderClear(renderer));
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
    assert(SDL_RenderPresent(renderer));
    bool colored=false;auto *pixels=(unsigned char*)surface->pixels;
    for(int row=0;row<surface->h&&!colored;row++)for(int col=0;col<surface->w;col++) {
        auto *pixel=pixels+row*surface->pitch+col*4;
        if(pixel[0]||pixel[1]||pixel[2]){colored=true;break;}
    }
    assert(colored);
    SDL_DestroyTexture(art);ImGui_ImplSDLRenderer3_Shutdown();ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);SDL_DestroySurface(surface);SDL_Quit();
    puts("PASS static SDL3/iconv, actual embedded theme fonts/art, ImGui software surface; no window");
    return 0;
}
