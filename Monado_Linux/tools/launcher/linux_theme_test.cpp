// [XCDE] LINUX-015: test-only component renderer; actual launcher main/handlers/widgets.
// Extent/DPI/backend and event adapters do not modify production launcher.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "imgui.h"
#include "imgui_internal.h"
static int test_width, test_height, test_frame, test_fork_refusals, test_min_width, test_min_height;
static float test_scale;
static std::string test_mode;
static bool test_wrapped_value;
static int test_wrapped_changes,test_wrapped_marked;
static SDL_Window *test_window;
static bool test_set_window_size(SDL_Window *, int, int);
static float test_display_scale(SDL_Window *);
static SDL_Window *test_create_window(const char *, int, int, SDL_WindowFlags);
static bool test_poll_event(SDL_Event *);
static bool test_present(SDL_Renderer *);
static SDL_Surface *test_load_png(SDL_IOStream *, bool);
static pid_t test_forbid_fork() { ++test_fork_refusals; errno = EPERM; return -1; }
namespace ImGui {
static void TestNewFrame();
static bool TestButton(const char *, const ImVec2& = ImVec2(0,0));
static bool TestCheckbox(const char *, bool *);
static bool TestCombo(const char *, int *, const char *const[], int, int = -1);
static bool TestBeginTabItem(const char *, bool * = nullptr, ImGuiTabItemFlags = 0);
static bool TestItemAdd(const ImRect&, ImGuiID, const ImRect * = nullptr, ImGuiItemFlags = 0);
}
#define SDL_CreateWindow test_create_window
#define SDL_SetWindowSize test_set_window_size
#define SDL_GetWindowDisplayScale test_display_scale
#define SDL_PollEvent test_poll_event
#define SDL_RenderPresent test_present
#define SDL_LoadPNG_IO test_load_png
#define fork test_forbid_fork
#define NewFrame TestNewFrame
#define Button TestButton
#define Checkbox TestCheckbox
#define Combo TestCombo
#define BeginTabItem TestBeginTabItem
#define ItemAdd TestItemAdd
#define main launcher_main_under_test
#ifndef THEME_LAUNCHER_SOURCE
#define THEME_TEST_CURRENT_SOURCE 1
#define THEME_LAUNCHER_SOURCE "tools/launcher/launcher.cpp"
#endif
#include THEME_LAUNCHER_SOURCE
#undef main
#undef ItemAdd
#undef BeginTabItem
#undef Combo
#undef Checkbox
#undef Button
#undef NewFrame
#undef fork
#undef SDL_LoadPNG_IO
#undef SDL_RenderPresent
#undef SDL_PollEvent
#undef SDL_GetWindowDisplayScale
#undef SDL_SetWindowSize
#undef SDL_CreateWindow
struct Widget {
    std::string type,label,window; ImGuiID id; ImVec2 a,b; bool visible,disabled,changed;
    std::vector<std::string> options;
};
struct Window { std::string name; float y,max_y; ImVec2 a,b; float x,max_x; };
static std::vector<Widget> frame_widgets,last_widgets;
static std::vector<Window> last_windows;
static unsigned last_popup_count,last_nav_id;
static std::vector<unsigned> last_popup_ids;
static bool last_nav_visible;
static void record_widget(const char *type,const char *label,bool changed) {
    ImGuiContext &ctx=*ImGui::GetCurrentContext();
    frame_widgets.push_back({type,label,ImGui::GetCurrentWindow()->Name,
        ImGui::GetItemID(),ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),
        ImGui::IsItemVisible(),bool(ctx.LastItemData.ItemFlags&ImGuiItemFlags_Disabled),changed,{}});
}
namespace ImGui {
static void TestNewFrame() {
    NewFrame();
#if defined(MONADO_LAUNCHER_RESOURCES) && defined(THEME_TEST_CURRENT_SOURCE)
    if(test_mode.compare(0,7,"wrapped")==0) {
        SetNextWindowPos(ImVec2(60*test_scale,180*test_scale));
        SetNextWindowSize(ImVec2(280*test_scale,240*test_scale));
        if(test_frame==3)SetNextWindowFocus();
        Begin("wrapped-probe",nullptr,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoCollapse);
        if(test_mode=="wrapped-keyboard"&&test_frame==5)SetKeyboardFocusHere();
        bool changed=launcher_theme::checkbox("A wrapped checkbox keeps its complete final line clickable and supports keyboard activation.",&test_wrapped_value);
        record_widget("WrappedProbe","probe",changed);
        if(changed) {++test_wrapped_changes;if(GetCurrentContext()->LastItemData.StatusFlags&ImGuiItemStatusFlags_Edited)++test_wrapped_marked;}
        End();
    }
#endif
}
static bool TestButton(const char *label,const ImVec2& size) {
    bool result=Button(label,size); record_widget("Button",label,result); return result;
}
static bool TestCheckbox(const char *label,bool *value) {
    bool result=Checkbox(label,value); record_widget("Checkbox",label,result); return result;
}
static bool TestCombo(const char *label,int *value,const char *const items[],int count,int max_height) {
    bool result=Combo(label,value,items,count,max_height); record_widget("Combo",label,result);
    for (int i=0;i<count;++i) frame_widgets.back().options.emplace_back(items[i]);
    return result;
}
static bool TestBeginTabItem(const char *label,bool *open,ImGuiTabItemFlags flags) {
    bool result=BeginTabItem(label,open,flags); record_widget("Tab",label,false); return result;
}
static bool TestItemAdd(const ImRect &box,ImGuiID id,const ImRect *nav,ImGuiItemFlags flags) {
    bool result=ItemAdd(box,id,nav,flags);
    record_widget("CustomItem","",false); return result;
}
}
static bool test_set_window_size(SDL_Window *window,int,int) {
    return SDL_SetWindowSize(window,test_width,test_height);
}
static float test_display_scale(SDL_Window *) { return test_scale; }
static SDL_Window *test_create_window(const char *title,int width,int height,SDL_WindowFlags flags) {
    test_window=SDL_CreateWindow(title,width,height,flags); return test_window;
}
static SDL_Surface *test_load_png(SDL_IOStream *input,bool closeio) {
    if (test_mode=="no-art") { if(closeio)SDL_CloseIO(input);SDL_SetError("synthetic decorative artwork decode failure");return nullptr; }
    return SDL_LoadPNG_IO(input,closeio);
}
static bool test_poll_event(SDL_Event *event) {
    if(SDL_PollEvent(event))return true;
    static int focus_frame=-1,motion_frame=-1,wheel_frame=-1,key_frame=-1;
    if(test_frame==1&&focus_frame!=test_frame) {
        focus_frame=test_frame; *event={};event->type=SDL_EVENT_WINDOW_FOCUS_GAINED;
        event->window.windowID=SDL_GetWindowID(test_window);return true;
    }
    if(((test_mode=="bottom"&&test_frame==5)||(test_mode=="bindings-bottom"&&(test_frame==5||test_frame==14||test_frame==22)))&&motion_frame!=test_frame) {
        motion_frame=test_frame;*event={};event->type=SDL_EVENT_MOUSE_MOTION;
        event->motion.windowID=SDL_GetWindowID(test_window);
        event->motion.x=test_width*.65f+(test_frame==14?30:test_frame==22?60:0);event->motion.y=test_height*.5f;return true;
    }
    if(((test_mode=="bottom"&&test_frame==8)||(test_mode=="bindings-bottom"&&(test_frame==8||test_frame==16||test_frame==24)))&&wheel_frame!=test_frame) {
        wheel_frame=test_frame;*event={};event->type=SDL_EVENT_MOUSE_WHEEL;
        event->wheel.windowID=SDL_GetWindowID(test_window);
        event->wheel.x=test_frame==24?1000:0;event->wheel.y=test_frame==24?0:-1000;event->wheel.mouse_x=test_width*.65f;event->wheel.mouse_y=test_height*.5f;
        event->wheel.direction=SDL_MOUSEWHEEL_NORMAL;return true;
    }
    if(test_mode=="focus"&&(test_frame==8||test_frame==9)&&key_frame!=test_frame) {
        key_frame=test_frame;*event={};event->type=test_frame==8?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
        event->key.windowID=SDL_GetWindowID(test_window);event->key.scancode=SDL_SCANCODE_TAB;
        event->key.key=SDLK_TAB;event->key.down=test_frame==8;return true;
    }
    return false;
}
static bool test_present(SDL_Renderer *renderer) {
    SDL_GetWindowMinimumSize(test_window,&test_min_width,&test_min_height);
    last_widgets=frame_widgets;frame_widgets.clear();last_windows.clear();
    ImGuiContext &ctx=*ImGui::GetCurrentContext();
    last_popup_count=ctx.OpenPopupStack.Size;last_popup_ids.clear();for(const auto &p:ctx.OpenPopupStack)last_popup_ids.push_back(p.PopupId);last_nav_id=ctx.NavId;last_nav_visible=ctx.NavCursorVisible;
    for (ImGuiWindow *w:ctx.Windows)
        if(w->Active)last_windows.push_back({w->Name,w->Scroll.y,w->ScrollMax.y,w->Pos,ImVec2(w->Pos.x+w->Size.x,w->Pos.y+w->Size.y),w->Scroll.x,w->ScrollMax.x});
    if(test_mode=="long-status")g_status="Synthetic long status: settings remain available. This is a deliberately long launcher message for layout verification only; no game has been launched and every existing action must remain accessible at this window size.";
    ++test_frame;
    bool result=SDL_RenderPresent(renderer);
    SDL_Delay(test_mode=="tooltip"?12:2);return result;
}
static void quoted(const std::string &s) {
    putchar('"');for(unsigned char c:s) {if(c=='"'||c=='\\'){putchar('\\');putchar(c);}else if(c=='\n')fputs("\\n",stdout);else if(c=='\r')fputs("\\r",stdout);else if(c=='\t')fputs("\\t",stdout);else if(c<32)printf("\\u%04x",c);else putchar(c);}putchar('"');
}
int main(int argc,char **argv) {
    if(argc<6)return 2;
    test_width=atoi(argv[1]);test_height=atoi(argv[2]);test_scale=(float)atof(argv[3]);test_mode=argv[4];
    if(test_width<320||test_height<320||test_width>8192||test_height>8192||test_scale<.5f||test_scale>4)return 2;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"dummy");SDL_SetHint(SDL_HINT_RENDER_DRIVER,"software");
    int result=launcher_main_under_test(argc-4,argv+4);
    printf("THEME_TEST {\"width\":%d,\"height\":%d,\"scale\":%.3f,\"frames\":%d,\"fork_refusals\":%d,\"popup_count\":%u,\"nav_id\":%u,\"nav_visible\":%s,\"fullscreen\":%s,\"window_index\":%d,\"show_fps\":%s,\"ui_lang\":%d,\"menu_navigation_speed\":%d,\"widgets\":[",test_width,test_height,test_scale,test_frame,test_fork_refusals,last_popup_count,last_nav_id,last_nav_visible?"true":"false",g.fullscreen?"true":"false",g.win,g.show_fps?"true":"false",g.ui_lang,g.menu_navigation_speed);
    bool first=true;
    for(const Widget &w:last_widgets) {
        if(!first)putchar(',');first=false;fputs("{\"type\":",stdout);quoted(w.type);fputs(",\"label\":",stdout);quoted(w.label);fputs(",\"window\":",stdout);quoted(w.window);
        printf(",\"id\":%u,\"rect\":[%.3f,%.3f,%.3f,%.3f],\"visible\":%s,\"disabled\":%s,\"changed\":%s,\"options\":[",w.id,w.a.x,w.a.y,w.b.x,w.b.y,w.visible?"true":"false",w.disabled?"true":"false",w.changed?"true":"false");
        for(size_t i=0;i<w.options.size();++i){if(i)putchar(',');quoted(w.options[i]);}fputs("]",stdout);printf(",\"expected_popup_id\":%u}",w.type=="Combo"?ImHashStr("##ComboPopup",0,w.id):0);
    }
    fputs("],\"windows\":[",stdout);first=true;
    for(const Window &w:last_windows){if(!first)putchar(',');first=false;fputs("{\"name\":",stdout);quoted(w.name);printf(",\"scroll_y\":%.3f,\"scroll_max_y\":%.3f,\"scroll_x\":%.3f,\"scroll_max_x\":%.3f,\"rect\":[%.3f,%.3f,%.3f,%.3f]}",w.y,w.max_y,w.x,w.max_x,w.a.x,w.a.y,w.b.x,w.b.y);}
    printf("],\"window_minimum_width\":%d,\"window_minimum_height\":%d,\"wrapped_value\":%s,\"wrapped_changes\":%d,\"wrapped_marked\":%d,\"popup_ids\":[",test_min_width,test_min_height,test_wrapped_value?"true":"false",test_wrapped_changes,test_wrapped_marked);for(size_t i=0;i<last_popup_ids.size();++i){if(i)putchar(',');printf("%u",last_popup_ids[i]);}
    fputs("],\"hash_contracts\":[",stdout);first=true;
    const char *labels[]={"Window size","Internal resolution","Launcher language","Menu navigation speed"};
    for(unsigned seed: {0u,1u,0x12345678u,0xdeadbeefu})for(const char *label:labels){
        ImGuiID original=ImHashStr(label,0,seed),bad=ImHashStr((std::string("###")+label).c_str(),0,seed),empty=ImHashStr("",0,original);
        if(!first)putchar(',');first=false;printf("{\"seed\":%u,\"original\":%u,\"old_hash_negative_control\":%u,\"empty_seeded\":%u}",seed,original,bad,empty);
        if(bad==original||empty!=original)result=4;
    }
    puts("]}");
    return test_fork_refusals?3:result;
}
