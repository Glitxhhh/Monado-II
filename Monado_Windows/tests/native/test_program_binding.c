// [XCDE] LINUX-046: actual authored command recorder + binding case, no GPU/game.
#include "hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym,fn) static void fn(Cpu *c)
#include MONADO_PROGRAM_CMDBUF_SOURCE
#include "nvn/program_binding.h"
#include <assert.h>
static struct { NvnProgram *program; NvnProgram bound_program; } st;
#include "program_bind_case.h"
int g_rt_unwind;
void rt_fatal_trace(void){abort();}
void rt_unwind_point(void){}
static NvnProgram *callback_program;
static unsigned callback_calls;
static unsigned char command_memory[16384];
void guest_call(Cpu*c,uint64_t fn){
 assert(fn==0x1234&&c->x[1]==0);++callback_calls;
 // A command allocator callback may destroy/reuse the original host program.
 memset(callback_program,0,sizeof*callback_program);
 NvnCmdBuf *cb=HOST(NvnCmdBuf,c->x[0]);cb->mem=(uint64_t)(uintptr_t)command_memory;
 cb->size=sizeof command_memory;cb->used=0;
}
static CmdRec *bind(NvnCmdBuf *cb,NvnProgram *program,uint32_t mask,bool callback){
 uint64_t guest_cb=(uint64_t)(uintptr_t)cb,guest_program=(uint64_t)(uintptr_t)program;
 cb->guest=(uint64_t)(uintptr_t)&guest_cb;
 cb->mem=callback?0:(uint64_t)(uintptr_t)command_memory;cb->used=0;cb->size=sizeof command_memory;
 cb->callback=callback?0x1234:0;callback_program=program;
 Cpu c={0};c.x[0]=cb->guest;c.x[1]=program?(uint64_t)(uintptr_t)&guest_program:0;c.x[2]=mask;
 hle_nvnCommandBufferBindProgram(&c);return(CmdRec*)command_memory;
}
static NvnProgram program(uint64_t base){
 NvnProgram p={0};p.stage_mask=0x3f;
 for(unsigned i=0;i<6;i++){p.stages[i].stage=i;p.stages[i].code_addr=base+0x100*i;p.stages[i].code_hash=base+i;}
 return p;
}
int main(void){
 NvnCmdBuf cb={0};NvnProgram a=program(0x1000),original=a;
 CmdRec *r=bind(&cb,&a,3,false);const NvnProgram *recorded=(void*)(uintptr_t)r->x[0];
 assert(recorded!=&a&&r->size==sizeof(CmdRec)+sizeof(NvnProgram));
 memset(&a,0,sizeof a);assert(!memcmp(recorded->stages,original.stages,sizeof a.stages));
 execute_binding(r);assert(st.program==&st.bound_program&&st.bound_program.stage_mask==3);
 assert(st.program->stages[1].code_addr==0x1100);
 // Every known stage is selected independently in the runtime's NVN order,
 // including compute, geometry and tessellation; zero mask is a true no-op.
 NvnProgram b=program(0x2000);r=bind(&cb,&b,1,false);execute_binding(r);
 assert(st.program->stages[0].code_addr==0x2000&&st.program->stages[1].code_addr==0x1100);
 NvnProgram before=st.bound_program;r=bind(&cb,&b,0,false);execute_binding(r);
 assert(!memcmp(&before,&st.bound_program,sizeof before));
 for(unsigned i=2;i<6;i++){r=bind(&cb,&b,1u<<i,false);execute_binding(r);assert(st.program->stages[i].code_addr==0x2000+0x100*i);}
 r=bind(&cb,NULL,2,false);execute_binding(r);
 assert(st.program&&!(st.program->stage_mask&2)&&st.program->stages[0].code_addr==0x2000);
 r=bind(&cb,NULL,0x3f,false);execute_binding(r);assert(!st.program&&!st.bound_program.stage_mask);
 // Program metadata must be captured before the actual record() allocator
 // callback, not reconstructed by re-reading its original guest handle later.
 a=program(0x3000);original=a;r=bind(&cb,&a,3,true);recorded=(void*)(uintptr_t)r->x[0];
 assert(callback_calls==1&&!memcmp(recorded->stages,original.stages,sizeof a.stages));
 execute_binding(r);assert(st.program->stages[1].code_addr==0x3100);
 // Ordinary immutable registrations retain the old small-record fast path.
 NvnProgram frozen=program(0x4000);a=program(0x5000);a.frozen=&frozen;
 r=bind(&cb,&a,3,true);assert(callback_calls==2&&r->x[0]==(uint64_t)(uintptr_t)&frozen);
 assert(r->size==sizeof(CmdRec));execute_binding(r);assert(st.program->stages[1].code_addr==0x4100);
 // State persists across command-list/submission boundaries without a borrowed
 // self pointer. A new subset bind changes only its selected stage.
 NvnProgram retained=st.bound_program;b=program(0x6000);r=bind(&cb,&b,1,false);execute_binding(r);
 assert(st.program==&st.bound_program&&st.program->stages[1].code_addr==retained.stages[1].code_addr);
 printf("native program recording, callback and six-stage binding contracts: PASS (record=%zu inline=%zu)\n",sizeof(CmdRec),sizeof(CmdRec)+sizeof(NvnProgram));
 return 0;
}
