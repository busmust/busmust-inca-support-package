// Generic installed entry; all transport state stays in the shared standard core.
#include "bmj2534.h"
#include "bmj2534_private.h"
#include <intrin.h>
#include <cstdio>
#include <new>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {
using U=unsigned long;
thread_local char lastError[80]="No error";
long fail(long code,const char* message) {
    strncpy_s(lastError,message,_TRUNCATE);
    return code;
}
#ifdef _WIN64
// Verified INCA x64 outer-list ABI; element and message layouts stay standard.
#pragma pack(push, 8)
struct IncaList { U count; void* data; };
#pragma pack(pop)
static_assert(sizeof(void*)==8 && sizeof(IncaList)==16 && offsetof(IncaList,data)==8,"INCA x64 ABI");
static_assert(sizeof(SCONFIG_LIST)==12 && offsetof(SCONFIG_LIST,ConfigPtr)==4,"Standard ABI retained");
static_assert(sizeof(SPARAM_LIST)==12 && offsetof(SPARAM_LIST,ParamPtr)==4,"Standard capabilities ABI");
static_assert(sizeof(PASSTHRU_MSG)==4152 && sizeof(SCONFIG)==8 && sizeof(SPARAM)==12,"Array elements unchanged");
// Only the optional INCA list adapter owns these extra memory checks.
// The caller still owns the lifetime of buffers throughout the call.
bool readIncaList(const void* input,IncaList* result) {
    __try { memcpy(result,input,sizeof(*result)); return true; }
    __except((GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR)
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool accessibleArray(const void* pointer,size_t bytes,bool write) {
    if(!bytes) return true;
    const uintptr_t start=reinterpret_cast<uintptr_t>(pointer);
    if(!start || bytes>std::numeric_limits<uintptr_t>::max()-start) return false;
    const uintptr_t finish=start+bytes;
    uintptr_t cursor=start;
    while(cursor<finish) {
        MEMORY_BASIC_INFORMATION info={};
        if(VirtualQuery(reinterpret_cast<const void*>(cursor),&info,sizeof(info))!=sizeof(info) ||
            info.State!=MEM_COMMIT || (info.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
        const DWORD protection=info.Protect&0xFF;
        const bool writable=protection==PAGE_READWRITE || protection==PAGE_WRITECOPY ||
            protection==PAGE_EXECUTE_READWRITE || protection==PAGE_EXECUTE_WRITECOPY;
        const bool readable=writable || protection==PAGE_READONLY || protection==PAGE_EXECUTE_READ;
        if(!readable || (write && !writable)) return false;
        const uintptr_t region=reinterpret_cast<uintptr_t>(info.BaseAddress);
        if(info.RegionSize>std::numeric_limits<uintptr_t>::max()-region) return false;
        const uintptr_t end=region+info.RegionSize;
        if(end<=cursor) return false;
        cursor=end<finish ? end : finish;
    }
    return true;
}
bool incaDefaultCaller(void* address) {
    char path[MAX_PATH]={};
    const char* name=bmj::detail::callerName(address,path);
    return name && _stricmp(name,"J2534OCD_DefaultPacking.dll")==0;
}
#endif
const char* const exports[]={
    "PassThruOpen","PassThruClose","PassThruConnect","PassThruDisconnect",
    "PassThruReadMsgs","PassThruWriteMsgs","PassThruStartPeriodicMsg","PassThruStopPeriodicMsg",
    "PassThruStartMsgFilter","PassThruStopMsgFilter","PassThruSetProgrammingVoltage",
    "PassThruReadVersion","PassThruGetLastError","PassThruIoctl"
};
struct Core {
    HMODULE module=nullptr;
    FARPROC functions[_countof(exports)]={};
    Core() {
        HMODULE entry=nullptr; wchar_t path[32768]={};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&PassThruOpen),&entry)) {
            fail(ERR_FAILED,"Cannot locate the endpoint entry module"); return;
        }
        const DWORD length=GetModuleFileNameW(entry,path,_countof(path));
        if(!length || length>=_countof(path)) { fail(ERR_FAILED,"Cannot locate the endpoint entry directory"); return; }
        std::wstring filename(path,length);
        filename.resize(filename.find_last_of(L"\\/")+1);
#ifdef _WIN64
        filename+=L"BMJ2534.dll";
#else
        filename+=L"BMJ25332.dll";
#endif
        module=LoadLibraryExW(filename.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!module) { fail(ERR_FAILED,"Cannot load the matching J2534 core beside this endpoint entry"); return; }
        for(size_t i=0;i<_countof(exports);++i) {
            functions[i]=GetProcAddress(module,exports[i]);
            if(!functions[i]) {
                fail(ERR_FAILED,"The J2534 core is missing a standard export");
                FreeLibrary(module); module=nullptr; return;
            }
        }
    }
    ~Core() { if(module) FreeLibrary(module); }
    Core(const Core&)=delete;
    Core& operator=(const Core&)=delete;
};
template<typename Function,typename... Args>
long forward(size_t index,Args... args) {
    try {
        Core core;
        if(!core.module) return ERR_FAILED;
        const long result=reinterpret_cast<Function>(core.functions[index])(args...);
        if(result!=STATUS_NOERROR) {
            char message[80]={};
            if(reinterpret_cast<decltype(&PassThruGetLastError)>(core.functions[12])(message)==STATUS_NOERROR)
                strncpy_s(lastError,message,_TRUNCATE);
            else fail(result,"The J2534 core could not return its error detail");
        }
        return result;
    } catch(const std::bad_alloc&) { return fail(ERR_EXCEEDED_LIMIT,"Memory limit exceeded"); }
    catch(...) { return fail(ERR_FAILED,"Endpoint entry forwarding failed"); }
}
}

