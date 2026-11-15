// Native SDL window transitions. Called only by the main SDL event thread.
#pragma once
#include <SDL3/SDL.h>
#include <windows.h>
#include <stdio.h>
#include "../platform/window_launch_hint_windows.h"
typedef struct WindowsWindowState {
    SDL_Window *window;
    int width,height,x,y;
    bool have_windowed;
} WindowsWindowState;
static SDL_DisplayID windows_window_display(const char *device) {
    SDL_DisplayID selected=SDL_GetPrimaryDisplay();
    if(!device||!*device)return selected;
    wchar_t wanted[32];if(!window_launch_monitor_valid(device)||
        !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,device,-1,wanted,32))return selected;
    int count=0;SDL_DisplayID *displays=SDL_GetDisplays(&count);if(!displays)return selected;
    for(int i=0;i<count;i++){
        HMONITOR monitor=(HMONITOR)SDL_GetPointerProperty(SDL_GetDisplayProperties(displays[i]),SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER,NULL);
        MONITORINFOEXW info;memset(&info,0,sizeof info);info.cbSize=sizeof info;
        if(monitor&&GetMonitorInfoW(monitor,(MONITORINFO*)&info)&&!wcscmp(info.szDevice,wanted)){selected=displays[i];break;}
    }
    SDL_free(displays);return selected;
}
static void windows_window_fit(const SDL_Rect *usable,int top,int left,int bottom,int right,
    int wanted_width,int wanted_height,int *width,int *height,int *x,int *y) {
    // Leave a visible margin and all decorations, including when saved size equals a whole monitor.
    int margin=24,maxw=usable->w-left-right-2*margin,maxh=usable->h-top-bottom-2*margin;
    if(maxw<1)maxw=1;if(maxh<1)maxh=1;
    *width=wanted_width>0?wanted_width:1280;*height=wanted_height>0?wanted_height:720;
    if(*width>maxw)*width=maxw;if(*height>maxh)*height=maxh;
    *x=usable->x+(usable->w-*width-left-right)/2+left;
    *y=usable->y+(usable->h-*height-top-bottom)/2+top;
}
static bool windows_window_capture(WindowsWindowState *state) {
    if(SDL_GetWindowFlags(state->window)&(SDL_WINDOW_FULLSCREEN|SDL_WINDOW_MAXIMIZED|SDL_WINDOW_MINIMIZED))return false;
    if(!SDL_GetWindowPosition(state->window,&state->x,&state->y)||!SDL_GetWindowSize(state->window,&state->width,&state->height))return false;
    state->have_windowed=state->width>0&&state->height>0;return state->have_windowed;
}
static bool windows_window_restore(WindowsWindowState *state,SDL_DisplayID display,bool center) {
    SDL_Window *window=state->window;
    if(!SDL_RestoreWindow(window)||!SDL_SetWindowBordered(window,true)||!SDL_SetWindowResizable(window,true)||!SDL_SyncWindow(window))return false;
    SDL_Rect usable;
    if(!display)display=SDL_GetDisplayForWindow(window);
    if(!SDL_GetDisplayUsableBounds(display,&usable)&&!SDL_GetDisplayBounds(display,&usable))return false;
    int top=0,left=0,bottom=0,right=0;
    if(!SDL_GetWindowBordersSize(window,&top,&left,&bottom,&right))return false;
    int width,height,x,y;
    windows_window_fit(&usable,top,left,bottom,right,state->width,state->height,&width,&height,&x,&y);
    if(!center&&state->have_windowed&&state->x-left>=usable.x&&state->y-top>=usable.y&&
        (int64_t)state->x+width+right<=(int64_t)usable.x+usable.w&&
        (int64_t)state->y+height+bottom<=(int64_t)usable.y+usable.h){x=state->x;y=state->y;}
    if(!SDL_SetWindowSize(window,width,height)||!SDL_SetWindowPosition(window,x,y)||!SDL_SyncWindow(window))return false;
    SDL_WindowFlags flags=SDL_GetWindowFlags(window);
    HWND hwnd=(HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);
    LONG_PTR style=hwnd?GetWindowLongPtrW(hwnd,GWL_STYLE):0;
    if((flags&(SDL_WINDOW_FULLSCREEN|SDL_WINDOW_BORDERLESS|SDL_WINDOW_MAXIMIZED))||
        !hwnd||(style&WS_CAPTION)!=WS_CAPTION||!(style&WS_THICKFRAME)||IsZoomed(hwnd)){
        SDL_SetError("Windowed transition did not produce a decorated restored native window");return false;
    }
    return windows_window_capture(state);
}
static bool windows_window_fullscreen(WindowsWindowState *state,bool fullscreen) {
    if(!state||!state->window)return SDL_SetError("Missing native SDL window");
    SDL_Window *window=state->window;
    if(fullscreen){
        if(!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN))windows_window_capture(state);
        if(!SDL_SetWindowFullscreenMode(window,NULL)||!SDL_SetWindowFullscreen(window,true)||!SDL_SyncWindow(window))return false;
        if(!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN))return SDL_SetError("Native fullscreen request was not applied");
        return true;
    }
    SDL_DisplayID display=SDL_GetDisplayForWindow(window);
    if(!SDL_SetWindowFullscreen(window,false)||!SDL_SyncWindow(window))return false;
    return windows_window_restore(state,display,false);
}
static bool windows_window_initialize(WindowsWindowState *state,SDL_Window *window,int width,int height,const char *device,bool fullscreen) {
    memset(state,0,sizeof *state);state->window=window;state->width=width;state->height=height;
    SDL_DisplayID display=windows_window_display(device);
    // Settle per-monitor DPI before measuring decorations and fitting the client size.
    if(!SDL_SetWindowPosition(window,SDL_WINDOWPOS_CENTERED_DISPLAY(display),SDL_WINDOWPOS_CENTERED_DISPLAY(display))||!SDL_SyncWindow(window))return false;
    if(!windows_window_restore(state,display,true))return false;
    return !fullscreen||windows_window_fullscreen(state,true);
}
