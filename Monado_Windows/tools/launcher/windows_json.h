// [XCDE] Bounded authored JSON reader for native preparation context/progress.
#pragma once
#include "../builder/payload_windows.h"
#include <map>
namespace monado_preparation {
struct JsonValue {
    char kind='n';std::string value;std::map<std::string,JsonValue> fields;std::vector<JsonValue> items;
    const JsonValue& at(const char *key)const{auto i=fields.find(key);monado_builder::require(kind=='o'&&i!=fields.end(),"Preparation record is missing a field");return i->second;}
    std::string text(const char *key)const{auto &x=at(key);monado_builder::require(x.kind=='s',"Preparation field is not text");return x.value;}
    std::string optional(const char *key)const{auto i=fields.find(key);return i!=fields.end()&&i->second.kind=='s'?i->second.value:"";}
    uint64_t integer()const{monado_builder::require(kind=='d'&&!value.empty(),"Preparation field is not a count");uint64_t n=0;for(char c:value){monado_builder::require(c>='0'&&c<='9'&&n<=(UINT64_MAX-(c-'0'))/10,"Preparation count is invalid");n=n*10+c-'0';}return n;}
};
class JsonReader {
    const std::string &s;size_t p=0,nodes=0;
    void space(){while(p<s.size()&&(s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r'))++p;}
    std::string string(){
        using namespace monado_builder;require(p<s.size()&&s[p++]=='"',"Expected preparation JSON text");std::string r;
        while(p<s.size()){
            unsigned char c=s[p++];if(c=='"'){wide(r);return r;}require(c>=32,"Control byte in preparation JSON");
            if(c!='\\'){r+=(char)c;require(r.size()<=65536,"Preparation JSON text exceeds its bound");continue;}require(p<s.size(),"Truncated preparation JSON escape");char e=s[p++];
            if(e=='"'||e=='\\'||e=='/')r+=e;else if(e=='n')r+='\n';else if(e=='r')r+='\r';else if(e=='t')r+='\t';else if(e=='b')r+='\b';else if(e=='f')r+='\f';
            else if(e=='u'){
                auto word=[&](){require(p+4<=s.size(),"Truncated preparation Unicode");unsigned n=0;for(int i=0;i<4;++i){char x=s[p++];n<<=4;if(x>='0'&&x<='9')n+=x-'0';else if(x>='a'&&x<='f')n+=x-'a'+10;else if(x>='A'&&x<='F')n+=x-'A'+10;else throw std::runtime_error("Invalid preparation Unicode");}return n;};
                unsigned a=word();std::wstring w(1,(wchar_t)a);if(a>=0xd800&&a<=0xdbff){require(p+2<=s.size()&&s[p]=='\\'&&s[p+1]=='u',"Missing preparation surrogate");p+=2;unsigned b=word();require(b>=0xdc00&&b<=0xdfff,"Invalid preparation surrogate");w+=(wchar_t)b;}else require(a<0xdc00||a>0xdfff,"Invalid preparation surrogate");r+=utf8(w);
            }else throw std::runtime_error("Invalid preparation JSON escape");
            require(r.size()<=65536,"Preparation JSON text exceeds its bound");
        }throw std::runtime_error("Truncated preparation JSON text");
    }
    JsonValue item(unsigned depth){
        using namespace monado_builder;require(depth<=32&&++nodes<=200000,"Preparation JSON exceeds its bound");space();require(p<s.size(),"Truncated preparation JSON");JsonValue x;char c=s[p];
        if(c=='"'){x.kind='s';x.value=string();return x;}
        if(c=='{'||c=='['){++p;x.kind=c=='{'?'o':'a';char close=c=='{'?'}':']';space();if(p<s.size()&&s[p]==close){++p;return x;}
            for(;;){space();if(c=='{'){auto key=string();space();require(p<s.size()&&s[p++]==':',"Invalid preparation field");require(x.fields.emplace(key,item(depth+1)).second,"Duplicate preparation JSON field");}else x.items.push_back(item(depth+1));space();require(p<s.size(),"Truncated preparation container");char end=s[p++];if(end==close)return x;require(end==',',"Invalid preparation separator");}}
        if(c>='0'&&c<='9'){x.kind='d';size_t start=p++;while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;require(p-start==1||s[start]!='0',"Invalid preparation count");x.value=s.substr(start,p-start);return x;}
        for(const char *literal:{"true","false","null"}){size_t n=strlen(literal);if(s.compare(p,n,literal)==0){p+=n;x.kind=literal[0]=='n'?'n':'b';x.value=literal;return x;}}
        throw std::runtime_error("Unsupported preparation JSON value");
    }
public:
    explicit JsonReader(const std::string &text):s(text){}
    JsonValue read(){monado_builder::require(s.size()<=16*1024*1024,"Preparation JSON exceeds its bound");auto result=item(0);space();monado_builder::require(p==s.size(),"Trailing preparation JSON");return result;}
};
}
