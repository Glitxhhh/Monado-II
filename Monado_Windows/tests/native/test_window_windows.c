// Authored hidden SDL/Win32 windows only. No renderer, game, profile, input, or human window control.
#include <assert.h>
#include <SDL3/SDL.h>
#include <windows.h>
static bool fail_fullscreen;
static bool SDLCALL checked_fullscreen(SDL_Window *window,bool fullscreen){
    if(fail_fullscreen)return SDL_SetError("authored denied fullscreen transition");
    return SDL_SetWindowFullscreen(window,fullscreen);
}
#define SDL_SetWindowFullscreen checked_fullscreen
#include "../../runtime/ui/window_windows.h"
#undef SDL_SetWindowFullscreen
char g_window_launch_monitor[32];
static void fit_tests(void){
    SDL_Rect bounds={-2560,40,2560,1400};int w,h,x,y;
    windows_window_fit(&bounds,30,8,8,8,99999,99999,&w,&h,&x,&y);
    assert(w>0&&h>0&&w<bounds.w&&h<bounds.h);
    assert(x-8>=bounds.x&&y-30>=bounds.y&&x+w+8<=bounds.x+bounds.w&&y+h+8<=bounds.y+bounds.h);
    windows_window_fit(&bounds,30,8,8,8,1280,720,&w,&h,&x,&y);assert(w==1280&&h==720);
    assert(window_launch_monitor_valid("\\\\.\\DISPLAY1")&&window_launch_monitor_valid("\\\\.\\DISPLAY999"));
    assert(!window_launch_monitor_valid("DISPLAY1")&&!window_launch_monitor_valid("\\\\.\\DISPLAY1;anything"));
}
int main(void){
    fit_tests();HWND foreground=GetForegroundWindow();
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN,"0");assert(SDL_Init(SDL_INIT_VIDEO));
    int count=0;SDL_DisplayID *displays=SDL_GetDisplays(&count);assert(displays&&count>0);
    unsigned cases=0;
    for(int i=0;i<count;i++){
        HMONITOR monitor=(HMONITOR)SDL_GetPointerProperty(SDL_GetDisplayProperties(displays[i]),SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER,NULL);
        MONITORINFOEXW info;memset(&info,0,sizeof info);info.cbSize=sizeof info;assert(monitor&&GetMonitorInfoW(monitor,(MONITORINFO*)&info));
        char device[32];assert(WideCharToMultiByte(CP_UTF8,0,info.szDevice,-1,device,sizeof device,NULL,NULL));
        assert(windows_window_display(device)==displays[i]);
        SDL_Window *window=SDL_CreateWindow("Monado authored hidden window fixture",640,360,SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY);assert(window);
        WindowsWindowState state;
        assert(windows_window_initialize(&state,window,99999,99999,device,false));
        assert(SDL_GetDisplayForWindow(window)==displays[i]);
        SDL_Rect usable;assert(SDL_GetDisplayUsableBounds(displays[i],&usable));
        int width,height;assert(SDL_GetWindowSize(window,&width,&height)&&width<usable.w&&height<usable.h);
        HWND hwnd=(HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);assert(hwnd&&!IsWindowVisible(hwnd));
        RECT rect;assert(GetWindowRect(hwnd,&rect));
        assert(rect.left>=info.rcWork.left&&rect.top>=info.rcWork.top&&rect.right<=info.rcWork.right&&rect.bottom<=info.rcWork.bottom);
        int oldw=state.width,oldh=state.height;
        assert(windows_window_fullscreen(&state,true));assert(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN);
        fail_fullscreen=true;assert(!windows_window_fullscreen(&state,false));assert(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN);fail_fullscreen=false;
        assert(windows_window_fullscreen(&state,false));assert(!(SDL_GetWindowFlags(window)&(SDL_WINDOW_FULLSCREEN|SDL_WINDOW_BORDERLESS|SDL_WINDOW_MAXIMIZED)));
        assert(SDL_GetWindowSize(window,&width,&height));
        if(width!=oldw||height!=oldh)fprintf(stderr,"restore size expected %dx%d actual %dx%d target=%u current=%u flags=%llx\n",oldw,oldh,width,height,(unsigned)displays[i],(unsigned)SDL_GetDisplayForWindow(window),(unsigned long long)SDL_GetWindowFlags(window));
        assert(width==oldw&&height==oldh);
        assert(!IsWindowVisible(hwnd)&&GetForegroundWindow()==foreground);
        SDL_DestroyWindow(window);++cases;
    }
    assert(windows_window_display("\\\\.\\DISPLAY999999")==SDL_GetPrimaryDisplay());
    SDL_free(displays);SDL_Quit();printf("native hidden window transitions PASS displays=%u\n",cases);return 0;
}
