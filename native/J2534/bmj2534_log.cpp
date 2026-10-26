// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 BUSMUST Tech Co.,Ltd.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include "bmj2534_log.h"
#include "bmj2534_version.h"
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace bmj { namespace diagnostic {
namespace {
const unsigned Capacity=128, PerSecond=128;
const ULONGLONG MaxFileBytes=4ULL*1024*1024;
thread_local const char* apiName=nullptr;
thread_local unsigned long apiTarget=0;
std::atomic<unsigned> writers{0};
std::atomic<ULONGLONG> skippedAttempts{0};
struct Record {
    ULONGLONG wall=0;
    LONGLONG tick=0;
    DWORD thread=0,target=0,a=0,b=0,c=0;
    bool truncated=false;
    char api[40]={},stage[40]={},detail[256]={};
};
struct Context {
    CRITICAL_SECTION mutex;
    bool initialized=false,started=false,counted=false;
    HANDLE wake=nullptr;
    HMODULE module=nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<bool> accepting{true};
    std::atomic<ULONGLONG> dropped{0};
    Record records[Capacity];
    unsigned head=0,count=0,budget=0;
    DWORD budgetTick=0;
    LONGLONG frequency=0;
    Context() {
        initialized=InitializeCriticalSectionAndSpinCount(&mutex,0)!=FALSE;
        wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        LARGE_INTEGER f={};QueryPerformanceFrequency(&f);frequency=f.QuadPart;
    }
    ~Context() {
        if(wake)CloseHandle(wake);
        if(initialized)DeleteCriticalSection(&mutex);
        if(module)FreeLibrary(module);
        if(counted)--writers;
    }
};
// Preserve valid UTF-8 (including paths); escape malformed native bytes so a
// diagnostic string cannot corrupt JSON or manufacture another record.
std::string quote(const char* s) {
    std::string out="\"";
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(s?s:"");*p;++p) {
        if(*p=='"' || *p=='\\') {out+='\\';out+=static_cast<char>(*p);}
        else if(*p>=128) {
            unsigned length=*p>=0xC2 && *p<=0xDF ? 2 : (*p>=0xE0 && *p<=0xEF ? 3 : (*p>=0xF0 && *p<=0xF4 ? 4 : 0));
            bool valid=length!=0;
            for(unsigned i=1;valid && i<length;++i)valid=p[i]>=0x80 && p[i]<=0xBF;
            if(valid && length==3)valid=!(*p==0xE0 && p[1]<0xA0) && !(*p==0xED && p[1]>=0xA0);
            if(valid && length==4)valid=!(*p==0xF0 && p[1]<0x90) && !(*p==0xF4 && p[1]>=0x90);
            if(valid){out.append(reinterpret_cast<const char*>(p),length);p+=length-1;}
            else {char escaped[7]={};sprintf_s(escaped,"\\u%04X",*p);out+=escaped;}
        }
        else if(*p<32 || *p==127) {char escaped[7]={};sprintf_s(escaped,"\\u%04X",*p);out+=escaped;}
        else out+=static_cast<char>(*p);
    }
    return out+'"';
}
std::string utf8(const std::wstring& s) {
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    if(n<=0)return std::string();
    std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),&out[0],n,nullptr,nullptr);return out;
}
std::string hashFile(const std::wstring& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return "unavailable";
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;
    bool ok=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)!=FALSE;
    if(ok)ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=FALSE;
    BYTE data[16384];DWORD read=0;
    while(ok) {
        if(!ReadFile(file,data,sizeof(data),&read,nullptr)){ok=false;break;}
        if(!read)break;
        if(!CryptHashData(hash,data,read,0))ok=false;
    }
    BYTE digest[32]={};DWORD size=sizeof(digest);
    if(ok)ok=CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0)!=FALSE && size==sizeof(digest);
    if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);CloseHandle(file);
    if(!ok)return "unavailable";
    char result[65]={};for(unsigned i=0;i<32;++i)sprintf_s(result+i*2,3,"%02x",digest[i]);return result;
}
bool ownedName(const std::wstring& name) {
    if(name.size()!=39 || name.compare(0,8,L"BMJ2534-") || name[24]!=L'-' || name.compare(33,6,L".jsonl"))return false;
    for(size_t i=8;i<33;++i)if(i!=24 && !((name[i]>=L'0' && name[i]<=L'9') || (name[i]>=L'A' && name[i]<=L'F')))return false;
    return true;
}
void retainClosedSessions(const std::wstring& directory) {
    WIN32_FIND_DATAW info={};HANDLE search=FindFirstFileW((directory+L"\\BMJ2534-*.jsonl").c_str(),&info);
    if(search==INVALID_HANDLE_VALUE)return;
    std::vector<std::wstring> names;
    do {if(!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) && ownedName(info.cFileName))names.push_back(info.cFileName);} while(FindNextFileW(search,&info));
    FindClose(search);std::sort(names.begin(),names.end(),std::greater<std::wstring>());
    for(size_t i=8;i<names.size();++i) {
        const auto base=directory+L"\\"+names[i];
        // Active writers deny delete sharing. Never remove their rotations.
        if(DeleteFileW(base.c_str())) {DeleteFileW((base+L".1").c_str());DeleteFileW((base+L".2").c_str());}
    }
}
struct File {
    HANDLE handle=INVALID_HANDLE_VALUE;
    std::wstring path;
    std::string headerLine;
    ULONGLONG bytes=0;
    bool enabled=false;
    ~File(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    bool write(const std::string& line) {
        if(!enabled)return false;
        if(bytes+line.size()>MaxFileBytes) {
            CloseHandle(handle);handle=INVALID_HANDLE_VALUE;
            DeleteFileW((path+L".2").c_str());
            MoveFileExW((path+L".1").c_str(),(path+L".2").c_str(),MOVEFILE_REPLACE_EXISTING);
            if(!MoveFileExW(path.c_str(),(path+L".1").c_str(),MOVEFILE_REPLACE_EXISTING)){enabled=false;return false;}
            handle=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle==INVALID_HANDLE_VALUE){enabled=false;return false;}
            bytes=0;
            if(!write(headerLine))return false;
        }
        DWORD n=0;
        if(!WriteFile(handle,line.data(),static_cast<DWORD>(line.size()),&n,nullptr) || n!=line.size()){enabled=false;return false;}
        bytes+=n;return true;
    }
    void open(Context* context) {
        wchar_t local[32768]={};const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,_countof(local));
        if(!n || n>=_countof(local) || !((n>=3 && local[1]==L':' && (local[2]==L'\\' || local[2]==L'/')) || (local[0]==L'\\' && local[1]==L'\\')))return;
        std::wstring directory(local,n);
        for(const auto* part:{L"\\BUSMUST",L"\\J2534",L"\\Logs"}) {
            directory+=part;if(!CreateDirectoryW(directory.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)return;
        }
        FILETIME stamp={};GetSystemTimeAsFileTime(&stamp);ULARGE_INTEGER value={};value.LowPart=stamp.dwLowDateTime;value.HighPart=stamp.dwHighDateTime;
        wchar_t name[80]={};swprintf_s(name,L"\\BMJ2534-%016llX-%08lX.jsonl",value.QuadPart,GetCurrentProcessId());path=directory+name;
        handle=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return;
        enabled=true;
        wchar_t module[32768]={},host[32768]={};
        const DWORD m=GetModuleFileNameW(context->module,module,_countof(module));
        const DWORD h=GetModuleFileNameW(nullptr,host,_countof(host));
        std::wstring modulePath(module,m<_countof(module)?m:0),hostPath(host,h<_countof(host)?h:0);
        const auto folder=modulePath.substr(0,modulePath.find_last_of(L"\\/")+1);
        char fields[256]={};sprintf_s(fields,"{\"schema\":1,\"event\":\"SESSION_BEGIN\",\"pid\":%lu,\"pointer_bits\":%u,\"qpc_frequency\":%lld,\"filetime\":%llu,\"version\":\"%s\",",GetCurrentProcessId(),static_cast<unsigned>(sizeof(void*)*8),context->frequency,value.QuadPart,BMJ_FILEVERSION_STRING);
        headerLine=std::string(fields)+"\"bmj2534_version\":"+quote(BMJ_FILEVERSION_STRING)+",\"module\":"+quote(utf8(modulePath).c_str())+",\"module_sha256\":"+quote(hashFile(modulePath).c_str())+",\"bmapi_sha256\":"+quote(hashFile(folder+L"BMAPI.dll").c_str())+",\"host\":"+quote(utf8(hostPath).c_str())+"}\n";
        write(headerLine);retainClosedSessions(directory);
    }
};
DWORD WINAPI writer(void* raw) {
    auto* context=static_cast<Context*>(raw);
    try {
        File file;file.open(context);context->accepting=file.enabled;
        DWORD lastFlush=GetTickCount(),lastSummary=lastFlush;
        for(;;) {
            WaitForSingleObject(context->wake,100);
            Record batch[32];unsigned count=0;
            EnterCriticalSection(&context->mutex);
            while(count<_countof(batch) && context->count) {batch[count++]=context->records[context->head];context->head=(context->head+1)%Capacity;--context->count;}
            const bool empty=context->count==0;
            // Read stop and queue state together. A producer may enqueue its
            // final close result after this batch was taken, then release the
            // last session owner; an earlier empty snapshot cannot prove drain.
            const bool finished=empty && context->stopping.load();
            LeaveCriticalSection(&context->mutex);
            for(unsigned i=0;i<count;++i) {
                const auto& item=batch[i];char fields[320]={};
                sprintf_s(fields,"{\"filetime\":%llu,\"qpc\":%lld,\"tid\":%lu,\"target\":%lu,\"a\":%lu,\"b\":%lu,\"c\":%lu,",item.wall,item.tick,item.thread,item.target,item.a,item.b,item.c);
                std::string versions;
                if(strcmp(item.stage,"BMAPI_RUNTIME_VERSION")==0) {
                    char runtime[80]={};sprintf_s(runtime,"%u.%u.%u.%u",item.a>>24,(item.a>>16)&255,(item.a>>8)&255,item.a&255);
                    versions=",\"bmapi_runtime_version\":"+quote(runtime);
                }
                file.write(std::string(fields)+"\"api\":"+quote(item.api)+",\"event\":"+quote(item.stage)+",\"detail\":"+quote(item.detail)+",\"detail_truncated\":"+(item.truncated?"true":"false")+versions+"}\n");
            }
            const DWORD now=GetTickCount();
            if(static_cast<DWORD>(now-lastSummary)>=1000 || context->stopping) {
                const auto dropped=context->dropped.exchange(0);
                if(dropped) {char line[160]={};sprintf_s(line,"{\"event\":\"LOG_RECORDS_DROPPED\",\"count\":%llu}\n",dropped);file.write(line);}
                const auto skipped=skippedAttempts.exchange(0);
                if(skipped) {char line[160]={};sprintf_s(line,"{\"event\":\"LOG_OPEN_ATTEMPTS_SKIPPED\",\"count\":%llu}\n",skipped);file.write(line);}
                lastSummary=now;
            }
            if(finished){file.write("{\"event\":\"SESSION_END\"}\n");if(file.enabled)FlushFileBuffers(file.handle);break;}
            if(file.enabled && static_cast<DWORD>(now-lastFlush)>=2000){if(!FlushFileBuffers(file.handle))file.enabled=false;lastFlush=now;}
            if(!file.enabled)context->accepting=false;
            if(!empty)SetEvent(context->wake);
        }
    } catch(...) { // Diagnostic failures never cross a business/API boundary.
        while(!context->stopping)WaitForSingleObject(context->wake,100);
    }
    HMODULE module=context->module;context->module=nullptr;delete context;
    FreeLibraryAndExitThread(module,0);
}
}
struct Sink {
    Context* context;
    explicit Sink(Context* p):context(p){}
    ~Sink() {
        if(context->started){context->stopping=true;SetEvent(context->wake);}
        else delete context;
    }
    void emit(const char* stage,unsigned long a,unsigned long b,unsigned long c,const char* detail) noexcept {
        if(!context->accepting)return;
        const DWORD previousError=GetLastError();
        if(TryEnterCriticalSection(&context->mutex)) {
            const DWORD now=GetTickCount();
            if(static_cast<DWORD>(now-context->budgetTick)>=1000){context->budgetTick=now;context->budget=0;}
            if(context->count<Capacity && context->budget<PerSecond) {
                auto& item=context->records[(context->head+context->count)%Capacity];
                FILETIME wall={};GetSystemTimeAsFileTime(&wall);ULARGE_INTEGER timestamp={};timestamp.LowPart=wall.dwLowDateTime;timestamp.HighPart=wall.dwHighDateTime;
                LARGE_INTEGER tick={};QueryPerformanceCounter(&tick);
                item.wall=timestamp.QuadPart;item.tick=tick.QuadPart;item.thread=GetCurrentThreadId();item.target=apiTarget;item.a=a;item.b=b;item.c=c;
                strncpy_s(item.api,apiName?apiName:"unscoped",_TRUNCATE);strncpy_s(item.stage,stage?stage:"",_TRUNCATE);strncpy_s(item.detail,detail?detail:"",_TRUNCATE);
                item.truncated=detail && strnlen_s(detail,sizeof(item.detail))==sizeof(item.detail);
                ++context->count;++context->budget;LeaveCriticalSection(&context->mutex);SetEvent(context->wake);
            } else {++context->dropped;LeaveCriticalSection(&context->mutex);}
        } else ++context->dropped;
        SetLastError(previousError);
    }
};
namespace {
struct Manager {std::mutex mutex;std::weak_ptr<Sink> session;};
Manager& manager(){static Manager value;return value;}
Session current() noexcept {
    try {auto& value=manager();std::lock_guard<std::mutex> guard(value.mutex);return value.session.lock();}catch(...){return Session();}
}
}
Session start() noexcept {
    const DWORD previousError=GetLastError();Session result;
    try {
        auto& value=manager();std::lock_guard<std::mutex> guard(value.mutex);result=value.session.lock();
        if(!result) {
            HKEY key=nullptr;DWORD enabled=1,bytes=sizeof(enabled),type=0;
            if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\BUSMUST\\J2534",0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key)==ERROR_SUCCESS) {
                if(RegQueryValueExW(key,L"FileLogEnabled",nullptr,&type,reinterpret_cast<BYTE*>(&enabled),&bytes)!=ERROR_SUCCESS || type!=REG_DWORD || bytes!=sizeof(enabled))enabled=1;
                RegCloseKey(key);
            }
            if(enabled && writers.load()<2) {
                std::unique_ptr<Context> context(new Context);
                if(context->initialized && context->wake && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&writer),&context->module)) {
                    result=std::make_shared<Sink>(context.get());context.release();
                    ++writers;result->context->counted=true;result->context->started=true;
                    HANDLE thread=CreateThread(nullptr,0,writer,result->context,0,nullptr);
                    if(thread){CloseHandle(thread);value.session=result;}
                    else {result->context->started=false;result.reset();}
                }
            } else if(enabled)++skippedAttempts;
        }
    }catch(...){result.reset();}
    SetLastError(previousError);return result;
}
void event(const Session& session,const char* stage,unsigned long a,unsigned long b,unsigned long c,const char* detail) noexcept {
    if(session)session->emit(stage,a,b,c,detail);
}
void event(const char* stage,unsigned long a,unsigned long b,unsigned long c,const char* detail) noexcept {
    const DWORD previousError=GetLastError();event(current(),stage,a,b,c,detail);SetLastError(previousError);
}
// Read/Write create API context only for a genuine error result. Their normal
// high-frequency paths do not touch diagnostic TLS or emit polling records.
void error(long status,const char* detail) noexcept {if(apiName)event("ERROR",static_cast<unsigned long>(status),0,0,detail);}
ApiScope::ApiScope(const char* name,unsigned long target) noexcept:previous_(apiName),previousTarget_(apiTarget){apiName=name;apiTarget=target;}
ApiScope::~ApiScope() noexcept {apiName=previous_;apiTarget=previousTarget_;}
}}
