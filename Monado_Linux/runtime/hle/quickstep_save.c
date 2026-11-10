// [XCDE] LINUX-039: authored optional per-save references. Native payloads and
// inventory items are never modified here. The filesystem owns commit ordering.
#include "../hle.h"
#include "../settings.h"
#include "quickstep_save.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { CAPTURES = 32, OBSERVATIONS = 256, PENDING = 32, PATH_CAP = 1100 };
static const char metadata_dir[] = ".monado-quickstep";
static const uint8_t magic[8] = {'M','I','I','Q','S','0','1',0};

// Authored SHA-256 for accidental stale-payload binding and metadata integrity.
// The digest is not an authentication signature or an anti-cheat mechanism.
static uint32_t rr(uint32_t n, unsigned b) { return (n >> b) | (n << (32 - b)); }
static void sha_block(uint32_t s[8], const uint8_t *p) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 | (uint32_t)p[i*4+2]<<8 | p[i*4+3];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t a = w[i-15], b = w[i-2];
        w[i] = w[i-16] + (rr(a,7)^rr(a,18)^(a>>3)) + w[i-7] + (rr(b,17)^rr(b,19)^(b>>10));
    }
    uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t t=h+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(rr(a,2)^rr(a,13)^rr(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
}
void quickstep_save_hash(const void *data, size_t bytes, uint8_t out[32]) {
    uint32_t s[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint8_t *p=data; size_t left=bytes;
    while (left >= 64) { sha_block(s,p);p+=64;left-=64; }
    uint8_t tail[128]={0};if(left)memcpy(tail,p,left);tail[left]=0x80;
    size_t used=left<56?64:128;uint64_t bits=(uint64_t)bytes*8;
    for (unsigned i=0;i<8;++i) tail[used-1-i]=(uint8_t)(bits>>(i*8));
    sha_block(s,tail);if(used==128)sha_block(s,tail+64);
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4;++j)out[i*4+j]=(uint8_t)(s[i]>>(24-j*8));
}
static uint32_t get32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t get64(const uint8_t *p) { return get32(p)|(uint64_t)get32(p+4)<<32; }
static void put32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(i*8)); }
static void put64(uint8_t *p,uint64_t n) { put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32)); }
static int bad(int error) { errno=error;return -1; }

static bool identity(const char *name,uint32_t *story,uint32_t *file,uint32_t *automatic) {
    size_t prefix;
    if(!strncmp(name,"bfsgame",7)){*story=0;prefix=7;}
    else if(!strncmp(name,"bfsmeria",8)){*story=1;prefix=8;}
    else return false;
    size_t n=strlen(name);
    if(n!=prefix+6 && n!=prefix+7)return false;
    if(name[prefix]<'0'||name[prefix]>'9'||name[prefix+1]<'0'||name[prefix+1]>'9')return false;
    *file=(unsigned)(name[prefix]-'0')*10+(unsigned)(name[prefix+1]-'0');
    *automatic=n==prefix+7;
    return *file<99 && (!*automatic || (!*file && name[prefix+2]=='a'))
        && !strcmp(name+prefix+2+*automatic,".sav");
}
static const char *relative(const char *root,const char *path) {
    size_t n=strlen(root);uint32_t story,file,automatic;
    if(n>=PATH_CAP||strncmp(root,path,n)||path[n]!='/')return NULL;
    const char *name=path+n+1;
    return strlen(name)<32 && identity(name,&story,&file,&automatic)?name:NULL;
}
static bool valid(const QuickstepSnapshot *s) {
    if(s->story>1||s->count>QUICKSTEP_CHARACTER_CAPACITY)return false;
    for(uint32_t i=0;i<s->count;++i) {
        const QuickstepEntry *e=&s->entries[i];
        if(!e->character||e->character>=16||!e->uid||!get32(e->fingerprint))return false;
        for(uint32_t j=0;j<i;++j)
            if(e->character==s->entries[j].character||e->uid==s->entries[j].uid)return false;
    }
    return true;
}
int quickstep_save_encode(const char *name,const uint8_t hash[32],uint64_t size,
                         const QuickstepSnapshot *s,uint8_t out[QUICKSTEP_META_MAX],size_t *bytes) {
    uint32_t story,file,automatic;
    if(!identity(name,&story,&file,&automatic)||!valid(s)||s->story!=story||!size||size>QUICKSTEP_SAVE_BYTES)return bad(EINVAL);
    size_t n=72+s->count*40+32;memset(out,0,n);memcpy(out,magic,8);
    put32(out+8,1);put32(out+12,(uint32_t)n);put32(out+16,story);put32(out+20,file);
    put32(out+24,automatic);put32(out+28,s->count);put64(out+32,size);memcpy(out+40,hash,32);
    for(uint32_t i=0;i<s->count;++i) {
        uint8_t *p=out+72+i*40;put32(p,s->entries[i].character);put32(p+4,s->entries[i].uid);
        memcpy(p+8,s->entries[i].fingerprint,32);
    }
    quickstep_save_hash(out,n-32,out+n-32);*bytes=n;return 0;
}
int quickstep_save_decode(const char *name,const uint8_t hash[32],uint64_t size,
                         const uint8_t *data,size_t bytes,QuickstepSnapshot *s) {
    uint32_t story,file,automatic;uint8_t check[32];
    memset(s,0,sizeof *s);
    if(!identity(name,&story,&file,&automatic)||bytes<104||bytes>QUICKSTEP_META_MAX)return bad(EINVAL);
    uint32_t count=get32(data+28);
    if(memcmp(data,magic,8)||get32(data+8)!=1||get32(data+12)!=bytes||count>16
       ||bytes!=104+count*40||get32(data+16)!=story||get32(data+20)!=file
       ||get32(data+24)!=automatic||get64(data+32)!=size||memcmp(data+40,hash,32))return bad(EINVAL);
    quickstep_save_hash(data,bytes-32,check);if(memcmp(check,data+bytes-32,32))return bad(EINVAL);
    s->story=story;s->count=count;
    for(uint32_t i=0;i<count;++i) {
        const uint8_t *p=data+72+i*40;s->entries[i].character=get32(p);s->entries[i].uid=get32(p+4);
        memcpy(s->entries[i].fingerprint,p+8,32);
    }
    if(!valid(s)){memset(s,0,sizeof *s);return bad(EINVAL);}return 0;
}

