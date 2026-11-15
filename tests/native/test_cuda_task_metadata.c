// Authored Linux procfs/Landlock scope tests, MIT, no game/GPU/UI.
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "linux_state_guard.h"
static _Atomic long tid;static _Atomic int stop;
static void *thread(void *unused){(void)unused;atomic_store(&tid,syscall(SYS_gettid));while(!atomic_load(&stop))usleep(100);return 0;}
static void denied(const char*p,int flags){errno=0;int f=open(p,flags,0600);fprintf(stderr,"NEGATIVE path=%s fd=%d errno=%d\n",p,f,errno);if(f>=0)close(f);assert(f<0&&errno==EACCES);}
int main(int argc,char**argv){assert(argc==4);bool enabled = !strcmp(argv[3], "enabled");char state[4096],outside[4096],path[8192],error[512];assert(realpath(argv[1],state)&&!strcmp(state,argv[1]));assert(realpath(argv[2],outside)&&!strcmp(outside,argv[2]));assert(!chdir(state));
int baseline=open(outside,O_WRONLY|O_CLOEXEC);assert(baseline>=0);close(baseline);int ro=open(outside,O_RDONLY|O_CLOEXEC);assert(ro>=0);
int pipefd[2];assert(!pipe2(pipefd,O_CLOEXEC));pid_t child=fork();assert(child>=0);if(!child){close(pipefd[1]);char b;assert(read(pipefd[0],&b,1)==1);_exit(0);}close(pipefd[0]);
snprintf(path,sizeof path,"/proc/%ld/task/%ld/comm",(long)child,(long)child);baseline=open(path,O_WRONLY|O_CLOEXEC);assert(baseline>=0);close(baseline);
assert(rt_linux_state_guard(state,error,sizeof error));denied(outside,O_WRONLY|O_TRUNC|O_CLOEXEC);denied(path,O_WRONLY|O_TRUNC|O_CLOEXEC);
snprintf(path,sizeof path,"/proc/self/fd/%d",ro);denied(path,O_WRONLY|O_TRUNC|O_CLOEXEC);
pthread_t th;assert(!pthread_create(&th,0,thread,0));while(!atomic_load(&tid))usleep(100);int named = pthread_setname_np(th,"monado-future");char name[16];
if (enabled) assert(!named && !pthread_getname_np(th,name,sizeof name) && !strcmp(name,"monado-future"));
else assert(named == EACCES);
snprintf(path,sizeof path,"/proc/self/task/%ld/root%s",atomic_load(&tid),outside);denied(path,O_WRONLY|O_TRUNC|O_CLOEXEC);
snprintf(path,sizeof path,"/proc/self/task/%ld/fd/%d",atomic_load(&tid),ro);denied(path,O_WRONLY|O_TRUNC|O_CLOEXEC);
snprintf(path,sizeof path,"/proc/self/task/%ld/cwd/../outside/sentinel",atomic_load(&tid));denied(path,O_WRONLY|O_TRUNC|O_CLOEXEC);
snprintf(path,sizeof path,"%s/../outside/created",state);denied(path,O_WRONLY|O_CREAT|O_CLOEXEC);
snprintf(path,sizeof path,"/proc/self/task/%ld/new-file",atomic_load(&tid));errno=0;int f=open(path,O_WRONLY|O_CREAT|O_CLOEXEC,0600);assert(f<0);fprintf(stderr,"NEGATIVE new-proc-node fd=%d errno=%d\n",f,errno);
atomic_store(&stop,1);assert(!pthread_join(th,0));assert(write(pipefd[1],"x",1)==1);close(pipefd[1]);assert(waitpid(child,0,0)==child);char bytes[32]={0};assert(pread(ro,bytes,sizeof bytes,0)==15&&!memcmp(bytes,"owned-sentinel\n",15));close(ro);printf("PASS metadata enabled=%d, outside/process/alias writes denied, sentinel unchanged\n", enabled);return 0;}
