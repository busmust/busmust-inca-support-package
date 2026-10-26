#include "bmj2534_backend.h"
#include "bmj2534_private.h"
#include <cstdio>
#include <climits>
#include <new>
#include <memory>
#include <sddl.h>
namespace bmj {
std::wstring moduleDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleDirectory), &module)) return {};
    wchar_t path[32768] = {};
    const DWORD length = GetModuleFileNameW(module, path, _countof(path));
    if (!length || length >= _countof(path)) return {};
    std::wstring result(path, length);
    return result.substr(0, result.find_last_of(L"\\/") + 1);
}
std::string identity(const BM_ChannelInfoTypeDef& info) {
    char name[64] = {};
    char* cursor = name;
    for (unsigned i = 0; i < sizeof(info.uid); ++i) cursor += sprintf_s(cursor, 64 - (cursor - name), "%02X", info.uid[i]);
    sprintf_s(cursor, 64 - (cursor - name), ":%u", info.port);
    return name;
}
namespace {
const wchar_t* const selectionKey=L"Software\\BUSMUST\\J2534";
bool validIdentity(const std::string& name) {
    if(name.size()<26 || name.size()>30 || name[24]!=':' ||
       name.substr(0,24).find_first_not_of("0123456789ABCDEF")!=std::string::npos) return false;
    if(name.size()>26 && name[25]=='0') return false;
    unsigned port=0;
    for(size_t i=25;i<name.size();++i) {
        if(name[i]<'0' || name[i]>'9') return false;
        port=port*10+name[i]-'0'; if(port>65535) return false;
    }
    return true;
}
bool samplePercent(const std::string& text,unsigned& value) {
    if(text.empty() || text.size()>3 || text[0]=='0') return false;
    value=0;
    for(const auto ch:text) {
        if(ch<'0' || ch>'9') return false;
        value=value*10+ch-'0';
    }
    return value>=1 && value<=100;
}
struct SelectionLock {
    HANDLE mutex=nullptr;
    bool acquired=false;
    SelectionLock() {
        HANDLE token=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return;
        std::unique_ptr<void,decltype(&CloseHandle)> tokenOwner(token,&CloseHandle);
        DWORD bytes=0; GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
        std::vector<BYTE> buffer(bytes);
        const bool ok=bytes && GetTokenInformation(token,TokenUser,buffer.data(),bytes,&bytes)!=FALSE;
        if(!ok) return;
        LPWSTR sid=nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&sid)) return;
        std::unique_ptr<wchar_t,decltype(&LocalFree)> sidOwner(sid,&LocalFree);
        std::wstring name=L"Global\\BUSMUST.BMJ2534.Selection.";name+=sid;
        mutex=CreateMutexW(nullptr,FALSE,name.c_str());
        if(mutex) {
            const DWORD result=WaitForSingleObject(mutex,5000);
            acquired=result==WAIT_OBJECT_0 || result==WAIT_ABANDONED;
        }
    }
    ~SelectionLock(){if(acquired)ReleaseMutex(mutex);if(mutex)CloseHandle(mutex);}
};
}
static bool legacyTermination(const std::string& identity,bool& enabled) {
    enabled=false;
    if(!validIdentity(identity)) return false;
    const std::string name="Termination_"+identity;
    HKEY key = nullptr;
    const LONG opened=RegOpenKeyExW(HKEY_CURRENT_USER,selectionKey,0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key);
    if(opened==ERROR_FILE_NOT_FOUND) return true;
    if(opened!=ERROR_SUCCESS) return false;
    DWORD value=0,type=0,bytes=sizeof(value);
    const LONG result=RegQueryValueExA(key,name.c_str(),nullptr,&type,reinterpret_cast<BYTE*>(&value),&bytes);
    RegCloseKey(key);
    if(result==ERROR_FILE_NOT_FOUND) return true;
    if(result!=ERROR_SUCCESS || type!=REG_DWORD || bytes!=sizeof(value) || (value!=0 && value!=3)) return false;
    enabled=value==3;return true;
}
SelectionStatus readSelection(Selection& value) {
    value=Selection(); HKEY key=nullptr;
    const LONG opened=RegOpenKeyExW(HKEY_CURRENT_USER,selectionKey,0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key);
    if(opened==ERROR_FILE_NOT_FOUND) return SelectionStatus::Missing;
    if(opened!=ERROR_SUCCESS) return SelectionStatus::InvalidSnapshot;
    char buffer[80]={};DWORD bytes=sizeof(buffer),type=0;
    LONG result=RegQueryValueExA(key,"Selection",nullptr,&type,reinterpret_cast<BYTE*>(buffer),&bytes);
    const bool legacy=result==ERROR_FILE_NOT_FOUND;
    if(legacy) {
        bytes=sizeof(buffer);type=0;
        result=RegQueryValueExA(key,"Device",nullptr,&type,reinterpret_cast<BYTE*>(buffer),&bytes);
    }
    RegCloseKey(key);
    if(result==ERROR_FILE_NOT_FOUND) return SelectionStatus::Missing;
    if(result!=ERROR_SUCCESS || type!=REG_SZ || !bytes || bytes>sizeof(buffer) || buffer[bytes-1] ||
       strlen(buffer)!=bytes-1) return SelectionStatus::InvalidSnapshot;
    std::string record(buffer);
    if(legacy) {
        if(!validIdentity(record) || !legacyTermination(record,value.termination)) return SelectionStatus::Invalid;
        value.mode=SelectionMode::Manual;value.device=record;return SelectionStatus::Present;
    }
    // Explicit selectors retain their old legacy-default override. Custom or
    // unreadable snapshots must fail closed rather than discard timing silently.
    const bool timingRecord=record.compare(0,2,"v3")==0 || record.compare(0,2,"v4")==0;
    if(timingRecord && record.compare(2,8,";manual;")!=0) return SelectionStatus::InvalidSnapshot;
    if(timingRecord) {
        std::vector<std::string> fields;
        size_t begin=0,end=0;
        while((end=record.find(';',begin))!=std::string::npos) {
            fields.push_back(record.substr(begin,end-begin));begin=end+1;
        }
        fields.push_back(record.substr(begin));
        if(fields.size()!=6 || !validIdentity(fields[2]) || (fields[3]!="0" && fields[3]!="1") ||
           !samplePercent(fields[4],value.nominalSample) || !samplePercent(fields[5],value.dataSample)) return SelectionStatus::InvalidSnapshot;
        value.mode=SelectionMode::Manual;value.device=fields[2];value.termination=fields[3]=="1";
        value.customSampling=true;value.classicSampling=fields[0]=="v4";return SelectionStatus::Present;
    }
    if(record=="v2;auto;0" || record=="v2;auto;1") {
        value.termination=record.back()=='1';return SelectionStatus::Present;
    }
    if(record.compare(0,10,"v2;manual;")==0) {
        if(record.size()<13 || record[record.size()-2]!=';' || (record.back()!='0' && record.back()!='1')) return SelectionStatus::Invalid;
        value.mode=SelectionMode::Manual;value.device=record.substr(10,record.size()-12);value.termination=record.back()=='1';
        return validIdentity(value.device) ? SelectionStatus::Present : SelectionStatus::Invalid;
    }
    if(record.size()<3 || record[record.size()-2]!=';' || (record.back()!='0' && record.back()!='3'))
        return SelectionStatus::Invalid;
    value.mode=SelectionMode::Manual;value.device=record.substr(0,record.size()-2);value.termination=record.back()=='3';
    return validIdentity(value.device) ? SelectionStatus::Present : SelectionStatus::Invalid;
}
bool savedTermination(const std::string& identity,bool& enabled,const Selection* snapshot) {
    Selection saved;
    if(snapshot) saved=*snapshot;
    else {
        const auto status=readSelection(saved);
        if(status==SelectionStatus::Invalid || status==SelectionStatus::InvalidSnapshot) return false;
    }
    if(saved.mode==SelectionMode::Manual && saved.device==identity) {
        enabled=saved.termination;return true;
    }
    return legacyTermination(identity,enabled);
}
bool saveSelection(const Selection& value) {
    if((value.mode!=SelectionMode::Auto && value.mode!=SelectionMode::Manual) ||
       (value.mode==SelectionMode::Manual ? !validIdentity(value.device) : !value.device.empty()) ||
       (value.classicSampling && !value.customSampling) ||
       (value.customSampling && (value.mode!=SelectionMode::Manual || value.nominalSample<1 || value.nominalSample>100 ||
        value.dataSample<1 || value.dataSample>100))) return false;
    SelectionLock lock; if(!lock.acquired) return false;
    const std::string record=(value.mode==SelectionMode::Auto ? std::string("v2;auto") :
        (value.customSampling ? (value.classicSampling ? "v4;manual;" : "v3;manual;") : "v2;manual;")+value.device)+(value.termination?";1":";0")+
        (value.customSampling ? ";"+std::to_string(value.nominalSample)+";"+std::to_string(value.dataSample) : "");
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,selectionKey,0,nullptr,0,KEY_SET_VALUE|KEY_WOW64_64KEY,
        nullptr,&key,nullptr)!=ERROR_SUCCESS) return false;
    // One atomic snapshot binds timing to the same exact identity/termination.
    const LONG result=RegSetValueExA(key,"Selection",0,REG_SZ,reinterpret_cast<const BYTE*>(record.c_str()),
        static_cast<DWORD>(record.size()+1));
    RegCloseKey(key);
    return result==ERROR_SUCCESS;
}
bool Api::load(std::string& error) {
    if (initialized) return true;
    const std::wstring directory = moduleDirectory();
    if (directory.empty()) { error = "Cannot locate the BMJ2534 module directory"; return false; }
    // Resolve the exact companion; never load a working-directory DLL.
    const wchar_t* companion = L"BMAPI.dll";
    module = LoadLibraryExW((directory + companion).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { error = "Cannot load the matching private BMAPI companion"; return false; }
#define BIND(member, symbol) member = reinterpret_cast<decltype(member)>(GetProcAddress(module, #symbol)); if (!member) { error = "BMAPI is missing " #symbol; unload(); return false; }
    BIND(init, BM_Init); BIND(uninit, BM_UnInit); BIND(enumerate, BM_Enumerate);
    BIND(open, BM_OpenEx); BIND(close, BM_Close); BIND(read, BM_Read); BIND(write, BM_Write);
    BIND(bitrate, BM_SetBitrate); BIND(getBitrate, BM_GetBitrate); BIND(mode, BM_SetCanMode); BIND(getMode, BM_GetCanMode);
    BIND(termination, BM_SetTerminalRegister); BIND(getTermination, BM_GetTerminalRegister);
    BIND(clear, BM_ClearBufferEx); BIND(clearAll, BM_ClearBuffer); BIND(status, BM_GetStatus); BIND(timestamp, BM_GetTimestamp);
    BIND(recoverBusOff, BM_RecoverBusOff);
    BIND(version, BM_GetVersion);
#undef BIND
    const BM_StatusTypeDef result = init();
    if (result != BM_ERROR_OK) { error = "BM_Init failed"; unload(); return false; }
    initialized = true;
    return true;
}
void Api::unload() {
    if (initialized && uninit) uninit();
    initialized = false;
    if (module) FreeLibrary(module);
    module = nullptr;
}
BM_StatusTypeDef Api::list(std::vector<BM_ChannelInfoTypeDef>& output) {
    output.clear();
    try {
        int capacity=32;
        const int maximum=INT_MAX/static_cast<int>(sizeof(BM_ChannelInfoTypeDef));
        for(;;) {
            std::vector<BM_ChannelInfoTypeDef> infos(static_cast<size_t>(capacity));
            int count=capacity;
            const BM_StatusTypeDef result=enumerate(infos.data(),&count);
            if(result!=BM_ERROR_OK) return result;
            if(count<0 || count>capacity) return BM_ERROR_ILLDATA;
            // BMAPI returns the number written, not the required total. A full
            // raw buffer may be truncated, even when filtered CAN count is low.
            if(count==capacity) {
                if(capacity==maximum) return BM_ERROR_RESOURCE;
                capacity=capacity>maximum/2 ? maximum : capacity*2;
                continue;
            }
            for(int i=0;i<count;++i) {
                if((infos[i].cap & (BM_CAN_CAP|BM_CAN_FD_CAP)) && !(infos[i].cap & (BM_REMOTE_CAP|BM_VIRTUAL_CAP))) output.push_back(infos[i]);
            }
            return BM_ERROR_OK;
        }
    } catch(const std::bad_alloc&) { output.clear(); return BM_ERROR_RESOURCE; }
}
}
