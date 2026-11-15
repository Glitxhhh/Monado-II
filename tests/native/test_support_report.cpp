#include <cstdio>
#include "../../Monado_Linux/tools/launcher/linux_report.h"
int main(int argc,char **argv) {
    if(argc!=2)return 2;std::string path,error;
    if(!monado_report::create(argv[1],path,error)){fprintf(stderr,"%s\n",error.c_str());return 1;}
    printf("%s\n",path.c_str());return 0;
}
