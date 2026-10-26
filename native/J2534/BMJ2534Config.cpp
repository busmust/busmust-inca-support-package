#include "bmj2534_backend.h"
#include "bmj2534_version.h"
#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>
#include <algorithm>
#include <string>
#include <cwchar>
#include <cstdio>
#include <cstring>

namespace {
const int CHANNEL=1000, REFRESH=1001, STATUS=1002, TERMINATION=1003, FIRMWARE=1006, MODE=1007, VERSIONS=1008;
const int SAMPLING=1009, NOMINAL=1010, DATA=1011;
struct Context {
    bmj::Api api;std::vector<BM_ChannelInfoTypeDef> devices;
    bool edited=false,termination=false,autoMode=true,autoTermination=false,validMode=true;std::string draft;
    std::string samplingDevice;
    bool customSampling=false,updatingSampling=false;
    std::wstring nominalDraft=L"75",dataDraft=L"80";
};
std::string jsonString(const char* value,size_t length) {
    std::string result="\"";
    for(size_t i=0;i<length && value[i];++i) {
        const unsigned char ch=static_cast<unsigned char>(value[i]);
        if(ch=='"' || ch=='\\') { result+='\\'; result+=ch; }
        else if(ch<32) { char escaped[7]={}; sprintf_s(escaped,"\\u%04X",ch); result+=escaped; }
        else result+=ch;
    }
    return result+'"';
}
bool writeJson(const std::string& value) {
    HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);
    if(!output || output==INVALID_HANDLE_VALUE || value.size()>MAXDWORD) return false;
    DWORD written=0;
    return WriteFile(output,value.data(),static_cast<DWORD>(value.size()),&written,nullptr) && written==value.size();
}
int enumerateJson() {
    // Read-only discovery shares the exact provider enumeration/filter logic.
    // No BM_OpenEx, bus configuration, registry write or GUI is performed.
    bmj::Api api;
    struct Unload { bmj::Api& api; ~Unload() { api.unload(); } } unload{api};
    try {
        std::string error;
        if(!api.load(error)) {
            writeJson("{\"status\":\"error\",\"message\":"+jsonString(error.c_str(),error.size())+"}\n");return 1;
        }
        std::vector<BM_ChannelInfoTypeDef> channels;
        const BM_StatusTypeDef status=api.list(channels);
        if(status!=BM_ERROR_OK) {
            char code[64]={};sprintf_s(code,"{\"status\":\"error\",\"code\":%u}\n",static_cast<unsigned>(status));writeJson(code);return 1;
        }
        std::string result="{\"schema\":\"busmust-j2534-channels-v1\",\"status\":\"ok\",\"hardware_opened\":false,\"channels\":[";
        bool first=true;
        for(const auto& item:channels) {
            char name[sizeof(item.name)+1]={};memcpy(name,item.name,sizeof(item.name));
            if(!name[0] || !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name,-1,nullptr,0)) {
                writeJson("{\"status\":\"error\",\"message\":\"BMAPI returned an empty or invalid UTF-8 channel name\"}\n");return 1;
            }
            char fields[256]={},serial[sizeof(item.sn)*2+1]={};
            for(unsigned i=0;i<sizeof(item.sn);++i) sprintf_s(serial+i*2,sizeof(serial)-i*2,"%02X",item.sn[i]);
            sprintf_s(fields,",\"port\":%u,\"vid\":%u,\"pid\":%u,\"cap\":%u,\"firmware\":\"%u.%u.%u.%u\",\"sn_hex\":\"%s\"}",
                item.port,item.vid,item.pid,item.cap,item.version[0],item.version[1],item.version[2],item.version[3],serial);
            if(!first) result+=',';
            first=false;
            result+="{\"uid\":\""+bmj::identity(item).substr(0,24)+"\",\"name\":"+jsonString(name,sizeof(item.name))+fields;
        }
        result+="]}\n";
        return writeJson(result) ? 0 : 2;
    } catch(...) {
        writeJson("{\"status\":\"error\",\"message\":\"Channel discovery failed\"}\n");return 1;
    }
}
std::wstring text(unsigned id) {
    wchar_t value[512]={}; LoadStringW(GetModuleHandleW(nullptr),id,value,_countof(value)); return value;
}
std::wstring wide(const char* value) {
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,nullptr,0);
    if(!size) return {};
    std::wstring result(size,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,&result[0],size);
    result.resize(size-1); return result;
}
void selectLanguage(const wchar_t* command) {
    LANGID language=0;
    if(wcsstr(command,L"--language=en")) language=MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US);
    else if(wcsstr(command,L"--language=zh")) language=MAKELANGID(LANG_CHINESE,SUBLANG_CHINESE_SIMPLIFIED);
    else {
        // ETAS documents this product-language preference. Read it only; the
        // configuration tool never changes INCA or Windows language settings.
        const HKEY roots[]={HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE};
        const REGSAM views[]={KEY_WOW64_64KEY,KEY_WOW64_32KEY};
        for(const auto root:roots) {
            for(const auto view:views) {
                HKEY key=nullptr;
                if(RegOpenKeyExW(root,L"Software\\ETAS\\General",0,KEY_QUERY_VALUE|view,&key)!=ERROR_SUCCESS) continue;
                wchar_t value[64]={};DWORD type=0,bytes=sizeof(value);
                const auto result=RegQueryValueExW(key,L"Language",nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes);RegCloseKey(key);
                if(result!=ERROR_SUCCESS || type!=REG_SZ || bytes<sizeof(wchar_t) || bytes>sizeof(value) || value[bytes/sizeof(wchar_t)-1]) continue;
                if(_wcsicmp(value,L"Chinese")==0) language=MAKELANGID(LANG_CHINESE,SUBLANG_CHINESE_SIMPLIFIED);
                else if(_wcsicmp(value,L"English")==0 || _wcsicmp(value,L"German")==0 || _wcsicmp(value,L"French")==0 || _wcsicmp(value,L"Japanese")==0)
                    language=MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US);
                if(language) break;
            }
            if(language) break;
        }
    }
    if(language) SetThreadUILanguage(language);
}
// Names are presentation only. Every combo item maps back to the original
// BMAPI record; opening/saving always uses its complete UID and native port.
std::wstring channelName(const BM_ChannelInfoTypeDef& info) {
    char name[sizeof(info.name)+1]={};
    memcpy(name,info.name,sizeof(info.name));
    const auto label=wide(name);
    return label.empty() ? wide(bmj::identity(info).c_str()) : label;
}
void placeholder(HWND window,int control,unsigned label) {
    SendDlgItemMessageW(window,control,CB_RESETCONTENT,0,0);
    SendDlgItemMessageW(window,control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text(label).c_str()));
    SendDlgItemMessageW(window,control,CB_SETITEMDATA,0,static_cast<LPARAM>(-1));
    SendDlgItemMessageW(window,control,CB_SETCURSEL,0,0);
}
const BM_ChannelInfoTypeDef* selectedChannel(HWND window,const Context& context) {
    const auto selected=SendDlgItemMessageW(window,CHANNEL,CB_GETCURSEL,0,0);
    if(selected<0) return nullptr;
    const auto index=SendDlgItemMessageW(window,CHANNEL,CB_GETITEMDATA,selected,0);
    return index>=0 && static_cast<size_t>(index)<context.devices.size()?&context.devices[index]:nullptr;
}
void samplingControls(HWND window,Context& context,const BM_ChannelInfoTypeDef* selected) {
    const bool enabled=context.validMode && !context.autoMode && selected;
    if(enabled && context.samplingDevice!=bmj::identity(*selected)) {
        context.samplingDevice=bmj::identity(*selected);
        bmj::Selection saved;
        const bool restored=bmj::readSelection(saved)==bmj::SelectionStatus::Present &&
            saved.mode==bmj::SelectionMode::Manual && saved.device==context.samplingDevice && saved.customSampling;
        context.customSampling=restored;
        context.nominalDraft=std::to_wstring(restored ? saved.nominalSample : 75);
        context.dataDraft=std::to_wstring(restored ? saved.dataSample : 80);
    }
    const bool custom=enabled && context.customSampling;
    context.updatingSampling=true;
    SendDlgItemMessageW(window,SAMPLING,CB_SETCURSEL,custom?1:0,0);
    SetDlgItemTextW(window,NOMINAL,custom?context.nominalDraft.c_str():L"75");
    SetDlgItemTextW(window,DATA,custom?context.dataDraft.c_str():L"80");
    context.updatingSampling=false;
    EnableWindow(GetDlgItem(window,SAMPLING),enabled);
    for(const auto id:{NOMINAL,208}) EnableWindow(GetDlgItem(window,id),custom);
    const bool dataEnabled=custom && (selected->cap & BM_CAN_FD_CAP);
    for(const auto id:{DATA,209}) EnableWindow(GetDlgItem(window,id),dataEnabled);
}
void channelChanged(HWND window,Context& context) {
    const auto* channel=selectedChannel(window,context);
    std::wstring firmware;
    if(channel) {
        firmware=std::to_wstring(channel->version[0])+L"."+std::to_wstring(channel->version[1])+L"."+
                 std::to_wstring(channel->version[2])+L"."+std::to_wstring(channel->version[3]);
    }
    SetDlgItemTextW(window,FIRMWARE,firmware.c_str());
    samplingControls(window,context,selectedChannel(window,context));
    if(context.autoMode && context.validMode) {
        EnableWindow(GetDlgItem(window,IDOK),TRUE);EnableWindow(GetDlgItem(window,TERMINATION),TRUE);
        SendDlgItemMessageW(window,TERMINATION,CB_SETCURSEL,context.autoTermination?1:0,0);
        SetDlgItemTextW(window,STATUS,text(context.devices.empty()?327:(context.devices.size()==1?329:328)).c_str());return;
    }
    const auto* selected=selectedChannel(window,context);
    EnableWindow(GetDlgItem(window,IDOK),context.validMode && selected!=nullptr);
    EnableWindow(GetDlgItem(window,TERMINATION),context.validMode && selected!=nullptr);
    bool enabled=false;
    const bool readable=!selected || bmj::savedTermination(bmj::identity(*selected),enabled);
    SendDlgItemMessageW(window,TERMINATION,CB_SETCURSEL,enabled?1:0,0);
    SetDlgItemTextW(window,STATUS,text(!selected?326:(readable?313:320)).c_str());
}
void rememberDraft(HWND window,Context& context) {
    if(context.autoMode) {
        context.edited=true;context.autoTermination=SendDlgItemMessageW(window,TERMINATION,CB_GETCURSEL,0,0)==1;return;
    }
    const auto* selected=selectedChannel(window,context);
    context.edited=true;context.draft=selected?bmj::identity(*selected):std::string();
    context.termination=selected && SendDlgItemMessageW(window,TERMINATION,CB_GETCURSEL,0,0)==1;
}
void refresh(HWND window,Context& context) {
    std::string error;
    const bool loaded=context.api.load(error);
    bmj::Selection saved;
    const auto savedStatus=bmj::readSelection(saved);
    if(!context.edited) {
        context.validMode=savedStatus==bmj::SelectionStatus::Missing || savedStatus==bmj::SelectionStatus::Present;
        context.autoMode=saved.mode==bmj::SelectionMode::Auto;
        context.draft=saved.device;
        if(context.autoMode) context.autoTermination=saved.termination;
        else context.termination=saved.termination;
    }
    SendDlgItemMessageW(window,MODE,CB_SETCURSEL,context.validMode?(context.autoMode?0:1):static_cast<WPARAM>(-1),0);
    context.devices.clear();
    placeholder(window,CHANNEL,312);
    EnableWindow(GetDlgItem(window,CHANNEL),FALSE);
    channelChanged(window,context);
    if(!loaded) { SetDlgItemTextW(window,STATUS,text(310).c_str()); return; }
    if(context.api.list(context.devices)!=BM_ERROR_OK) {
        SetDlgItemTextW(window,STATUS,text(311).c_str()); return;
    }
    const auto version=context.api.version();
    wchar_t versions[128]={};
    swprintf_s(versions,L"J2534 %hs | BMAPI %u.%u.%u.%u",BMJ_FILEVERSION_STRING,version>>24,(version>>16)&255,(version>>8)&255,version&255);
    SetDlgItemTextW(window,VERSIONS,versions);
    if(context.validMode && context.autoMode) {
        placeholder(window,CHANNEL,330);
        channelChanged(window,context);
        return;
    }
    if(context.devices.empty()) {
        SetDlgItemTextW(window,STATUS,text(312).c_str()); return;
    }
    SendDlgItemMessageW(window,CHANNEL,CB_RESETCONTENT,0,0);
    int target=-1;
    // Preserve BMAPI's channel names and order. Identity, never list position,
    // restores the selection after refresh and remains the saved binding.
    for(size_t i=0;i<context.devices.size();++i) {
        const auto& info=context.devices[i];
        auto label=channelName(info);
        if(std::count_if(context.devices.begin(),context.devices.end(),[&](const BM_ChannelInfoTypeDef& other) {
            return channelName(other)==label;
        })>1) label+=L" ["+wide(bmj::identity(info).c_str())+L"]";
        const auto item=SendDlgItemMessageW(window,CHANNEL,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        SendDlgItemMessageW(window,CHANNEL,CB_SETITEMDATA,item,static_cast<LPARAM>(i));
        if(bmj::identity(info)==context.draft) target=static_cast<int>(item);
    }
    if(target<0 && context.draft.empty() && context.validMode) target=0;
    if(target<0) {
        // An absent saved channel must not silently become a different device.
        // This explicit unavailable state exists only while the binding is missing.
        const auto item=SendDlgItemMessageW(window,CHANNEL,CB_INSERTSTRING,0,reinterpret_cast<LPARAM>(text(335).c_str()));
        SendDlgItemMessageW(window,CHANNEL,CB_SETITEMDATA,item,static_cast<LPARAM>(-1));
        target=static_cast<int>(item);
    }
    SendDlgItemMessageW(window,CHANNEL,CB_SETCURSEL,target,0);
    EnableWindow(GetDlgItem(window,CHANNEL),context.validMode);
    channelChanged(window,context);
    if(selectedChannel(window,context) && bmj::identity(*selectedChannel(window,context))==context.draft)
        SendDlgItemMessageW(window,TERMINATION,CB_SETCURSEL,context.termination?1:0,0);
    if(context.draft.empty() && selectedChannel(window,context)) {
        context.draft=bmj::identity(*selectedChannel(window,context));
        context.termination=SendDlgItemMessageW(window,TERMINATION,CB_GETCURSEL,0,0)==1;
    }
    if(!context.validMode) SetDlgItemTextW(window,STATUS,text(321).c_str());
    else if(!selectedChannel(window,context)) SetDlgItemTextW(window,STATUS,text(314).c_str());
    else SetDlgItemTextW(window,STATUS,text(context.edited?313:315).c_str());
}
INT_PTR CALLBACK dialog(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* context=reinterpret_cast<Context*>(GetWindowLongPtrW(window,DWLP_USER));
    switch(message) {
    case WM_INITDIALOG: {
        context=reinterpret_cast<Context*>(lParam); SetWindowLongPtrW(window,DWLP_USER,lParam);
        const auto instance=GetModuleHandleW(nullptr);
        // Use the original BUSMUST asset's native frame for each shell surface.
        // Reusing the large handle for the caption would rescale the small logo.
        const auto largeIcon=LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_SHARED);
        const auto smallIcon=LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED);
        SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(largeIcon));
        SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(smallIcon));
        SendDlgItemMessageW(window,TERMINATION,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text(317).c_str()));
        SendDlgItemMessageW(window,TERMINATION,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text(318).c_str()));
        SendDlgItemMessageW(window,MODE,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text(330).c_str()));
        SendDlgItemMessageW(window,MODE,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text(331).c_str()));
        SendDlgItemMessageW(window,SAMPLING,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text(332).c_str()));
        SendDlgItemMessageW(window,SAMPLING,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text(333).c_str()));
        for(const auto id:{NOMINAL,DATA}) SendDlgItemMessageW(window,id,EM_SETLIMITTEXT,3,0);
        SetDlgItemTextW(window,VERSIONS,(L"J2534 "+wide(BMJ_FILEVERSION_STRING)).c_str());
        // Dialog units include popup height for combos and a taller static
        // text cell. Match each label/value to the collapsed native row.
        RECT combo={};GetWindowRect(GetDlgItem(window,CHANNEL),&combo);
        for(const auto id:{200,201,204,205,206,207,208,209,FIRMWARE,VERSIONS,NOMINAL,DATA}) {
            RECT bounds={};GetWindowRect(GetDlgItem(window,id),&bounds);
            SetWindowPos(GetDlgItem(window,id),nullptr,0,0,bounds.right-bounds.left,combo.bottom-combo.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        refresh(window,*context); return TRUE;
    }
    case WM_COMMAND:
        if(!context) return FALSE;
        if(LOWORD(wParam)==MODE && HIWORD(wParam)==CBN_SELCHANGE) {
            rememberDraft(window,*context);
            const auto mode=SendDlgItemMessageW(window,MODE,CB_GETCURSEL,0,0);
            context->validMode=mode==0 || mode==1;context->autoMode=mode==0;context->edited=true;
            refresh(window,*context);return TRUE;
        }
        if(LOWORD(wParam)==CHANNEL && HIWORD(wParam)==CBN_SELCHANGE) {
            const auto item=SendDlgItemMessageW(window,CHANNEL,CB_GETCURSEL,0,0);
            if(item>0 && SendDlgItemMessageW(window,CHANNEL,CB_GETITEMDATA,0,0)==-1) {
                SendDlgItemMessageW(window,CHANNEL,CB_DELETESTRING,0,0);
                SendDlgItemMessageW(window,CHANNEL,CB_SETCURSEL,item-1,0);
            }
            channelChanged(window,*context);
            rememberDraft(window,*context);
            return TRUE;
        }
        if(LOWORD(wParam)==TERMINATION && HIWORD(wParam)==CBN_SELCHANGE) {
            rememberDraft(window,*context);
            wchar_t status[128]={};GetDlgItemTextW(window,STATUS,status,_countof(status));
            // Clear only the saved-termination warning; retain other validation errors.
            if(!context->autoMode && context->validMode && selectedChannel(window,*context) &&
               SendDlgItemMessageW(window,TERMINATION,CB_GETCURSEL,0,0)>=0 && std::wstring(status)==text(320))
                SetDlgItemTextW(window,STATUS,text(313).c_str());
            return TRUE;
        }
        if(LOWORD(wParam)==SAMPLING && HIWORD(wParam)==CBN_SELCHANGE) {
            if(!IsWindowEnabled(GetDlgItem(window,SAMPLING)))return TRUE;
            context->customSampling=SendDlgItemMessageW(window,SAMPLING,CB_GETCURSEL,0,0)==1;
            context->edited=true;samplingControls(window,*context,selectedChannel(window,*context));return TRUE;
        }
        if((LOWORD(wParam)==NOMINAL || LOWORD(wParam)==DATA) && HIWORD(wParam)==EN_CHANGE) {
            if(!context->updatingSampling && IsWindowEnabled(GetDlgItem(window,LOWORD(wParam)))) {
                wchar_t value[4]={};GetDlgItemTextW(window,LOWORD(wParam),value,_countof(value));
                (LOWORD(wParam)==NOMINAL ? context->nominalDraft : context->dataDraft)=value;
                context->edited=true;
            }
            return TRUE;
        }
        if(LOWORD(wParam)==REFRESH) { refresh(window,*context); return TRUE; }
        if(LOWORD(wParam)==IDOK) {
            if(!context->validMode)return TRUE;
            const auto term=SendDlgItemMessageW(window,TERMINATION,CB_GETCURSEL,0,0);
            if(term!=0 && term!=1) return TRUE;
            bmj::Selection choice;choice.termination=term==1;
            if(!context->autoMode) {
                const auto* info=selectedChannel(window,*context);if(!info)return TRUE;
                choice.mode=bmj::SelectionMode::Manual;choice.device=bmj::identity(*info);
                choice.customSampling=SendDlgItemMessageW(window,SAMPLING,CB_GETCURSEL,0,0)==1;
                choice.classicSampling=choice.customSampling;
                if(choice.customSampling) {
                    BOOL nominalValid=FALSE,dataValid=FALSE;
                    choice.nominalSample=GetDlgItemInt(window,NOMINAL,&nominalValid,FALSE);
                    choice.dataSample=(info->cap & BM_CAN_FD_CAP) ? GetDlgItemInt(window,DATA,&dataValid,FALSE) : 80;
                    if(!(info->cap & BM_CAN_FD_CAP)) dataValid=TRUE;
                    if(!nominalValid || !dataValid || choice.nominalSample<1 || choice.nominalSample>100 || choice.dataSample<1 || choice.dataSample>100) {
                        SetDlgItemTextW(window,STATUS,text(334).c_str());return TRUE;
                    }
                }
                // Auto has no identity; only Manual needs presence validation.
                std::vector<BM_ChannelInfoTypeDef> current;
                if(context->api.list(current)!=BM_ERROR_OK || std::none_of(current.begin(),current.end(),[&](const BM_ChannelInfoTypeDef& d){return bmj::identity(d)==choice.device;})) {
                    refresh(window,*context); SetDlgItemTextW(window,STATUS,text(314).c_str()); return TRUE;
                }
            }
            if(!bmj::saveSelection(choice)) { SetDlgItemTextW(window,STATUS,text(316).c_str()); return TRUE; }
            EndDialog(window,IDOK); return TRUE;
        }
        if(LOWORD(wParam)==IDCANCEL) { EndDialog(window,IDCANCEL); return TRUE; }
        break;
    case WM_DEVICECHANGE:
        // Windows broadcasts device-tree changes to top-level windows. Debounce
        // a burst and only enumerate; no background service or device open.
        if(wParam==DBT_DEVNODES_CHANGED || wParam==DBT_DEVICEARRIVAL || wParam==DBT_DEVICEREMOVECOMPLETE)
            SetTimer(window,1,400,nullptr);
        return TRUE;
    case WM_TIMER:
        if(wParam==1 && context) { KillTimer(window,1);refresh(window,*context);return TRUE; }
        break;
    case WM_DESTROY: KillTimer(window,1);break;
    case WM_CTLCOLORSTATIC:
        if(GetDlgCtrlID(reinterpret_cast<HWND>(lParam))==STATUS) {
            SetTextColor(reinterpret_cast<HDC>(wParam),RGB(0,102,204));
            SetBkColor(reinterpret_cast<HDC>(wParam),GetSysColor(COLOR_3DFACE));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
        }
        break;
    case WM_CLOSE: EndDialog(window,IDCANCEL); return TRUE;
    }
    return FALSE;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR command,int) {
    int argumentCount=0;
    LPWSTR* arguments=CommandLineToArgvW(GetCommandLineW(),&argumentCount);
    if(!arguments) {
        writeJson("{\"status\":\"error\",\"message\":\"Cannot parse command line\"}\n");
        return 1;
    }
    bool enumeration=false;
    for(int i=1;i<argumentCount;++i)
        enumeration=enumeration || wcscmp(arguments[i],L"--enumerate-json")==0;
    LocalFree(arguments);
    if(enumeration) {
        if(argumentCount!=2) {
            writeJson("{\"status\":\"error\",\"message\":\"Use --enumerate-json without other arguments\"}\n");
            return 1;
        }
        return enumerateJson();
    }
    selectLanguage(command);
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    Context context;
    const auto result=DialogBoxParamW(instance,MAKEINTRESOURCEW(100),nullptr,dialog,reinterpret_cast<LPARAM>(&context));
    context.api.unload();
    return result==-1 ? 1 : 0;
}
