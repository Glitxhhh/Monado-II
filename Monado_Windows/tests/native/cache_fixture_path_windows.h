#pragma once
// Native API receives an explicit extended path; the CRT _wchdir has a 260-character limit.
static bool cache_fixture_cwd(const wchar_t *path) {
    wchar_t native[WS_PATH];
    if(wcslen(path)+8>=WS_PATH)return false;
    if(!wcsncmp(path,L"\\\\",2))swprintf(native,WS_PATH,L"\\\\?\\UNC\\%ls",path+2);
    else swprintf(native,WS_PATH,L"\\\\?\\%ls",path);
    bool ok=SetCurrentDirectoryW(native)!=0;
    if(!ok)fprintf(stderr,"native fixture SetCurrentDirectoryW failed error=%lu input_length=%zu extended_length=%zu\n",(unsigned long)GetLastError(),wcslen(path),wcslen(native));
    return ok;
}
