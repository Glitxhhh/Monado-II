// [XCDE] LINUX-099: actual optional client against an authored ABI fixture.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static unsigned resources, logs;
static const char *module;
static bool resource_exists=true;
static bool rt_resource(const char *name,const char *dev,char *out,size_t bytes){assert(!strcmp(name,"libmonado-present-observer.so")&&!dev);resources++;if(!resource_exists)return false;assert(strlen(module)<bytes);strcpy(out,module);return true;}
#define LOG(...) (++logs)
#include "../../Monado_Linux/runtime/nvn/vulkan/present_observer_client_linux.h"
int main(int argc,char **argv){
 assert(argc==3);module=argv[2];double rate=999;bool ready=true;
 if(!strcmp(argv[1],"off")){assert(!observer_client_begin(false)&&!resources&&!observer_client_module);assert(!observer_client_rates(1,&rate,&ready)&&rate==0&&!ready);return 0;}
 if(!strcmp(argv[1],"uncontrolled")){unsetenv("MONADO_PRESENT_OBSERVER");assert(!observer_client_begin(true));setenv("MONADO_PRESENT_OBSERVER","true",1);assert(!observer_client_begin(true));setenv("MONADO_PRESENT_OBSERVER","0",1);assert(!observer_client_begin(true));assert(!resources&&!observer_client_module);return 0;}
 setenv("MONADO_PRESENT_OBSERVER","1",1);
 if(!strcmp(argv[1],"missing-resource")){resource_exists=false;assert(!observer_client_begin(true)&&resources==1&&logs==1);assert(!observer_client_begin(true)&&resources==1);return 0;}
 if(!strcmp(argv[1],"invalid-module")||!strcmp(argv[1],"missing-symbol")){assert(!observer_client_begin(true)&&resources==1&&logs==1);assert(!observer_client_rates(1,&rate,&ready)&&rate==0&&!ready);return 0;}
 assert(observer_client_begin(true)&&observer_client_module&&resources==1);assert(observer_client_begin(true)&&resources==1);
 typedef void(*Configure)(unsigned);void *symbol=dlsym(observer_client_module,"authored_configure");Configure configure;assert(symbol&&sizeof configure==sizeof symbol);memcpy(&configure,&symbol,sizeof configure);
 configure(0);assert(observer_client_rates(123,&rate,&ready)&&ready&&fabs(rate-227.4)<1e-9); // independent measurement, no gameFPS input
 MonadoPresentObserverSnapshot snapshot;
 assert(observer_client_read(123,&snapshot)&&snapshot.accepted==701&&snapshot.failed==4&&snapshot.window_ns==500000000&&snapshot.last_accepted_ns==122&&fabs(snapshot.output_fps-227.4)<1e-9);
 assert(!observer_client_read(123,NULL));
 for(unsigned mode=1;mode<=10;mode++){configure(mode);assert(!observer_client_rates(123,&rate,&ready)&&!ready&&rate==0);}
 for(unsigned mode=1;mode<=10;mode++){configure(mode);memset(&snapshot,255,sizeof snapshot);assert(!observer_client_read(123,&snapshot));if(mode<=4)assert(!snapshot.abi&&!snapshot.accepted);else assert(snapshot.abi==MONADO_PRESENT_OBSERVER_ABI&&snapshot.accepted==701&&snapshot.failed==4);}
 configure(11);assert(observer_client_rates(123,&rate,&ready)&&!ready&&rate==0); // attached, still warming
 configure(0);observer_client_disable();assert(!observer_client_rates(123,&rate,&ready)&&!ready&&rate==0);assert(!observer_client_begin(true)&&resources==1);
 assert(!observer_client_rates(123,NULL,&ready));assert(!observer_client_rates(123,&rate,NULL));puts("actual optional client: PASS");return 0;
}
