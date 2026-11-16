// Authored portable setup view. Shares the actual settings launcher's artwork,
// fonts, layout and widgets; the existing checked backend owns every operation.
#pragma once
static void branded_path(const char *label,HWND field_control,int browse_id,std::vector<char>&buffer,float scale){
    ImGui::BeginDisabled(!monado_setup_policy::path_controls_enabled(busy.load()));
    auto current=utf8(text(field_control));
    require(current.size()<buffer.size(),"Selected path exceeds the setup field limit");
    if(strcmp(buffer.data(),current.c_str()))memcpy(buffer.data(),current.c_str(),current.size()+1);
    ImGui::TextUnformatted(label);ImGui::PushID(label);
    float width=ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(ImMax(80.f*scale,width-116.f*scale));
    if(ImGui::InputText("##path",buffer.data(),buffer.size()))SetWindowTextW(field_control,wide(buffer.data()).c_str());
    ImGui::SameLine();if(ImGui::Button("Browse...",ImVec2(106.f*scale,0)))SendMessageW(window,WM_COMMAND,browse_id,0);
    ImGui::PopID();ImGui::EndDisabled();
}
static void branded_action(const char *label,int command,bool enabled=true){
    ImGui::BeginDisabled(!enabled);if(ImGui::Button(label))SendMessageW(window,WM_COMMAND,command,0);ImGui::EndDisabled();
}
static int branded_main(HWND bridge,const std::wstring&shot){
    SDL_SetMainReady();branded_active=true;
    require(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD),"Cannot initialize the launcher display");
    SDL_Window *view=SDL_CreateWindow("Monado II",1360,780,SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY|SDL_WINDOW_HIDDEN);
    require(view!=nullptr,"Cannot create the Monado II launcher window");
    interaction_window=(HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(view),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr);
    SDL_Renderer *renderer=SDL_CreateRenderer(view,shot.empty()?nullptr:"software");
    if(!renderer)renderer=SDL_CreateRenderer(view,"software");
    require(renderer!=nullptr,"Cannot create the launcher renderer");SDL_SetRenderVSync(renderer,1);
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=nullptr;
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;
    float scale=shot.empty()?SDL_GetWindowDisplayScale(view):1.f;if(scale<=0)scale=1;
    SDL_SetWindowSize(view,(int)(1360*scale),(int)(780*scale));SDL_SetWindowMinimumSize(view,(int)(640*scale),(int)(480*scale));
    SDL_SetWindowPosition(view,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED);
    if(!launcher_theme::fonts(io,scale))io.Fonts->AddFontDefault();launcher_theme::style(scale);
    SDL_Texture *art=launcher_theme::artwork(renderer);require(art!=nullptr,"The reviewed launcher artwork is missing");
    ImGui_ImplSDL3_InitForSDLRenderer(view,renderer);ImGui_ImplSDLRenderer3_Init(renderer);
    std::vector<char> game_path(131073),install_path(131073);bool advanced=false,show_details=false,run=true;int frames=0,result=0;
    if(text(destination).empty()){
        auto self=executable();auto slash=self.find_last_of(L"\\/");
        require(slash!=std::wstring::npos,"Cannot choose a portable installation default");
        SetWindowTextW(destination,(self.substr(0,slash)+L"\\MonadoII").c_str());
    }
    SendMessageW(input,EM_SETLIMITTEXT,32767,0);SendMessageW(destination,EM_SETLIMITTEXT,32767,0);
    if(shot.empty())SDL_ShowWindow(view);
    if(shot.empty()||branded_bootstrap_fixture)launch_action(L"resume",true);
    else SetWindowTextW(phase,L"Choose your game files to get started");
    while(run){
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT){run=false;break;}TranslateMessage(&message);DispatchMessageW(&message);}
        if(!run||branded_handoff)break;
        SDL_Event event_value;while(SDL_PollEvent(&event_value)){
            ImGui_ImplSDL3_ProcessEvent(&event_value);
            if(event_value.type==SDL_EVENT_QUIT)SendMessageW(bridge,WM_CLOSE,0,0);
        }
        ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();
        auto vp=ImGui::GetMainViewport();ImGui::SetNextWindowPos(vp->WorkPos);ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("main",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground);
        bool installed=checked_installation,recover=monado_setup_policy::primary(setup_selection)==monado_setup_policy::Primary::locate;
        const char *actions[]={recover?"LOCATE GAME FILES":installed?"OPEN LAUNCHER":"BUILD MONADO II","Advanced","Details",busy?"Cancel":"Use existing install","Exit"};
        auto layout=launcher_theme::layout(vp->WorkPos,vp->WorkSize,scale,actions,busy.load());
        launcher_theme::backdrop(layout);launcher_theme::branding(layout,MONADO_WINDOWS_BASELINE_LABEL);
        ImGui::SetCursorScreenPos(ImVec2(vp->WorkPos.x+layout.panel_x+16*scale,vp->WorkPos.y+layout.header+12*scale));
        ImGui::BeginChild("setup-content",ImVec2(layout.panel_width-32*scale,ImMax(40.f*scale,layout.content_bottom-layout.header-12*scale)));
        ImGui::PushTextWrapPos(0);
        launcher_theme::section(installed?"Your installation":"Welcome to Monado II");
        ImGui::TextWrapped(installed?"Open your launcher to change settings and play. Your existing settings and saves stay with this installation.":"Choose your own extracted game folder. Monado II will build the game locally and keep those original files in place.");
        branded_path("Game files (folder containing exefs and romfs)",input,INPUT_BROWSE,game_path,scale);
        branded_path("Install location",destination,DEST_BROWSE,install_path,scale);
        ImGui::TextDisabled("Choose a short, empty folder for a new installation.");
        ImGui::BeginDisabled(busy.load());int jobs=(int)SendMessageW(workers,CB_GETCURSEL,0,0);const char *counts[]={"1","2","3","4","5","6"};
        ImGui::SetNextItemWidth(90*scale);if(launcher_theme::combo("Build threads",&jobs,counts,6))SendMessageW(workers,CB_SETCURSEL,jobs,0);
        bool shaders=SendMessageW(prepare,BM_GETCHECK,0,0)==BST_CHECKED;
        if(launcher_theme::checkbox("Prepare shaders after building (optional)",&shaders))SendMessageW(prepare,BM_SETCHECK,shaders?BST_CHECKED:BST_UNCHECKED,0);
        ImGui::EndDisabled();
        ImGui::TextWrapped("Windows Experimental: DLSS can show purple flashes and flickering. These issues remain unresolved.");
        if(advanced){
            launcher_theme::section("Graphics SDKs and updates");ImGui::BeginDisabled(busy.load());
            branded_action("Set up NVIDIA DLSS (experimental)",DLSS_SDK);ImGui::SameLine();branded_action("Use local AMD FSR3 SDK",FFX_SDK);
            branded_action("Check for updates",UPDATES,installed);ImGui::SameLine();branded_action("Apply update",APPLY_UPDATE,installed&&update_available);
            branded_action("Locate moved game files",LOCATE,installed);ImGui::SameLine();branded_action("Use an existing installation",ADOPT);
            branded_action("Build FSR4 preparation tools",FSR4_TOOLS,installed);branded_action("Prepare FSR4 Lite",FSR4_PREPARE,installed);
            std::wstring notice_cache;{std::lock_guard<std::mutex> lock(state_mutex);notice_cache=cache;}
            auto license_path=notice_cache+L"\\licenses\\README.txt";
            if(!notice_cache.empty()&&ordinary(license_path)&&ImGui::Button("Open-source license notices")){
                auto opened=ShellExecuteW(interaction_window,L"open",license_path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
                require((INT_PTR)opened>32,"Cannot open the local license notice index");
            }
            ImGui::EndDisabled();
        }
        if(show_details){launcher_theme::section("Build details");auto log=utf8(text(details));ImGui::TextUnformatted(log.c_str());}
        ImGui::PopTextWrapPos();ImGui::EndChild();
        ImGui::SetCursorScreenPos(ImVec2(vp->WorkPos.x+layout.panel_x+16*scale,vp->WorkPos.y+layout.footer_top+12*scale));
        ImGui::PushTextWrapPos(vp->WorkPos.x+layout.panel_x+layout.panel_width-32*scale);auto status=utf8(text(phase));ImGui::TextUnformatted(status.c_str());ImGui::PopTextWrapPos();
        if(busy.load()){
            float fraction=displayed_total?(float)((double)displayed_done/displayed_total):0.f;
            std::string count=displayed_total?std::to_string(displayed_done)+" / "+std::to_string(displayed_total):"Preparing...";
            ImGui::ProgressBar(std::clamp(fraction,0.f,1.f),ImVec2(layout.panel_width-32*scale,0),count.c_str());
        }
        launcher_theme::footer_button(layout,1);if(ImGui::Button(actions[1],layout.button_sizes[1]))advanced=!advanced;
        launcher_theme::footer_button(layout,2);if(ImGui::Button(actions[2],layout.button_sizes[2]))show_details=!show_details;
        launcher_theme::footer_button(layout,3);if(ImGui::Button(actions[3],layout.button_sizes[3]))SendMessageW(bridge,WM_COMMAND,busy.load()?CANCEL:ADOPT,0);
        launcher_theme::footer_button(layout,4);if(ImGui::Button(actions[4],layout.button_sizes[4]))SendMessageW(bridge,WM_CLOSE,0,0);
        launcher_theme::footer_button(layout,0);ImGui::PushStyleColor(ImGuiCol_Button,launcher_theme::color(0xC91542));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,launcher_theme::color(0xE32954));ImGui::PushStyleColor(ImGuiCol_ButtonActive,launcher_theme::color(0x9D1034));
        ImGui::BeginDisabled(busy.load()||(!installed&&!recover&&(text(input).empty()||text(destination).empty())));
        if(ImGui::Button(actions[0],layout.button_sizes[0]))SendMessageW(bridge,WM_COMMAND,recover?LOCATE:installed?OPEN:BUILD,0);
        ImGui::EndDisabled();ImGui::PopStyleColor(3);ImGui::End();ImGui::Render();
        SDL_SetRenderDrawColor(renderer,10,20,32,255);SDL_RenderClear(renderer);launcher_theme::render_art(renderer,art);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
        if(!shot.empty()&&(!branded_bootstrap_fixture||branded_bootstrap_complete)&&++frames==5){
            auto surface=SDL_RenderReadPixels(renderer,nullptr);require(surface!=nullptr,"Cannot capture authored setup fixture");
            bool saved=SDL_SaveBMP(surface,utf8(shot).c_str());SDL_DestroySurface(surface);require(saved,"Cannot save authored setup fixture");result=branded_bootstrap_fixture?fixture_exit:0;run=false;
        }
        SDL_RenderPresent(renderer);
    }
    if(IsWindow(bridge))SendMessageW(bridge,WM_CLOSE,0,0);
    if(worker.joinable())worker.join();
    ImGui_ImplSDLRenderer3_Shutdown();ImGui_ImplSDL3_Shutdown();ImGui::DestroyContext();
    SDL_DestroyTexture(art);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(view);SDL_Quit();interaction_window=nullptr;
    return result;
}
