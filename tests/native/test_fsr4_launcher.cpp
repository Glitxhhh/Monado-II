// Actual launcher model-preparation IPC and availability; no SDL initialization.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#define main launcher_main_under_test
#include "../../Monado_Linux/tools/launcher/launcher.cpp"
#undef main
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FSR4 launcher FAIL line %d: %s\n",__LINE__,#x);abort();}}while(0)
static std::string packet(int fd){char buf[2049];ssize_t n=recv(fd,buf,sizeof(buf),0);CHECK(n>0&&n<2049);return std::string(buf,n);}
int main(int argc,char**argv){
    CHECK(argc==2&&!mkdir(argv[1],0700));g_root=argv[1];load();int original=g.upscaler;
    CHECK(!g_fsr3_available&&!linux_fsr4_available());CHECK(linux_component_hint("1",&g_fsr3_available));
    CHECK(!linux_component_hint("yes",&g_fsr3_available)&&g_fsr3_available);CHECK(!linux_component_hint(nullptr,&g_fsr3_available));
    int sockets[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets));
    {int type=0;socklen_t length=sizeof(type);if(getsockopt(sockets[0],SOL_SOCKET,SO_TYPE,&type,&length)&&errno==EPERM){puts("SKIP tool sandbox denies local socket descriptor inspection");return 77;}}
    if(!launcher_linux::game_adopt_fd(sockets[0])){int type=0;socklen_t len=sizeof(type);sockaddr_storage addr{};socklen_t alen=sizeof(addr);getsockopt(sockets[0],SOL_SOCKET,SO_TYPE,&type,&len);getsockname(sockets[0],(sockaddr*)&addr,&alen);fprintf(stderr,"adopt fd=%d flags=%d status=%d type=%d family=%d configured=%d error=%s errno=%d\n",sockets[0],fcntl(sockets[0],F_GETFD),fcntl(sockets[0],F_GETFL),type,addr.ss_family,launcher_linux::game_broker_configured,launcher_linux::game_message.c_str(),errno);abort();}
    CHECK(packet(sockets[1]).rfind("1\thello\t",0)==0);
    CHECK(launcher_linux::game_receive("1\tready\thost"));CHECK(launcher_linux::request_fsr4_prepare());CHECK(packet(sockets[1])=="1\tprepare-fsr4\t1");
    CHECK(launcher_linux::fsr4_preparing()&&launcher_linux::activity_busy());CHECK(!launcher_linux::request_fsr4_prepare());CHECK(!launcher_linux::start_game(g_root));
    CHECK(!launcher_linux::game_receive("1\tfsr4\t2\tready\twrong request"));CHECK(!linux_fsr4_available());
    CHECK(!launcher_linux::game_receive("1\tfsr4\t1\tunknown\tbad phase"));
    CHECK(launcher_linux::game_receive("1\tfsr4\t1\trunning\tCapturing model set 2/4"));CHECK(strstr(launcher_linux::fsr4_status(),"2/4"));
    CHECK(launcher_linux::cancel_fsr4_prepare());CHECK(packet(sockets[1])=="1\tcancel-fsr4\t1");CHECK(launcher_linux::fsr4_preparing());
    CHECK(launcher_linux::game_receive("1\tfsr4\t1\tcancelled\tCancelled safely"));CHECK(!launcher_linux::activity_busy()&&!linux_fsr4_available());
    CHECK(launcher_linux::request_fsr4_prepare());CHECK(packet(sockets[1])=="1\tprepare-fsr4\t2");
    CHECK(launcher_linux::game_receive("1\tfsr4\t2\tready\tModels ready"));CHECK(!launcher_linux::activity_busy()&&linux_fsr4_available());CHECK(g.upscaler==original);
    launcher_linux::game=std::string(argv[1])+"/xcde-linux";
    for(const char*n:{"libmonado-fidelityfx.so","FidelityFX-MIT.txt"}){FILE*f=fopen((std::string(argv[1])+"/"+n).c_str(),"w");CHECK(f&&!fclose(f));}
    g.upscaler=2;g.frame_gen=true;linux_graphics_normalize();CHECK(g.upscaler==2&&g.frame_gen);
    g_fsr3_available=false;linux_graphics_normalize();CHECK(g.upscaler==1&&!g.frame_gen);
    g.upscaler=4;g.frame_gen=true;linux_graphics_normalize();CHECK(g.upscaler==4&&g.frame_gen);CHECK(save());load();CHECK(g.upscaler==4&&g.frame_gen);
    launcher_linux::fsr4_models_ready=false;linux_graphics_normalize();CHECK(g.upscaler==1&&!g.frame_gen);
    launcher_linux::game_disconnect("fixture complete");close(sockets[1]);CHECK(!SDL_WasInit(0));puts("FSR4 launcher capability/IPC/progress/cancel/retry/selection/roundtrip PASS");
}
