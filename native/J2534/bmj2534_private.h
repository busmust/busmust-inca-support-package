// Private helpers shared by the standard provider and its optional port entries.
#pragma once
#include <windows.h>
#include <cstring>
#include <string>

namespace bmj {
namespace detail {
inline const char* callerName(void* address, char (&path)[MAX_PATH]) {
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),&module)) return nullptr;
    const DWORD length=GetModuleFileNameA(module,path,_countof(path));
    if(!length || length>=_countof(path)) return nullptr;
    const char* name=strrchr(path,'\\');
    return name ? name+1 : path;
}
inline bool compatiblePacking(const char* name) {
#ifdef _WIN64
    return !name || _stricmp(name,"J2534OCD_DefaultPacking.dll")!=0;
#else
    (void)name;
    return true;
#endif
}
}
}