typedef struct { uint64_t address,age;uint8_t hash[32];QuickstepSnapshot snapshot; } Capture;
typedef struct {
    uint64_t age,address,size;char root[PATH_CAP],name[32];
    bool assembled,copied,wire_known;uint8_t hash[32];QuickstepSnapshot snapshot;
} Observation;
typedef struct { uint64_t age,address;char root[PATH_CAP],name[32];uint8_t bytes[48]; } HeaderRead;
typedef struct { uint64_t age,address;uint32_t story;char name[32]; } ReadIntent;
typedef struct { bool used;char root[PATH_CAP];QuickstepSaveWrite write; } Pending;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
// Adoption itself calls back into set_required(), so it cannot hold lock.
// This separate short gate serializes lifecycle invalidation/suspend/adoption;
// never hold it across a native helper, story resolution or stat refresh.
static pthread_mutex_t transition=PTHREAD_MUTEX_INITIALIZER;
static Capture captures[CAPTURES];static Observation observations[OBSERVATIONS];static Pending pending[PENDING];
static ReadIntent read_intents[OBSERVATIONS];
static HeaderRead headers[CAPTURES];
static uint64_t age;static bool required,has_intent,have_load,load_selected;
static uint64_t lifecycle,load_lifecycle;
static char intent[32];static uint32_t intent_story;
static char selected_name[32];static uint32_t selected_story;
static uint64_t load_context,load_playing;static QuickstepSnapshot load_snapshot;
static unsigned restore_messages;
// [XCDE] LINUX-044: bounded local diagnostics distinguish an intentionally empty
// optional slot from a rejected restore. Never print native payload or identity.
static void restore_log(const char *reason,const char *name,unsigned count) {
    if(__atomic_fetch_add(&restore_messages,1,__ATOMIC_RELAXED)<96)
        LOG("[quickstep-save] restore %s file=%s entries=%u\n",reason,name&&*name?name:"unknown",count);
}
void quickstep_save_set_required(bool value) { pthread_mutex_lock(&lock);required=value;pthread_mutex_unlock(&lock); }
static bool needed(void) { pthread_mutex_lock(&lock);bool value=required||g_settings.quickstep_slot;pthread_mutex_unlock(&lock);return value; }

