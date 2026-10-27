// SPDX-License-Identifier: Apache-2.0
// Software regression only. It never loads an installed BMAPI or opens USB.
#include "bmj2534.h"
#include "bmj2534_backend.h"
#include "bmj2534_version.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <functional>

namespace {
unsigned checks=0;
void check(bool ok,const char* name) {
    ++checks;printf("%s %s\n",ok?"PASS":"FAIL",name);
    if(!ok)throw std::runtime_error(name);
}
template<class T> T symbol(HMODULE module,const char* name) {
    auto result=reinterpret_cast<T>(GetProcAddress(module,name));
    if(!result)throw std::runtime_error(std::string("Missing export: ")+name);
    return result;
}
struct Isolation {
    HKEY root=nullptr;bool overridden=false;std::wstring name;
    Isolation() {
        name=L"Software\\BUSMUST\\J2534Regression\\"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount());
        check(RegCreateKeyExW(HKEY_CURRENT_USER,name.c_str(),0,nullptr,0,KEY_ALL_ACCESS|KEY_WOW64_64KEY,nullptr,&root,nullptr)==ERROR_SUCCESS,"create isolated registry root");
        check(RegOverridePredefKey(HKEY_CURRENT_USER,root)==ERROR_SUCCESS,"redirect this process HKCU only");overridden=true;
    }
    ~Isolation() {
        if(overridden)RegOverridePredefKey(HKEY_CURRENT_USER,nullptr);
        if(root)RegCloseKey(root);
        RegDeleteTreeW(HKEY_CURRENT_USER,name.c_str());
    }
};
const wchar_t* keyName=L"Software\\BUSMUST\\J2534";
void reset(const char* record=nullptr,const char* valueName="Selection",DWORD type=REG_SZ) {
    RegDeleteTreeW(HKEY_CURRENT_USER,keyName);HKEY key=nullptr;
    check(RegCreateKeyExW(HKEY_CURRENT_USER,keyName,0,nullptr,0,KEY_ALL_ACCESS|KEY_WOW64_64KEY,nullptr,&key,nullptr)==ERROR_SUCCESS,"create isolated selection key");
    if(record)check(RegSetValueExA(key,valueName,0,type,reinterpret_cast<const BYTE*>(record),static_cast<DWORD>(strlen(record)+1))==ERROR_SUCCESS,"set controlled selection input");
    RegCloseKey(key);
}
bool selectionAbsent() {
    HKEY key=nullptr;const auto opened=RegOpenKeyExW(HKEY_CURRENT_USER,keyName,0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key);
    if(opened==ERROR_FILE_NOT_FOUND)return true;
    if(opened!=ERROR_SUCCESS)return false;
    const auto result=RegQueryValueExA(key,"Selection",nullptr,nullptr,nullptr,nullptr);RegCloseKey(key);return result==ERROR_FILE_NOT_FOUND;
}
std::string identity(unsigned bias=0,unsigned port=0) {
    BM_ChannelInfoTypeDef info={};memcpy(info.uid,"TESTDEVICE01",12);info.uid[11]+=static_cast<unsigned char>(bias);info.port=static_cast<unsigned short>(port);return bmj::identity(info);
}
void checkPe(const std::wstring& path) {
    DWORD ignored=0;const auto size=GetFileVersionInfoSizeW(path.c_str(),&ignored);check(size!=0,"PE version resource exists");
    std::vector<BYTE> bytes(size);check(GetFileVersionInfoW(path.c_str(),0,size,bytes.data())!=FALSE,"read PE version");
    VS_FIXEDFILEINFO* info=nullptr;UINT length=0;
    check(VerQueryValueW(bytes.data(),L"\\",reinterpret_cast<void**>(&info),&length)!=FALSE && length>=sizeof(*info),"PE fixed version block");
    unsigned a=0,b=0,c=0,d=0;check(sscanf_s(BMJ_FILEVERSION_STRING,"%u.%u.%u.%u",&a,&b,&c,&d)==4,"expected human version has four components");
    check(info->dwFileVersionMS==static_cast<DWORD>(MAKELONG(b,a)) && info->dwFileVersionLS==static_cast<DWORD>(MAKELONG(d,c)) && info->dwProductVersionMS==static_cast<DWORD>(MAKELONG(b,a)) && info->dwProductVersionLS==static_cast<DWORD>(MAKELONG(d,c)),"PE FileVersion/ProductVersion match independent product authority");
}
struct Client {
    HMODULE mock=nullptr,core=nullptr,endpoint=nullptr;std::vector<unsigned long> devices,channels;
    decltype(&PassThruOpen) open=nullptr;decltype(&PassThruClose) close=nullptr;decltype(&PassThruConnect) connect=nullptr;
    decltype(&PassThruDisconnect) disconnect=nullptr;decltype(&PassThruIoctl) ioctl=nullptr;
    decltype(&PassThruWriteMsgs) write=nullptr;decltype(&PassThruReadVersion) version=nullptr;
    void (__cdecl* topology)(int,int,int)=nullptr;void (__cdecl* set)(int,int)=nullptr;void (__cdecl* fault)(int,int)=nullptr;
    unsigned (__cdecl* stats)(int)=nullptr;unsigned (__cdecl* review)(int)=nullptr;
    Client(const std::wstring& folder) {
        mock=LoadLibraryExW((folder+L"BMAPI.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        check(mock!=nullptr,"load adjacent explicit test double");
        topology=symbol<decltype(topology)>(mock,"MockTopology");set=symbol<decltype(set)>(mock,"MockSet");
        fault=symbol<decltype(fault)>(mock,"MockFault");stats=symbol<decltype(stats)>(mock,"MockStats");review=symbol<decltype(review)>(mock,"MockReviewStats");
        // Refuse a real BMAPI before the provider can be loaded.
        const auto file=folder+(sizeof(void*)==8?L"BMJ2534.dll":L"BMJ25332.dll");
        core=LoadLibraryExW(file.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);check(core!=nullptr,"load matching Release provider");
        endpoint=LoadLibraryExW((folder+L"BMJ2534Endpoint.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);check(endpoint!=nullptr,"load generic endpoint");
        const char* names[]={"PassThruOpen","PassThruClose","PassThruConnect","PassThruDisconnect","PassThruReadMsgs","PassThruWriteMsgs","PassThruStartPeriodicMsg","PassThruStopPeriodicMsg","PassThruStartMsgFilter","PassThruStopMsgFilter","PassThruSetProgrammingVoltage","PassThruReadVersion","PassThruGetLastError","PassThruIoctl"};
        for(auto name:names)check(GetProcAddress(core,name) && GetProcAddress(endpoint,name),name);
#define BIND(member,name) member=symbol<decltype(member)>(core,name)
        BIND(open,"PassThruOpen");BIND(close,"PassThruClose");BIND(connect,"PassThruConnect");BIND(disconnect,"PassThruDisconnect");BIND(ioctl,"PassThruIoctl");BIND(write,"PassThruWriteMsgs");BIND(version,"PassThruReadVersion");
#undef BIND
        checkPe(file);checkPe(folder+L"BMJ2534Endpoint.dll");checkPe(folder+L"BMJ2534Config.exe");
    }
    ~Client() {clean();if(endpoint)FreeLibrary(endpoint);if(core)FreeLibrary(core);if(mock)FreeLibrary(mock);}
    void clean() {for(auto ch:channels)disconnect(ch);channels.clear();for(auto dev:devices)close(dev);devices.clear();}
    long opened(unsigned long& id,const char* selector=nullptr) {id=0;const auto result=open(const_cast<char*>(selector),&id);if(!result)devices.push_back(id);return result;}
    unsigned long connected(unsigned long id,unsigned long protocol=CAN) {unsigned long ch=0;check(connect(id,protocol,0,500000,&ch)==0,"connect");channels.push_back(ch);return ch;}
    long config(unsigned long ch,unsigned long parameter,unsigned long value,bool save=true,unsigned long* actual=nullptr) {
        SCONFIG item={parameter,value};SCONFIG_LIST list={1,&item};const auto result=ioctl(ch,save?SET_CONFIG:GET_CONFIG,&list,nullptr);if(actual)*actual=item.Value;return result;
    }
    bool sent(unsigned long ch) {PASSTHRU_MSG m={};m.ProtocolID=CAN;m.DataSize=12;const BYTE payload[]={0,0,4,0x30,0,1,2,3,4,5,6,7};memcpy(m.Data,payload,sizeof(payload));unsigned long n=1;return write(ch,&m,&n,1500)==0 && n==1;}
};
bool wait(const std::function<bool()>& predicate) {const auto start=GetTickCount();do {if(predicate())return true;Sleep(10);}while(static_cast<DWORD>(GetTickCount()-start)<2500);return false;}
void samplingCases(Client& c) {
    c.topology(1,1,0);
    reset();
    unsigned long device=0,actual=0;
    check(c.opened(device)==0,"open default sampling fixture");
    auto channel=c.connected(device);
    check(c.stats(7)==75,"Classic CAN defaults to 75 percent");
    check(c.config(channel,BIT_SAMPLE_POINT,0,false,&actual)==0 && actual==75,"Classic GET reports default readback");
    c.clean();
    check(c.opened(device)==0,"open FD default fixture");
    channel=c.connected(device,CAN_FD_PS);
    check(c.stats(7)==75 && c.stats(8)==80,"FD defaults are nominal 75 and data 80");
    c.clean();

    bmj::Selection selection;
    selection.mode=bmj::SelectionMode::Manual;
    selection.device=identity();
    selection.customSampling=true;
    selection.classicSampling=true;
    selection.nominalSample=65;
    selection.dataSample=85;
    check(bmj::saveSelection(selection),"save CAN and FD sampling snapshot");
    bmj::Selection restored;
    check(bmj::readSelection(restored)==bmj::SelectionStatus::Present && restored.classicSampling &&
        restored.nominalSample==65 && restored.dataSample==85,"read new sampling snapshot without swapping phases");
    const unsigned long protocols[]={CAN,ISO15765,CAN_FD_PS,FD_CAN_PS};
    for(auto protocol:protocols) {
        check(c.opened(device)==0,"open custom sampling fixture");
        channel=c.connected(device,protocol);
        check(c.stats(7)==65,"custom nominal sampling applies to CAN and FD");
        const bool fd=protocol==CAN_FD_PS || protocol==FD_CAN_PS;
        check(c.stats(8)==(fd?85:80),"custom data sampling applies only to FD");
        check(c.config(channel,HS_CAN_TERMINATION,3)==0 && c.stats(7)==65,"termination update preserves sampling");
        if(protocol==CAN) {
            check(c.config(channel,BIT_SAMPLE_POINT,0,false,&actual)==0 && actual==65,"Classic GET reports configured sampling");
            check(c.config(channel,BIT_SAMPLE_POINT,70)==0 && c.stats(7)==70,"explicit standard Classic setting still works");
            check(c.config(channel,BIT_SAMPLE_POINT,0,false,&actual)==0 && actual==70,"GET follows later Classic setting");
        }
        c.clean();
    }
    c.fault(7,1);
    check(c.opened(device)==0,"custom snapshot opens Classic-only channel");
    channel=c.connected(device);
    check(c.stats(7)==65,"Classic-only hardware receives custom sample point");
    c.clean();c.fault(7,0);

    reset(("v3;manual;"+identity()+";0;70;85").c_str());
    check(bmj::readSelection(restored)==bmj::SelectionStatus::Present && restored.customSampling &&
        !restored.classicSampling,"old FD-only snapshot retains its scope");
    check(c.opened(device)==0,"open legacy sampling as Classic");
    channel=c.connected(device);
    check(c.stats(7)==75,"old FD-only snapshot does not override Classic sampling");
    c.clean();
    check(c.opened(device)==0,"open legacy sampling as FD");
    channel=c.connected(device,CAN_FD_PS);
    check(c.stats(7)==70 && c.stats(8)==85,"legacy FD custom values remain unchanged");
    c.clean();
    reset(("v4;manual;"+identity()+";0;0;80").c_str());
    check(c.opened(device,identity().c_str())==ERR_FAILED && !device,"corrupt new sampling cannot be bypassed by explicit selector");
    reset();
}
void cases(Client& c) {
    unsigned long d=0;bmj::Selection saved;
    reset();c.topology(0,1,0);check(c.opened(d)==ERR_DEVICE_NOT_CONNECTED && !d,"Auto with zero physical candidates rejects");
    c.topology(1,1,0);check(c.opened(d)==0,"Auto unique A opens with LIN/remote/virtual filtered");
    check(selectionAbsent(),"implicit Auto never commits a Manual UID");
    check(bmj::readSelection(saved)==bmj::SelectionStatus::Missing && saved.mode==bmj::SelectionMode::Auto && !saved.termination,"no config means Auto and termination Off");
    char f[80]={},v[80]={},a[80]={};check(c.version(d,f,v,a)==0 && strcmp(f,"3.2.6.1")==0 && strcmp(v,BMJ_FILEVERSION_STRING)==0 && strcmp(a,"04.04")==0,"ReadVersion firmware/product/API have separate responsibilities");
    auto ch=c.connected(d);check(c.review(3)==BM_TRESISTOR_DISABLED,"Auto default termination Off");
    check(c.config(ch,HS_CAN_TERMINATION,3)==0 && c.review(3)==BM_TRESISTOR_120,"standard termination 3 maps to 120 ohms");
    unsigned long actual=0;check(c.config(ch,HS_CAN_TERMINATION,0,false,&actual)==0 && actual==3,"standard termination GET returns 3");
    check(c.config(ch,HS_CAN_TERMINATION,1)!=0,"standard termination rejects nonstandard value 1");
    check(c.config(ch,HS_CAN_TERMINATION,0)==0 && c.review(3)==BM_TRESISTOR_DISABLED,"standard termination 0 maps Off");
    check(c.sent(ch),"normal write requires transport confirmation");c.clean();
    c.set(5,1);check(c.opened(d)==0,"Close A then unique B opens without configuration");check(selectionAbsent(),"B resolution still leaves Auto implicit");c.clean();c.set(5,0);
    c.topology(2,1,0);check(c.opened(d)==ERR_FAILED && !d,"Auto two single-channel devices rejects ambiguity");
    c.topology(1,4,0);check(c.opened(d)==ERR_FAILED && !d,"Auto one four-channel device rejects ambiguity");
    saved=bmj::Selection();saved.mode=bmj::SelectionMode::Manual;saved.device=identity();saved.termination=true;
    check(bmj::saveSelection(saved),"save one atomic versioned Manual record");
    c.topology(1,1,0);c.set(5,1);check(c.opened(d)==ERR_DEVICE_NOT_CONNECTED && !d,"Manual A absent with only B does not substitute");
    saved=bmj::Selection();saved.termination=true;check(bmj::saveSelection(saved),"explicitly return to Auto with its termination preference");
    check(c.opened(d)==0,"Auto B now available");ch=c.connected(d);check(c.review(3)==BM_TRESISTOR_120,"saved Auto termination applied");c.clean();c.set(5,0);
    reset((identity()+";3").c_str());check(bmj::readSelection(saved)==bmj::SelectionStatus::Present && saved.mode==bmj::SelectionMode::Manual && saved.device==identity() && saved.termination,"old Selection is interpreted as Manual");
    check(c.opened(d)==0,"legacy exact selection opens");c.clean();
    reset(identity().c_str(),"Device");check(bmj::readSelection(saved)==bmj::SelectionStatus::Present && saved.mode==bmj::SelectionMode::Manual && saved.device==identity(),"legacy Device is interpreted as Manual");
    for(const char* corrupt:{"garbage","v3;auto;0","v2;auto;3","v2;manual;","v2;manual;544553544445564943453031:00;1"}) {reset(corrupt);check(c.opened(d)==ERR_FAILED && !d,"corrupt record never becomes Auto");}
    check(c.opened(d,identity().c_str())==0,"explicit selector overrides damaged default");c.clean();
    reset("damaged","Selection",REG_BINARY);check(c.opened(d)==ERR_FAILED && !d,"wrong registry type rejected");
    reset("v2;auto;0");c.topology(1,2,0);check(c.opened(d,identity(0,1).c_str())==0,"explicit physical selector overrides ambiguous Auto");c.clean();
    c.topology(1,1,0);check(c.opened(d)==0,"Auto connection before disconnect");ch=c.connected(d);const auto count=c.stats(0);c.set(2,1);c.set(5,1);
    check(wait([&]{return !c.sent(ch);}),"active A transport loss rejects writes");check(c.stats(0)==count,"active Auto never opens replacement B");c.clean();c.set(2,0);check(c.opened(d)==0,"normal Close/Open may resolve B");c.clean();c.set(5,0);
    reset();unsigned long ids[2]={};long results[2]={};
    std::thread one([&]{results[0]=c.open(nullptr,&ids[0]);});std::thread two([&]{results[1]=c.open(nullptr,&ids[1]);});one.join();two.join();
    check((results[0]==0 && results[1]==ERR_DEVICE_IN_USE) || (results[1]==0 && results[0]==ERR_DEVICE_IN_USE),"concurrent Open has exactly one owner");
    for(auto id:ids)if(id)c.devices.push_back(id);check(selectionAbsent(),"concurrent Auto Open does not publish a Manual record");c.clean();
    check(c.opened(d)==0,"open cleanup/recovery fixture");ch=c.connected(d);
    c.fault(1,BM_ERROR_ILLOPERATION);check(c.ioctl(ch,CLEAR_RX_BUFFER,nullptr,nullptr)==0 && c.ioctl(ch,CLEAR_TX_BUFFER,nullptr,nullptr)==0,"old directional-clear unsupported falls back locally");check(c.sent(ch),"old-clear fallback retains a usable port");
    c.fault(1,BM_ERROR_ILLPARAMVAL);check(c.ioctl(ch,CLEAR_TX_BUFFER,nullptr,nullptr)!=0,"real clear error propagates");c.fault(1,0);
    auto attempts=c.review(1);c.fault(3,1);check(wait([&]{return c.review(1)==attempts+1;}),"Bus-Off invokes native recovery");Sleep(300);check(c.review(1)==attempts+1,"one recovery attempt per episode");check(c.sent(ch),"positive ACK after recovery");
    attempts=c.review(1);c.fault(4,1);c.fault(3,1);check(wait([&]{return c.review(1)==attempts+1;}),"persistent Bus-Off receives one attempt");Sleep(500);check(c.review(1)==attempts+1 && !c.sent(ch),"persistent Bus-Off stays bounded and rejects writes");c.fault(3,0);c.fault(4,0);
    const auto clears=c.review(2);c.clean();check(c.review(2)==clears+1 && c.review(4)==BM_CAN_OFF_MODE,"Disconnect performs full clear and CAN-off");
    c.topology(2,32,0);check(c.opened(d,identity(1,31).c_str())==0,"enumeration grows to final 32-port device channel");c.clean();c.topology(2,32,1);check(c.opened(d,identity(1,31).c_str())==0,"physical identity survives enumeration reorder");c.clean();
    check(c.stats(0)==0 && c.stats(1)==0,"all test handles closed, no read after close");
}
}
int wmain() {
    try {
        wchar_t path[32768]={};const auto length=GetModuleFileNameW(nullptr,path,_countof(path));check(length && length<_countof(path),"resolve exact test runtime");
        const auto folder=std::wstring(path,length).substr(0,std::wstring(path,length).find_last_of(L"\\/")+1);
        const auto log=folder+L"test-local-appdata";CreateDirectoryW(log.c_str(),nullptr);SetEnvironmentVariableW(L"LOCALAPPDATA",log.c_str());
        Isolation registry;Client client(folder);cases(client);samplingCases(client);
        printf("PASS %u checks; %u-bit mock-only regression; no hardware or INCA claim\n",checks,static_cast<unsigned>(sizeof(void*)*8));return 0;
    } catch(const std::exception& error) {fprintf(stderr,"FAIL after %u checks: %s\n",checks,error.what());return 1;}
}
