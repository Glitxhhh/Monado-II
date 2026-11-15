// Ordinary module paths returned by Win32 may carry an extended DOS/UNC prefix.
// Normalize only those spellings before the existing ordinary-file guards;
// this never resolves links or accepts the NT/device namespace.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>

static wchar_t monado_module_upper(wchar_t value) {
    return value>=L'a'&&value<=L'z'?value-(L'a'-L'A'):value;
}
static bool monado_module_reserved(const wchar_t *name,size_t size) {
    size_t base=0;while(base<size&&name[base]!=L'.')base++;
    if(base==6||base==7) {
        const wchar_t *console=base==6?L"CONIN$":L"CONOUT$";bool same=true;
        for(size_t i=0;i<base;i++)same&=monado_module_upper(name[i])==console[i];
        if(same)return true;
    }
    if(base==3) {
        wchar_t a=monado_module_upper(name[0]),b=monado_module_upper(name[1]),c=monado_module_upper(name[2]);
        return (a==L'C'&&b==L'O'&&c==L'N')||(a==L'P'&&b==L'R'&&c==L'N')||
               (a==L'A'&&b==L'U'&&c==L'X')||(a==L'N'&&b==L'U'&&c==L'L');
    }
    if(base==4) {
        wchar_t a=monado_module_upper(name[0]),b=monado_module_upper(name[1]),c=monado_module_upper(name[2]),d=name[3];
        bool number=(d>=L'1'&&d<=L'9')||d==0xb9||d==0xb2||d==0xb3;
        return number&&((a==L'C'&&b==L'O'&&c==L'M')||(a==L'L'&&b==L'P'&&c==L'T'));
    }
    return false;
}
static bool monado_windows_module_path(wchar_t *output,size_t capacity,const wchar_t *input) {
    if(!output||!capacity||!input)return false;
    size_t length=0;while(length<32768&&input[length])length++;
    if(!length||length==32768)return false;
    const wchar_t *source=input;bool extended=false,unc=false;
    if(length>=4&&!wcsncmp(input,L"\\\\?\\",4)) {
        extended=true;source=input+4;
        if(length>=8&&monado_module_upper(source[0])==L'U'&&monado_module_upper(source[1])==L'N'&&
           monado_module_upper(source[2])==L'C'&&source[3]==L'\\') {unc=true;source+=4;}
        else if(!((source[0]>=L'A'&&source[0]<=L'Z')||(source[0]>=L'a'&&source[0]<=L'z'))||source[1]!=L':'||source[2]!=L'\\')return false;
    } else if(length>=4&&!wcsncmp(input,L"\\\\.\\",4))return false;
    if(!extended) {
        unc=length>=2&&input[0]==L'\\'&&input[1]==L'\\';
        if(unc)source+=2;
        else if(length<3||!((input[0]>=L'A'&&input[0]<=L'Z')||(input[0]>=L'a'&&input[0]<=L'z'))||input[1]!=L':'||(input[2]!=L'\\'&&input[2]!=L'/'))return false;
    }
    size_t tail=wcslen(source),size=tail+(unc?2:0);
    if(size>=capacity)return false;
    // In-place normalization is safe because removal only shifts towards front.
    memmove(output+(unc?2:0),source,(tail+1)*sizeof(wchar_t));
    if(unc)output[0]=output[1]=L'\\';
    for(size_t i=0;i<size;i++) {
        wchar_t ch=output[i];
        if(ch==L'/'){if(extended)return false;output[i]=L'\\';}
        else if(ch<32||ch==L'<'||ch==L'>'||ch==L'"'||ch==L'|'||ch==L'?'||ch==L'*'||(ch==L':'&&(unc||i!=1)))return false;
    }
    size_t begin=unc?2:3;unsigned components=0;
    for(size_t end=begin;end<=size;end++)if(end==size||output[end]==L'\\') {
        size_t count=end-begin;
        if(!count||output[end-1]==L'.'||output[end-1]==L' '||
           (count==1&&output[begin]==L'.')||(count==2&&output[begin]==L'.'&&output[begin+1]==L'.')||
           ((!unc||components>=2)&&monado_module_reserved(output+begin,count)))return false;
        components++;begin=end+1;
    }
    return !unc||components>=3; // A module names a leaf below the UNC share.
}

// Only use after the ordinary canonical pathname has passed the held native
// ancestor/leaf checks. LoadLibraryExW still needs extended spelling for a long
// module path even when ordinary CreateFileW is longPathAware. This changes
// spelling only; it performs no lookup, resolution, search or guard bypass.
static bool monado_windows_module_mapping_path(wchar_t *output,size_t capacity,const wchar_t *ordinary) {
    if(!monado_windows_module_path(output,capacity,ordinary))return false;
    size_t length=wcslen(output);bool unc=output[0]==L'\\'&&output[1]==L'\\';size_t added=unc?6:4;
    if(length+added>=capacity)return false;
    if(unc) {
        memmove(output+8,output+2,(length-2+1)*sizeof(wchar_t));
        memcpy(output,L"\\\\?\\UNC\\",8*sizeof(wchar_t));
    } else {
        memmove(output+4,output,(length+1)*sizeof(wchar_t));
        memcpy(output,L"\\\\?\\",4*sizeof(wchar_t));
    }
    return true;
}