static int directory(const char *root,bool create) {
    int fd=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    if(create&&mkdirat(fd,metadata_dir,0700)&&errno!=EEXIST){int e=errno;close(fd);return bad(e);}
    int dir=openat(fd,metadata_dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    int e=errno;
    if(create&&dir>=0&&fsync(fd)){e=errno;close(dir);dir=-1;}
    close(fd);errno=e;return dir;
}
static int metadata_read(const char *root,const char *name,uint8_t data[QUICKSTEP_META_MAX],size_t *size) {
    int dir=directory(root,false);if(dir<0)return -1;
    int fd=openat(dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);int e=errno;close(dir);if(fd<0)return bad(e);
    struct stat st;int result=0;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size<104||st.st_size>QUICKSTEP_META_MAX)result=bad(EINVAL);
    else {
        ssize_t got=pread(fd,data,(size_t)st.st_size,0);
        if(got!=st.st_size)result=bad(got<0?errno:EIO);else *size=(size_t)got;
    }
    e=errno;if(close(fd)&&!result){result=-1;e=errno;}errno=e;return result;
}
static unsigned observation_slot(uint64_t address) {
    unsigned slot=0;
    for(unsigned i=0;i<OBSERVATIONS;++i) {
        if(observations[i].address==address)return i;
        if(observations[i].age<observations[slot].age)slot=i;
    }
    return slot;
}
static void bind_wire(Observation *seen) {
    uint8_t data[QUICKSTEP_META_MAX];size_t n;
    // Scans with no optional metadata must not hash every1.4MiB native file.
    // The reader has completed this exact wire buffer; hashes are captured now,
    // before native typed copies can omit padding or recycle the source memory.
    if(metadata_read(seen->root,seen->name,data,&n))return;
    quickstep_save_hash((const void *)(uintptr_t)seen->address,QUICKSTEP_SAVE_BYTES,seen->hash);
    if(!quickstep_save_decode(seen->name,seen->hash,seen->size,data,n,&seen->snapshot))seen->wire_known=true;
}
void quickstep_save_observe(const char *root,const char *path,const void *payload,int64_t offset,int64_t bytes) {
    const char *name=relative(root,path);if(!name||offset<0||bytes<=0||bytes>QUICKSTEP_SAVE_BYTES)return;
    uint64_t address=(uint64_t)(uintptr_t)payload;
    if(!offset&&bytes==48) {
        HeaderRead item={.address=address};strcpy(item.root,root);strcpy(item.name,name);memcpy(item.bytes,payload,48);
        pthread_mutex_lock(&lock);unsigned slot=0;
        for(unsigned i=0;i<CAPTURES;++i) {
            if(headers[i].address==address){slot=i;break;}
            if(headers[i].age<headers[slot].age)slot=i;
        }
        item.age=++age;headers[slot]=item;pthread_mutex_unlock(&lock);return;
    }
    Observation seen={.address=address,.size=(uint64_t)bytes};strcpy(seen.root,root);strcpy(seen.name,name);
    pthread_mutex_lock(&lock);uint64_t epoch=lifecycle,token=0;
    if(offset) {
        bool found=false;
        for(unsigned i=0;i<OBSERVATIONS;++i) {
            Observation *item=&observations[i];
            if(item->assembled&&item->size==(uint64_t)offset&&item->address+(uint64_t)offset==address
                    &&!strcmp(item->root,root)&&!strcmp(item->name,name)
                    &&(uint64_t)bytes<=QUICKSTEP_SAVE_BYTES-item->size) {
                seen=*item;token=item->age;seen.size+=(uint64_t)bytes;found=true;break;
            }
        }
        if(!found){pthread_mutex_unlock(&lock);return;}
    } else {
        if(bytes<1024*1024){pthread_mutex_unlock(&lock);return;}
        unsigned slot=observation_slot(address);
        if(observations[slot].address==address&&observations[slot].size==QUICKSTEP_SAVE_BYTES
                &&bytes!=QUICKSTEP_SAVE_BYTES&&!strcmp(observations[slot].root,root)&&!strcmp(observations[slot].name,name)) {
            pthread_mutex_unlock(&lock);return;
        }
    }
    // Reserve the actual read event before metadata I/O/hash work. A newer
    // header/read/copy at this address must defeat the old publication.
    unsigned reserved=observation_slot(seen.address);seen.age=++age;token=seen.age;observations[reserved]=seen;
    pthread_mutex_unlock(&lock);
    if(seen.size==QUICKSTEP_SAVE_BYTES)bind_wire(&seen);
    // Do not hash speculative title/100-slot reads. The confirmed adoption path
    // opens exact-filename metadata first, then checks the adopted raw payload.
    pthread_mutex_lock(&lock);unsigned slot=observation_slot(seen.address);
    if(epoch==lifecycle&&observations[slot].address==seen.address&&observations[slot].age==token) {
        observations[slot]=seen;
    }
    pthread_mutex_unlock(&lock);
}
int quickstep_save_prepare(const char *root,const char *path,const void *payload,int64_t offset,int64_t bytes,QuickstepSaveWrite *plan) {
    memset(plan,0,sizeof *plan);const char *name=relative(root,path);if(!name)return 0;
    uint8_t prior[QUICKSTEP_META_MAX];size_t prior_size=0;
    bool exists=metadata_read(root,name,prior,&prior_size)==0;
    if(!exists&&errno!=ENOENT)return -1;
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<PENDING;++i)
        if(pending[i].used&&!strcmp(pending[i].root,root)&&!strcmp(pending[i].write.name,name))exists=true;
    pthread_mutex_unlock(&lock);
    bool must=needed()||exists;
    if(offset||bytes!=QUICKSTEP_SAVE_BYTES)return must?bad(EINVAL):0;
    uint8_t hash[32];quickstep_save_hash(payload,(size_t)bytes,hash);QuickstepSnapshot snapshot={0};bool found=false,ambiguous=false;
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<CAPTURES;++i)if(captures[i].age&&!memcmp(captures[i].hash,hash,32)) {
        if(captures[i].address==(uint64_t)(uintptr_t)payload){snapshot=captures[i].snapshot;found=true;ambiguous=false;break;}
        if(found&&memcmp(&snapshot,&captures[i].snapshot,sizeof snapshot))ambiguous=true;
        else {snapshot=captures[i].snapshot;found=true;}
    }
    pthread_mutex_unlock(&lock);
    if(!found||ambiguous)return must?bad(EIO):0;
    uint32_t story,file,automatic;(void)identity(name,&story,&file,&automatic);
    if(!snapshot.count)snapshot.story=story;
    if(snapshot.story!=story)return bad(EINVAL);
    if(!snapshot.count&&!exists)return 0; // Default OFF/unused slots create no optional files.
    plan->tracked=true;strcpy(plan->name,name);memcpy(plan->payload_hash,hash,32);
    if(snapshot.count&&quickstep_save_encode(name,hash,(uint64_t)bytes,&snapshot,plan->metadata,&plan->bytes))return -1;
    return 0; // bytes0 means remove stale optional metadata at this commit.
}
int quickstep_save_stage(const char *root,const QuickstepSaveWrite *plan) {
    if(!plan->tracked)return 0;if(strlen(root)>=PATH_CAP)return bad(ENAMETOOLONG);
    pthread_mutex_lock(&lock);int index=-1;
    for(unsigned i=0;i<PENDING;++i) {
        if(pending[i].used&&!strcmp(pending[i].root,root)&&!strcmp(pending[i].write.name,plan->name)){index=(int)i;break;}
        if(!pending[i].used&&index<0)index=(int)i;
    }
    if(index>=0){pending[index].used=true;strcpy(pending[index].root,root);pending[index].write=*plan;}
    pthread_mutex_unlock(&lock);return index<0?bad(ENOSPC):0;
}
static uint8_t *payload_read(const char *root,const char *name) {
    int base=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(base<0)return NULL;
    int fd=openat(base,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);int error=errno;close(base);
    if(fd<0){errno=error;return NULL;}
    struct stat st;uint8_t *contents=NULL;
    if(fstat(fd,&st))error=errno;
    else if(!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size!=QUICKSTEP_SAVE_BYTES)error=EINVAL;
    else if(!(contents=malloc(QUICKSTEP_SAVE_BYTES)))error=ENOMEM;
    else {
        error=0;size_t at=0;
        while(at<QUICKSTEP_SAVE_BYTES) {
            ssize_t n=pread(fd,contents+at,QUICKSTEP_SAVE_BYTES-at,(off_t)at);
            if(n<0&&errno==EINTR)continue;
            if(n<=0){error=n<0?errno:EIO;break;}at+=(size_t)n;
        }
    }
    if(close(fd)&&!error)error=errno;
    if(error){free(contents);contents=NULL;}errno=error;return contents;
}
int quickstep_save_changed(const char *root,const char *path,const char *destination) {
    const char *old=relative(root,path),*next=destination?relative(root,destination):NULL;
    if(old&&next&&!strcmp(old,next))return 0;
    uint8_t metadata[QUICKSTEP_META_MAX];size_t metadata_size=0;bool prior=false,saw_pending=false;
    if(old) {
        pthread_mutex_lock(&lock);
        for(unsigned i=0;i<PENDING;++i)if(pending[i].used&&!strcmp(pending[i].root,root)
                &&!strcmp(pending[i].write.name,old)) {
            saw_pending=true;metadata_size=pending[i].write.bytes;
            if(metadata_size){memcpy(metadata,pending[i].write.metadata,metadata_size);prior=true;}break;
        }
        pthread_mutex_unlock(&lock);
        if(!saw_pending) {
            prior=metadata_read(root,old,metadata,&metadata_size)==0;
            if(!prior&&errno!=ENOENT)return -1;
        }
    }
    if(next) {
        if(!prior&&!saw_pending&&!needed()) {
            uint8_t destination_meta[QUICKSTEP_META_MAX];size_t n;
            if(metadata_read(root,next,destination_meta,&n)) {
                if(errno==ENOENT)return 0;return -1;
            }
        }
        uint8_t *contents=payload_read(root,next);if(!contents)return -1;
        QuickstepSaveWrite plan={0};int result=0;
        if(prior) {
            uint8_t hash[32];quickstep_save_hash(contents,QUICKSTEP_SAVE_BYTES,hash);
            QuickstepSnapshot snapshot;
            if(quickstep_save_decode(old,hash,QUICKSTEP_SAVE_BYTES,metadata,metadata_size,&snapshot))result=-1;
            else {
                plan.tracked=true;strcpy(plan.name,next);memcpy(plan.payload_hash,hash,32);
                result=quickstep_save_encode(next,hash,QUICKSTEP_SAVE_BYTES,&snapshot,plan.metadata,&plan.bytes);
            }
        } else result=quickstep_save_prepare(root,destination,contents,0,QUICKSTEP_SAVE_BYTES,&plan);
        free(contents);if(result||quickstep_save_stage(root,&plan))return -1;
    }
    if(prior||saw_pending) {
        QuickstepSaveWrite removed={.tracked=true,.deleted=true};strcpy(removed.name,old);
        if(quickstep_save_stage(root,&removed))return -1;
    }
    return 0;
}
int quickstep_save_flush(const char *root) {
    // FS holds its save-transaction mutex across payload writes and Commit.
    for(unsigned i=0;i<PENDING;++i) {
        pthread_mutex_lock(&lock);Pending entry=pending[i];pthread_mutex_unlock(&lock);
        if(!entry.used||strcmp(entry.root,root))continue;
        int error=0;
        if(entry.write.deleted) {
            int base=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(base<0)return -1;
            struct stat st;int rc=fstatat(base,entry.write.name,&st,AT_SYMLINK_NOFOLLOW);int e=errno;close(base);
            if(!rc||e!=ENOENT)return bad(!rc?EIO:e);
        } else {
            uint8_t *contents=payload_read(root,entry.write.name);if(!contents)return -1;
            uint8_t hash[32];quickstep_save_hash(contents,QUICKSTEP_SAVE_BYTES,hash);free(contents);
            if(memcmp(hash,entry.write.payload_hash,32))return bad(EIO);
        }
        int dir=directory(root,entry.write.bytes!=0);
        if(dir<0){if(!entry.write.bytes&&errno==ENOENT)continue;return -1;}
        error=0;
        if(!entry.write.bytes) {if(unlinkat(dir,entry.write.name,0)&&errno!=ENOENT)error=errno;}
        else {
            char temporary[48];snprintf(temporary,sizeof temporary,"%s.tmp",entry.write.name);
            int fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
            if(fd<0)error=errno;
            else {
                size_t at=0;
                while(at<entry.write.bytes) {
                    ssize_t n=write(fd,entry.write.metadata+at,entry.write.bytes-at);
                    if(n<0&&errno==EINTR)continue;
                    if(n<=0){error=n<0?errno:EIO;break;}at+=(size_t)n;
                }
                if(!error&&fsync(fd))error=errno;
                if(close(fd)&&!error)error=errno;
                if(!error&&renameat(dir,temporary,dir,entry.write.name))error=errno;
                if(error)unlinkat(dir,temporary,0);
            }
        }
        if(!error&&fsync(dir))error=errno;
        if(close(dir)&&!error)error=errno;
        if(error)return bad(error);
    }
    return 0;
}
void quickstep_save_committed(const char *root) {
    pthread_mutex_lock(&lock);for(unsigned i=0;i<PENDING;++i)
        if(pending[i].used&&!strcmp(pending[i].root,root))memset(&pending[i],0,sizeof pending[i]);
    pthread_mutex_unlock(&lock);
}

