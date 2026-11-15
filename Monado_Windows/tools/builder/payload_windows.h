// [XCDE] Authored portable Windows setup payload reader. MIT license.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>

namespace monado_builder {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default; explicit Handle(HANDLE h):value(h){}
    Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE && value)CloseHandle(value);}
};
inline void require(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
inline std::wstring wide(const std::string &s){
    if(s.empty())return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);
    require(n>0,"Invalid UTF-8 text");std::wstring r(n,0);
    require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),r.data(),n)==n,"Invalid UTF-8 text");return r;
}
inline std::string utf8(const std::wstring &s){
    if(s.empty())return {};
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    require(n>0,"Invalid Unicode text");std::string r(n,0);
    require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr)==n,"Invalid Unicode text");return r;
}
inline std::wstring quote(const std::wstring &arg){
    std::wstring r=L"\"";size_t n=0;
    for(wchar_t c:arg){if(c==L'\\'){++n;continue;}if(c==L'\"'){r.append(n*2+1,L'\\');r+=c;n=0;continue;}r.append(n,L'\\');n=0;r+=c;}
    r.append(n*2,L'\\');r+=L'\"';return r;
}
inline std::wstring executable(){std::vector<wchar_t> s(32768);DWORD n=GetModuleFileNameW(nullptr,s.data(),(DWORD)s.size());require(n && n<s.size(),"Cannot locate this setup executable");return std::wstring(s.data(),n);}
inline std::wstring native(const std::wstring &p){
    if(p.rfind(L"\\\\?\\",0)==0)return p;
    if(p.rfind(L"\\\\",0)==0)return L"\\\\?\\UNC\\"+p.substr(2);
    require(p.size()>=3&&p[1]==L':'&&p[2]==L'\\',"Absolute cache path required");return L"\\\\?\\"+p;
}
inline bool ordinary(const std::wstring &p,bool directory=false){
    DWORD a=GetFileAttributesW(native(p).c_str());if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT)||bool(a&FILE_ATTRIBUTE_DIRECTORY)!=directory)return false;
    if(directory)return true;Handle f(CreateFileW(native(p).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};
    return f.value!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(f.value,&info)&&info.nNumberOfLinks==1&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
inline void directory(const std::wstring &p){
    if(CreateDirectoryW(native(p).c_str(),nullptr))return;
    require(GetLastError()==ERROR_ALREADY_EXISTS && ordinary(p,true),"Cache directory is missing, busy or contains a link");
}
inline void seek(HANDLE f,uint64_t offset){LARGE_INTEGER x{};x.QuadPart=(LONGLONG)offset;require(SetFilePointerEx(f,x,nullptr,FILE_BEGIN),"Cannot seek setup payload");}
inline void read(HANDLE f,void *data,size_t bytes){auto p=(unsigned char*)data;while(bytes){DWORD count=0,want=(DWORD)std::min<size_t>(bytes,1<<20);require(ReadFile(f,p,want,&count,nullptr)&&count==want,"Truncated setup payload");p+=count;bytes-=count;}}
inline void write(HANDLE f,const void *data,size_t bytes){auto p=(const unsigned char*)data;while(bytes){DWORD count=0,want=(DWORD)std::min<size_t>(bytes,1<<20);require(WriteFile(f,p,want,&count,nullptr)&&count==want,"Cannot write setup cache");p+=count;bytes-=count;}}
struct Sha256 {
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::vector<unsigned char> object;
    Sha256(){
        require(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0,"Cannot initialize SHA-256");
        ULONG bytes=0,n=0;require(BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,(PUCHAR)&bytes,sizeof bytes,&n,0)==0,"Cannot initialize SHA-256");
        object.resize(bytes);require(BCryptCreateHash(alg,&hash,object.data(),bytes,nullptr,0,0)==0,"Cannot initialize SHA-256");
    }
    ~Sha256(){if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);}
    void add(const void *p,size_t size){auto b=(const unsigned char*)p;while(size){ULONG n=(ULONG)std::min<size_t>(size,1<<20);require(BCryptHashData(hash,(PUCHAR)b,n,0)==0,"Cannot hash setup payload");b+=n;size-=n;}}
    std::array<unsigned char,32> finish(){std::array<unsigned char,32> r;require(BCryptFinishHash(hash,r.data(),(ULONG)r.size(),0)==0,"Cannot finish setup checksum");return r;}
};
inline std::array<unsigned char,32> hash_range(HANDLE f,uint64_t offset,uint64_t bytes){
    seek(f,offset);Sha256 h;std::vector<unsigned char>b(1<<20);while(bytes){size_t n=(size_t)std::min<uint64_t>(b.size(),bytes);read(f,b.data(),n);h.add(b.data(),n);bytes-=n;}return h.finish();
}
inline std::string hex(const std::array<unsigned char,32>&v){const char *d="0123456789abcdef";std::string r;for(auto b:v){r+=d[b>>4];r+=d[b&15];}return r;}
inline void publish_new(const std::wstring &root,const std::wstring &target,const void *data,size_t bytes){
    std::array<unsigned char,32> nonce{};require(BCryptGenRandom(nullptr,nonce.data(),(ULONG)nonce.size(),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"Cannot name an exclusive setup cache temporary");
    std::wstring temporary=root+L"\\state\\extract-"+wide(hex(nonce))+L".part";
    {Handle output(CreateFileW(native(temporary).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));require(output.value!=INVALID_HANDLE_VALUE,"Cannot create setup cache temporary");write(output.value,data,bytes);require(FlushFileBuffers(output.value),"Cannot flush setup cache temporary");}
    // No replacement is permitted. An interrupted write stays in mutable state,
    // so retry can fill absent final members without mistaking partial bytes for
    // a changed immutable member or deleting an unknown user's file.
    require(MoveFileExW(native(temporary).c_str(),native(target).c_str(),MOVEFILE_WRITE_THROUGH),"Setup cache destination appeared or could not be published; existing files are preserved");
}
inline uint32_t u32(const unsigned char *p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
inline uint64_t u64(const unsigned char *p){return uint64_t(u32(p))|uint64_t(u32(p+4))<<32;}
inline std::wstring relative_name(const std::string &name){
    require(!name.empty()&&name.size()<=4096&&name.front()!='/'&&name.back()!='/',"Unsafe setup member path");
    std::wstring w=wide(name);size_t start=0;
    for(size_t i=0;i<=w.size();++i){
        if(i<w.size()){wchar_t c=w[i];require(c>=32&&c!=127&&c!=L'\\'&&c!=L':'&&c!=L'<'&&c!=L'>'&&c!=L'\"'&&c!=L'|'&&c!=L'?'&&c!=L'*',"Unsafe setup member path");if(c!=L'/')continue;}
        std::wstring part=w.substr(start,i-start);require(!part.empty()&&part!=L"."&&part!=L".."&&part.back()!=L'.'&&part.back()!=L' ',"Unsafe setup member component");
        auto dot=part.find(L'.');std::wstring stem=part.substr(0,dot);for(auto &c:stem)c=(wchar_t)towupper(c);
        bool reserved=stem==L"CON"||stem==L"PRN"||stem==L"AUX"||stem==L"NUL"||
            (stem.size()==4&&(stem.substr(0,3)==L"COM"||stem.substr(0,3)==L"LPT")&&stem[3]>=L'1'&&stem[3]<=L'9');
        require(!reserved,"Reserved Windows setup member name");start=i+1;
    }
    std::replace(w.begin(),w.end(),L'/',L'\\');return w;
}
inline void parents(const std::wstring &root,const std::wstring &relative){
    size_t pos=0;while((pos=relative.find(L'\\',pos))!=std::wstring::npos){directory(root+L"\\"+relative.substr(0,pos));++pos;}
}
inline void checked_members(const std::wstring &root,const std::wstring &relative,const std::set<std::wstring> &expected){
    WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW(native(root+L"\\"+relative+L"*").c_str(),&data);require(find!=INVALID_HANDLE_VALUE,"Cannot inspect immutable setup cache");
    try{do{
        std::wstring leaf=data.cFileName;if(leaf==L"."||leaf==L"..")continue;std::wstring child=relative+leaf,folded=child;for(auto &c:folded)c=(wchar_t)towlower(c);
        require(!(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Portable cache contains an unexpected filesystem link");
        if(relative.empty()&&leaf==L"state"&&(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))continue;
        if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)checked_members(root,child+L"\\",expected);
        else require(expected.count(folded)>0 || (relative.empty()&&(leaf==L".lock"||leaf==L".complete")),"Portable cache contains undeclared files; preserve it and use a fresh content-addressed cache");
    }while(FindNextFileW(find,&data));require(GetLastError()==ERROR_NO_MORE_FILES,"Cannot finish immutable cache inspection");FindClose(find);
    }catch(...){FindClose(find);throw;}
}
struct Payload {uint64_t offset=0,size=0,raw_size=0;uint32_t count=0;std::array<unsigned char,32> sha{},manifest_sha{};std::wstring cache;};
inline Payload inspect(HANDLE file){
    LARGE_INTEGER size{};require(GetFileSizeEx(file,&size)&&size.QuadPart>=80&&uint64_t(size.QuadPart)<=uint64_t(2304)*1024*1024,"This executable has no bounded portable setup payload");
    unsigned char footer[56];seek(file,(uint64_t)size.QuadPart-sizeof footer);read(file,footer,sizeof footer);
    require(memcmp(footer,"M2BNDL01",8)==0,"This executable has no portable setup payload");Payload p;
    p.offset=u64(footer+8);p.size=u64(footer+16);std::copy(footer+24,footer+56,p.sha.begin());
    require(p.offset>=4096&&p.size>=24&&p.size<=uint64_t(2)*1024*1024*1024&&p.size<=(uint64_t)size.QuadPart-sizeof footer&&p.offset==(uint64_t)size.QuadPart-sizeof footer-p.size,"Invalid setup payload bounds");
    require(hash_range(file,p.offset,p.size)==p.sha,"The portable setup checksum failed; obtain a fresh copy");
    unsigned char header[24];seek(file,p.offset);read(file,header,sizeof header);
    require(memcmp(header,"MONADOP1",8)==0&&u32(header+8)==1,"Unsupported setup payload");p.count=u32(header+12);
    p.raw_size=u64(header+16);require(p.count>0&&p.count<=100000&&p.raw_size<=uint64_t(4)*1024*1024*1024,"Setup payload exceeds limits");return p;
}
// Both first extraction and every reopen validate the whole immutable payload.
// Mutable build progress/registry lives in state/, outside all payload members.
inline void materialize(HANDLE file,Payload &p,const std::wstring &base,void(*progress)(uint32_t,uint32_t)=nullptr){
    directory(base);p.cache=base+L"\\"+wide(hex(p.sha));directory(p.cache);directory(p.cache+L"\\state");
    Handle lock(CreateFileW(native(p.cache+L"\\.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    require(lock.value!=INVALID_HANDLE_VALUE,"Another setup process is preparing this cache; retry after it finishes");
    FILE_ATTRIBUTE_TAG_INFO info{};require(GetFileInformationByHandleEx(lock.value,FileAttributeTagInfo,&info,sizeof info)&&!(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Unsafe setup cache lock");
    std::wstring marker=p.cache+L"\\.complete";bool reuse=ordinary(marker);uint64_t cursor=p.offset+24,total=0;std::wstring previous;std::set<std::wstring> expected_members;
    DECOMPRESSOR_HANDLE decompressor=nullptr;require(CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&decompressor),"Cannot initialize Windows payload decompression");
    try{
        for(uint32_t index=0;index<p.count;++index){
            require(cursor+56<=p.offset+p.size,"Truncated setup record");unsigned char h[56];seek(file,cursor);read(file,h,sizeof h);cursor+=sizeof h;
            uint32_t length=u32(h),flags=u32(h+4);uint64_t raw=u64(h+8),stored=u64(h+16);
            require(length>0&&length<=4096&&flags<=1&&raw<=512*1024*1024&&stored<=512*1024*1024&&cursor+length+stored<=p.offset+p.size&&(flags||raw==stored),"Invalid setup record bounds");
            std::string name(length,0);read(file,name.data(),length);cursor+=length;std::wstring relative=relative_name(name),folded=relative;for(auto &c:folded)c=(wchar_t)towlower(c);
            require(previous.empty()||previous<folded,"Duplicate or unordered setup member");previous=folded;expected_members.insert(folded);
            require(relative.rfind(L"state\\",0)!=0&&relative[0]!=L'.',"Payload cannot overwrite mutable setup state");
            std::array<unsigned char,32> expected;std::copy(h+24,h+56,expected.begin());std::wstring path=p.cache+L"\\"+relative;
            if(name=="payload-manifest.json")p.manifest_sha=expected;
            parents(p.cache,relative);
            if(reuse||GetFileAttributesW(native(path).c_str())!=INVALID_FILE_ATTRIBUTES){
                require(ordinary(path),"The portable cache changed; move its content-addressed folder aside and retry");
                Handle old(CreateFileW(native(path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));LARGE_INTEGER size{};
                require(old.value!=INVALID_HANDLE_VALUE&&GetFileSizeEx(old.value,&size)&&(uint64_t)size.QuadPart==raw&&hash_range(old.value,0,raw)==expected,"The portable cache checksum changed; move its content-addressed folder aside and retry");
            }else{
                std::vector<unsigned char> packed((size_t)stored),data((size_t)raw);seek(file,cursor);read(file,packed.data(),packed.size());
                if(flags){SIZE_T result=0;require(Decompress(decompressor,packed.data(),packed.size(),data.data(),data.size(),&result)&&result==raw,"Setup decompression failed");}
                else data.swap(packed);
                Sha256 sha;sha.add(data.data(),data.size());require(sha.finish()==expected,"Setup member checksum failed");
                publish_new(p.cache,path,data.data(),data.size());
            }
            cursor+=stored;total+=raw;require(total<=uint64_t(4)*1024*1024*1024,"Setup content exceeds limits");if(progress&&(index%32==0||index+1==p.count))progress(index+1,p.count);
        }
        require(cursor==p.offset+p.size&&total==p.raw_size,"Unexpected trailing setup data or content size");checked_members(p.cache,L"",expected_members);CloseDecompressor(decompressor);decompressor=nullptr;
        if(!reuse){auto sum=hex(p.sha);publish_new(p.cache,marker,sum.data(),sum.size());}
        directory(p.cache+L"\\state");
    }catch(...){if(decompressor)CloseDecompressor(decompressor);throw;}
}
} // namespace monado_builder