long WINAPI PassThruOpen(void* name,U* output) {
    try {
        if(!output) return fail(ERR_NULL_PARAMETER,"Missing device ID output");
        *output=0;
        // Selection belongs to the shared core/current-user configuration.
        // Preserve explicit client selectors; never derive identity from a DLL name.
        return forward<decltype(&PassThruOpen)>(0,name,output);
    } catch(const std::bad_alloc&) { return fail(ERR_EXCEEDED_LIMIT,"Memory limit exceeded"); }
    catch(...) { return fail(ERR_FAILED,"Endpoint entry open failed"); }
}
long WINAPI PassThruClose(U id) { return forward<decltype(&PassThruClose)>(1,id); }
long WINAPI PassThruConnect(U id,U protocol,U flags,U rate,U* output) { return forward<decltype(&PassThruConnect)>(2,id,protocol,flags,rate,output); }
long WINAPI PassThruDisconnect(U id) { return forward<decltype(&PassThruDisconnect)>(3,id); }
long WINAPI PassThruReadMsgs(U id,PASSTHRU_MSG* messages,U* count,U timeout) { return forward<decltype(&PassThruReadMsgs)>(4,id,messages,count,timeout); }
long WINAPI PassThruWriteMsgs(U id,PASSTHRU_MSG* messages,U* count,U timeout) { return forward<decltype(&PassThruWriteMsgs)>(5,id,messages,count,timeout); }
long WINAPI PassThruStartPeriodicMsg(U id,PASSTHRU_MSG* message,U* output,U interval) { return forward<decltype(&PassThruStartPeriodicMsg)>(6,id,message,output,interval); }
long WINAPI PassThruStopPeriodicMsg(U id,U message) { return forward<decltype(&PassThruStopPeriodicMsg)>(7,id,message); }
long WINAPI PassThruStartMsgFilter(U id,U type,PASSTHRU_MSG* mask,PASSTHRU_MSG* pattern,PASSTHRU_MSG* flow,U* output) { return forward<decltype(&PassThruStartMsgFilter)>(8,id,type,mask,pattern,flow,output); }
long WINAPI PassThruStopMsgFilter(U id,U filter) { return forward<decltype(&PassThruStopMsgFilter)>(9,id,filter); }
long WINAPI PassThruSetProgrammingVoltage(U id,U pin,U voltage) { return forward<decltype(&PassThruSetProgrammingVoltage)>(10,id,pin,voltage); }
long WINAPI PassThruReadVersion(U id,char* firmware,char* dll,char* api) { return forward<decltype(&PassThruReadVersion)>(11,id,firmware,dll,api); }
long WINAPI PassThruGetLastError(char* output) {
    if(!output) return ERR_NULL_PARAMETER;
    strcpy_s(output,80,lastError);
    return STATUS_NOERROR;
}
long WINAPI PassThruIoctl(U id,U ioctl,void* input,void* output) {
#ifdef _WIN64
    const bool defaultPacking=incaDefaultCaller(_ReturnAddress());
    if(defaultPacking && input && (ioctl==GET_CONFIG || ioctl==SET_CONFIG)) {
        IncaList source={};
        if(!readIncaList(input,&source)) return fail(ERR_NULL_PARAMETER,"Unreadable INCA list header");
        if(source.count<=64 && !accessibleArray(source.data,static_cast<size_t>(source.count)*sizeof(SCONFIG),ioctl==GET_CONFIG))
            return fail(ERR_NULL_PARAMETER,"Inaccessible INCA list array");
        SCONFIG_LIST converted={source.count,static_cast<SCONFIG*>(source.data)};
        return forward<decltype(&PassThruIoctl)>(13,id,ioctl,&converted,output);
    }
    if(defaultPacking && input && (ioctl==GET_DEVICE_INFO || ioctl==GET_PROTOCOL_INFO)) {
        IncaList source={};
        if(!readIncaList(input,&source)) return fail(ERR_NULL_PARAMETER,"Unreadable INCA list header");
        if(source.count<=128 && !accessibleArray(source.data,static_cast<size_t>(source.count)*sizeof(SPARAM),true))
            return fail(ERR_NULL_PARAMETER,"Inaccessible INCA list array");
        SPARAM_LIST converted={source.count,static_cast<SPARAM*>(source.data)};
        return forward<decltype(&PassThruIoctl)>(13,id,ioctl,&converted,output);
    }
#endif
    // Standard callers/Win32 and unsupported IOCTLs retain core semantics.
    return forward<decltype(&PassThruIoctl)>(13,id,ioctl,input,output);
}