static bool range(uint64_t p,uint64_t n) { return p>=GUEST_LO&&p<GUEST_HI&&n<=GUEST_HI-p; }
// Native typed SaveData copies intentionally omit padding. Retain the original
// validated wire digest, while proving each referenced gem is the exact copied
// owned instance; rank/potency alone must never recover an association.
static bool raw_references(uint64_t raw,const QuickstepSnapshot *s) {
    if(!range(raw,QUICKSTEP_SAVE_BYTES))return false;
    for(unsigned i=0;i<s->count;++i) {
        const QuickstepEntry *e=&s->entries[i];
        if((e->uid&0xffff0000u)!=0x30000u||(e->uid&0xffffu)>=QUICKSTEP_GEM_CAPACITY)return false;
        uint64_t item=raw+0x2c380+(e->uid&0xffffu)*0x2c;uint8_t fingerprint[32];
        if(LD32(item)!=e->uid||LD16(item+6)!=3||!LD8(item+0x10)
                ||LD8(item+0x1a)!=1||LD16(item+0x1c)!=77||!LD8(item+0x1e)||LD8(item+0x1e)>25
                ||LD8(item+0x16)<1||LD8(item+0x16)>6)return false;
        memcpy(fingerprint,(void *)(uintptr_t)(item+0xc),4);
        memcpy(fingerprint+4,(void *)(uintptr_t)(item+4),4);
        memcpy(fingerprint+8,(void *)(uintptr_t)(item+0x14),24);
        if(memcmp(fingerprint,e->fingerprint,32))return false;
    }
    return true;
}
static void invalidate_raw(uint64_t address) {
    if(!range(address,QUICKSTEP_SAVE_BYTES))return;
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<OBSERVATIONS;++i)if(observations[i].address==address)memset(&observations[i],0,sizeof observations[i]);
    pthread_mutex_unlock(&lock);
}
void quickstep_save_header_copy(Cpu *c) {
    uint64_t source=c->sp+0x70,destination=c->x[8];
    if(!range(source,48)||!range(destination,QUICKSTEP_SAVE_BYTES)){invalidate_raw(destination);return;}
    pthread_mutex_lock(&lock);HeaderRead found={0};
    for(unsigned i=0;i<CAPTURES;++i)if(headers[i].address==source&&headers[i].age>found.age)found=headers[i];
    unsigned slot=observation_slot(destination);memset(&observations[slot],0,sizeof observations[slot]);
    if(found.age&&!memcmp(found.bytes,(void *)(uintptr_t)source,48)
            &&!memcmp(found.bytes,(void *)(uintptr_t)destination,48)&&get32(found.bytes)!=0x31636278u) {
        Observation item={.address=destination,.size=48,.assembled=true,.age=++age};
        strcpy(item.root,found.root);strcpy(item.name,found.name);observations[slot]=item;
    }
    pthread_mutex_unlock(&lock);
}
static void copied_raw(uint64_t destination,uint64_t source) {
    if(!range(destination,QUICKSTEP_SAVE_BYTES))return;
    if(!range(source,QUICKSTEP_SAVE_BYTES)){invalidate_raw(destination);return;}
    pthread_mutex_lock(&lock);Observation item={0};uint64_t epoch=lifecycle;
    for(unsigned i=0;i<OBSERVATIONS;++i)if(observations[i].address==source&&observations[i].age>item.age)item=observations[i];
    unsigned slot=observation_slot(destination);
    uint64_t destination_age=observations[slot].address==destination?observations[slot].age:0;
    pthread_mutex_unlock(&lock);
    bool valid=item.age&&item.size==QUICKSTEP_SAVE_BYTES&&item.wire_known
        &&raw_references(source,&item.snapshot)&&raw_references(destination,&item.snapshot);
    if(valid&&!item.copied) {
        uint8_t hash[32];quickstep_save_hash((void *)(uintptr_t)source,QUICKSTEP_SAVE_BYTES,hash);
        valid=!memcmp(hash,item.hash,32);
    }
    pthread_mutex_lock(&lock);unsigned source_slot=observation_slot(source);slot=observation_slot(destination);
    uint64_t current_destination_age=observations[slot].address==destination?observations[slot].age:0;
    if(lifecycle==epoch&&destination_age==current_destination_age) {
        valid=valid&&observations[source_slot].address==source&&observations[source_slot].age==item.age;
        memset(&observations[slot],0,sizeof observations[slot]);
        if(valid){item.address=destination;item.copied=true;item.age=++age;observations[slot]=item;}
    }
    pthread_mutex_unlock(&lock);
}
void quickstep_save_cache_copy(Cpu *c) { copied_raw(c->x[19],c->x[20]); }
void quickstep_save_selected_copy(Cpu *c) {
    if(range(c->x[19],8))copied_raw(c->x[21],LD64(c->x[19]));else invalidate_raw(c->x[21]);
}
static uint32_t story(Cpu *c,uint64_t context) {
    Cpu saved=*c;uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();
    if((c->sp&15)||!range(c->sp-0x400,0x400))FATAL("[quickstep-save] invalid native stack\n");
    uint64_t ref=c->sp-16;ST64(ref,context);c->sp-=0x400;c->x[0]=ref;
    guest_call(c,0x7000592a30ull);uint64_t view=c->x[0];*c=saved;rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);
    if(!range(view,0x151))FATAL("[quickstep-save] invalid native story view\n");
    return(LD8(view+0x150)>>3)&1;
}
void quickstep_save_serializing(Cpu *c) {
    Cpu saved=*c;uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();
    quickstep_slot_tick(c,c->x[1]);*c=saved;rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);
}
void quickstep_save_serialized(Cpu *c) {
    uint64_t address=c->x[19];if(!range(address,QUICKSTEP_SAVE_BYTES))return;
    Capture item={.address=address};
    bool owned=quickstep_slot_snapshot_for_save(address,&item.snapshot)&&valid(&item.snapshot);
    if(!owned) {
        pthread_mutex_lock(&lock);for(unsigned i=0;i<CAPTURES;++i)
            if(captures[i].address==address)memset(&captures[i],0,sizeof captures[i]);
        pthread_mutex_unlock(&lock);return;
    }
    quickstep_save_hash((const void *)(uintptr_t)address,QUICKSTEP_SAVE_BYTES,item.hash);
    pthread_mutex_lock(&lock);unsigned slot=0;
    for(unsigned i=0;i<CAPTURES;++i) {
        if(captures[i].address==address){slot=i;break;}
        if(captures[i].age<captures[slot].age)slot=i;
    }
    item.age=++age;captures[slot]=item;pthread_mutex_unlock(&lock);
}
void quickstep_save_intent(uint32_t value,uint16_t file,bool automatic) {
    if(value>1||file>=99||(automatic&&file)) {
        pthread_mutex_lock(&lock);has_intent=false;pthread_mutex_unlock(&lock);return;
    }
    char name[32];snprintf(name,sizeof name,"%s%02u%s.sav",value?"bfsmeria":"bfsgame",file,automatic?"a":"");
    pthread_mutex_lock(&lock);strcpy(intent,name);intent_story=value;has_intent=true;pthread_mutex_unlock(&lock);
}
void quickstep_save_read_intent(Cpu *c) {
    Cpu saved=*c;uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();
    // Checked full-read entry87040 uses task+150 as the native raw buffer. A
    // scan may start another read while the chosen load is pending: retain each
    // task's intent, never a global "most recently read slot" association.
    uint64_t task=c->x[0],address=task+0x150;
    uint16_t file=(uint16_t)c->x[1];bool automatic=(c->x[2]&1)!=0;
    pthread_mutex_lock(&lock);unsigned slot=0;uint64_t epoch=lifecycle,token=++age;
    for(unsigned i=0;i<OBSERVATIONS;++i) {
        if(read_intents[i].address==address){slot=i;break;}
        if(read_intents[i].age<read_intents[slot].age)slot=i;
    }
    // Reserve before the native story helper can yield. A newer read of this
    // task, or a load/newgame/reset, invalidates this exact pending publication.
    read_intents[slot]=(ReadIntent){.address=address,.age=token};pthread_mutex_unlock(&lock);
    if(range(task,0x150)&&range(address,QUICKSTEP_SAVE_BYTES)&&file<99&&(!automatic||!file)) {
        ReadIntent item={.address=address,.story=story(c,LD64(task+8))};
        snprintf(item.name,sizeof item.name,"%s%02u%s.sav",item.story?"bfsmeria":"bfsgame",file,automatic?"a":"");
        pthread_mutex_lock(&lock);
        if(lifecycle==epoch&&read_intents[slot].address==address&&read_intents[slot].age==token) {
            item.age=token;read_intents[slot]=item;
        }
        pthread_mutex_unlock(&lock);
    }
    *c=saved;rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);
}
void quickstep_save_continue_intent(Cpu *c) {
    Cpu saved=*c;uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();uint64_t owner=c->x[19];
    pthread_mutex_lock(&lock);uint64_t epoch=lifecycle;has_intent=false;pthread_mutex_unlock(&lock);
    if(range(owner,0xb0)) {
        uint64_t holder=LD64(owner+0x10),context=range(holder,0x30)?LD64(holder+0x28):0;
        if(range(context,0x18de4)) {
            uint32_t index=LD32(context+0x18de0);
            if(index<100) {
                uint32_t value=story(c,LD64(owner+0xa8));
                pthread_mutex_lock(&lock);
                if(lifecycle==epoch) {
                    snprintf(intent,sizeof intent,"%s%02u%s.sav",value?"bfsmeria":"bfsgame",index?index-1:0,index?"":"a");
                    intent_story=value;has_intent=true;
                }
                pthread_mutex_unlock(&lock);
            }
        }
    }
    *c=saved;rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);
}
void quickstep_save_loading(void) {
    uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();
    pthread_mutex_lock(&transition);
    pthread_mutex_lock(&lock);++lifecycle;have_load=false;load_selected=has_intent;
    strcpy(selected_name,intent);selected_story=intent_story;has_intent=false;
    pthread_mutex_unlock(&lock);quickstep_slot_suspend();
    pthread_mutex_unlock(&transition);
    rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);
}
void quickstep_save_adopt(Cpu *c) {
    uint64_t raw=c->x[22];if(!range(raw,QUICKSTEP_SAVE_BYTES))return;
    char name[32]={0};uint32_t expected=0;bool selected;Observation found={0};uint8_t hash[32];
    pthread_mutex_lock(&lock);uint64_t epoch=lifecycle;selected=load_selected;strcpy(name,selected_name);expected=selected_story;
    for(unsigned i=0;i<OBSERVATIONS;++i)if(read_intents[i].age&&read_intents[i].name[0]&&read_intents[i].address==raw) {
        selected=true;strcpy(name,read_intents[i].name);expected=read_intents[i].story;
        memset(&read_intents[i],0,sizeof read_intents[i]);break;
    }
    // The actual decoded buffer, filename AND mount identify the selected read.
    // Another profile with the same filename must never provide its sidecar.
    for(unsigned i=0;i<OBSERVATIONS;++i) {
        Observation *item=&observations[i];
        if(item->address!=raw)continue;
        if(item->age>found.age)found=*item;
    }
    pthread_mutex_unlock(&lock);
    // A reused heap address may retain an old task/global intent. The exact
    // wire-validated native header/copy chain owns this decoded raw object.
    if(found.wire_known&&(found.assembled||found.copied)) {
        uint32_t file,automatic;
        selected=identity(found.name,&expected,&file,&automatic);strcpy(name,found.name);
    }
    QuickstepSnapshot snapshot={.story=expected};
    bool full=found.age&&found.size==QUICKSTEP_SAVE_BYTES&&!strcmp(found.name,name);
    const char *reason=!selected?"missing-intent":!full?"missing-full-read":"missing-metadata";
    if(selected&&full) {
        uint8_t bytes[QUICKSTEP_META_MAX];size_t size;
        if(!metadata_read(found.root,name,bytes,&size)) {
            if(found.copied&&found.wire_known)memcpy(hash,found.hash,32);
            else if(!found.copied)quickstep_save_hash((const void *)(uintptr_t)raw,QUICKSTEP_SAVE_BYTES,hash);
            else {memset(hash,0,sizeof hash);reason="missing-validated-wire";}
            if(strcmp(reason,"missing-validated-wire"))
                reason=quickstep_save_decode(name,hash,found.size,bytes,size,&snapshot)?"invalid-binding":"validated";
            if(snapshot.count&&found.copied&&!raw_references(raw,&snapshot)){memset(&snapshot,0,sizeof snapshot);snapshot.story=expected;reason="copied-instance-mismatch";}
        } else if(errno!=ENOENT)reason="metadata-unreadable";
    }
    restore_log(reason,name,snapshot.count);
    pthread_mutex_lock(&lock);
    if(lifecycle==epoch) {
        load_snapshot=snapshot;load_context=c->x[28];load_playing=c->x[20];load_lifecycle=epoch;have_load=true;load_selected=false;
    }
    pthread_mutex_unlock(&lock);
}
void quickstep_save_loaded(Cpu *c) {
    Cpu saved=*c;uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr(),context=c->x[19];
    QuickstepSnapshot snapshot;uint64_t playing=0,epoch=0;
    pthread_mutex_lock(&lock);if(have_load&&load_context==context){snapshot=load_snapshot;playing=load_playing;epoch=load_lifecycle;have_load=false;}
    pthread_mutex_unlock(&lock);
    if(playing) {
        uint32_t actual=story(c,context);
        pthread_mutex_lock(&transition);
        pthread_mutex_lock(&lock);bool same=epoch==lifecycle;pthread_mutex_unlock(&lock);
        if(!same){pthread_mutex_unlock(&transition);restore_log("stale-finalization",NULL,0);*c=saved;rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);return;}
        if(snapshot.story!=actual){restore_log("story-mismatch",NULL,snapshot.count);memset(&snapshot,0,sizeof snapshot);snapshot.story=actual;}
        quickstep_slot_adopt(playing,actual,&snapshot);pthread_mutex_unlock(&transition);
        quickstep_slot_tick(c,context);
        QuickstepSnapshot adopted;quickstep_slot_snapshot(&adopted);
        restore_log(adopted.count==snapshot.count?"adopted":"inventory-rejected",NULL,adopted.count);
    } else restore_log("context-mismatch",NULL,0);
    *c=saved;rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);
}
static void new_game_locked(uint64_t playing,uint32_t value) {
    QuickstepSnapshot empty={.story=value};pthread_mutex_lock(&lock);++lifecycle;has_intent=have_load=load_selected=false;
    memset(read_intents,0,sizeof read_intents);memset(observations,0,sizeof observations);memset(headers,0,sizeof headers);pthread_mutex_unlock(&lock);
    quickstep_slot_adopt(playing,value,&empty);
}
void quickstep_save_new_game(uint64_t playing,uint32_t value) {
    pthread_mutex_lock(&transition);new_game_locked(playing,value);pthread_mutex_unlock(&transition);
}
void quickstep_save_newgame_begin(Cpu *c) {
    Cpu saved=*c;uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();
    pthread_mutex_lock(&transition);
    pthread_mutex_lock(&lock);++lifecycle;has_intent=have_load=load_selected=false;intent[0]=0;
    memset(read_intents,0,sizeof read_intents);memset(observations,0,sizeof observations);memset(headers,0,sizeof headers);pthread_mutex_unlock(&lock);
    quickstep_slot_suspend();pthread_mutex_unlock(&transition);*c=saved;rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);
}
void quickstep_save_newgame_end(Cpu *c) {
    Cpu saved=*c;uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr(),context=c->x[19];
    pthread_mutex_lock(&lock);uint64_t epoch=lifecycle;pthread_mutex_unlock(&lock);
    if((c->sp&15)||!range(c->sp-0x400,0x400))FATAL("[quickstep-save] invalid new game stack\n");
    uint64_t ref=c->sp-16;ST64(ref,context);c->sp-=0x400;c->x[0]=ref;
    guest_call(c,0x7000592b80ull);uint64_t playing=c->x[0];*c=saved;rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);
    uint32_t value=story(c,context);pthread_mutex_lock(&transition);
    pthread_mutex_lock(&lock);bool same=epoch==lifecycle;pthread_mutex_unlock(&lock);
    if(same)new_game_locked(playing,value);
    pthread_mutex_unlock(&transition);
    if(same)quickstep_slot_tick(c,context);
    else restore_log("stale-newgame",NULL,0);
    *c=saved;rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);
}
RT_ON_RESET(quickstep_save_reset) {
    pthread_mutex_lock(&transition);
    pthread_mutex_lock(&lock);memset(captures,0,sizeof captures);memset(observations,0,sizeof observations);memset(read_intents,0,sizeof read_intents);memset(headers,0,sizeof headers);
    memset(pending,0,sizeof pending);required=has_intent=have_load=load_selected=false;++lifecycle;age=0;intent[0]=selected_name[0]=0;pthread_mutex_unlock(&lock);
    quickstep_slot_suspend();pthread_mutex_unlock(&transition);
    __atomic_store_n(&restore_messages,0,__ATOMIC_RELAXED);
}
