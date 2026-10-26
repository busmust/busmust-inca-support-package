// SPDX-License-Identifier: Apache-2.0
// Test double only: no USB driver, firmware or BMAPI implementation.
#define BMAPI_EXPORT
#include "bmapi.h"
#include <mutex>
#include <deque>
#include <chrono>
#include <cstring>
#include <cstdlib>
#define NOMINMAX
#include <windows.h>
namespace {
struct Port { bool open=false; BM_CanModeTypeDef mode=BM_CAN_OFF_MODE; BM_BitrateTypeDef rate={}; BM_TerminalResistorTypeDef term=BM_TRESISTOR_DISABLED; std::deque<BM_DataTypeDef> rx,pending,writes; } ports[128];
int deviceCount=2,perDevice=8,identityBias=0;bool reversed=false;
unsigned enumerationCalls=0,lastCapacity=0,openedCount=0;
std::mutex mutex;
bool initialized=false,holdAck=false,lost=false;
int modeFailure=0,rounding=0;
int clearFailure=0,recoverFailure=0,statusFailure=0,writeFailure=0;
unsigned recoverCalls=0,clearAllCalls=0;
bool busOff=false,persistent=false,classicOnly=false;

unsigned readsAfterClose=0,writeCount=0,clearRx=0,clearTx=0;
uint32_t stamp() { return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
Port* port(void* p) { for(auto& item:ports) if(p==&item) return &item; return nullptr; }
BM_StatusTypeDef ready(Port* p) { return !p || !p->open || (p==&ports[0] && lost) ? BM_ERROR_ILLHW : BM_ERROR_OK; }
void ack(Port& p,BM_DataTypeDef data) { data.header.type|=BM_ACK_DATA; data.timestamp=stamp(); p.rx.push_back(data); }
}
BM_StatusTypeDef BM_Init() { std::lock_guard<std::mutex> l(mutex); if(initialized) return BM_ERROR_ILLOPERATION;
initialized=true; return BM_ERROR_OK; }
BM_StatusTypeDef BM_UnInit() { std::lock_guard<std::mutex> l(mutex); initialized=false; return BM_ERROR_OK; }
BM_StatusTypeDef BM_Enumerate(BM_ChannelInfoTypeDef* infos,int* count) {
    std::lock_guard<std::mutex> l(mutex);
    ++enumerationCalls;lastCapacity=*count;
int secondCount=perDevice;
    const int total=deviceCount*(perDevice+4)+2;
    const int n=total<*count ? total : *count;
    memset(infos,0,n*sizeof(*infos));
    for(int i=0;i<n;++i) {
        const int source=reversed ? total-1-i : i;
        if(source>=deviceCount*(perDevice+4)) { infos[i].cap=BM_CAN_FD_CAP | ((source & 1)?BM_VIRTUAL_CAP:BM_REMOTE_CAP); continue; }
        int d=source/(perDevice+4),p=source%(perDevice+4);
        memcpy(infos[i].uid,"TESTDEVICE01",12);infos[i].uid[11]=static_cast<unsigned char>('1'+d+identityBias);
        infos[i].version[0]=d==0?3:2;infos[i].version[1]=d==0?2:6;infos[i].version[2]=d==0?6:3;infos[i].version[3]=1;infos[i].port=p;infos[i].cap=p<perDevice && (d!=1 || p<secondCount) ? (classicOnly ? BM_CAN_CAP : BM_CAN_FD_CAP) : BM_LIN_CAP;
        sprintf_s(reinterpret_cast<char*>(infos[i].sn),sizeof(infos[i].sn),"%d",10126+d);
        sprintf_s(infos[i].name,"BM-PCIE-X%d(%s) CH%d",d==1?secondCount:perDevice,infos[i].sn,p+1);
    }
    *count=n;return BM_ERROR_OK;
}
BM_StatusTypeDef BM_OpenEx(BM_ChannelHandle* out,BM_ChannelInfoTypeDef* info,uint32_t mode,BM_TerminalResistorTypeDef term,const BM_BitrateTypeDef* rate,const BM_RxFilterTypeDef*,int) {
    std::lock_guard<std::mutex> l(mutex); const int d=info->uid[11]-'1'-identityBias;if(d<0 || d>=deviceCount || info->port>=perDevice) return BM_ERROR_ILLHW;auto& p=ports[d*32+info->port]; if(p.open) return BM_ERROR_HWINUSE;
    p.open=true; ++openedCount; p.mode=static_cast<BM_CanModeTypeDef>(mode); p.term=term; p.rate=*rate; p.rx.clear(); p.pending.clear(); p.writes.clear(); *out=&p; return BM_ERROR_OK;
}
BM_StatusTypeDef BM_Close(BM_ChannelHandle h) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(!p || !p->open) return BM_ERROR_ILLHW; p->open=false; --openedCount; p->rx.clear(); p->pending.clear(); return BM_ERROR_OK; }
BM_StatusTypeDef BM_Read(BM_ChannelHandle h,BM_DataTypeDef* out) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(p && !p->open) ++readsAfterClose; if(ready(p)) return ready(p); if(p->mode==BM_CAN_OFF_MODE) return BM_ERROR_BUSOFF; if(p->rx.empty()) return BM_ERROR_QRCVEMPTY; *out=p->rx.front(); p->rx.pop_front(); return BM_ERROR_OK; }
BM_StatusTypeDef BM_Write(BM_ChannelHandle h,const BM_DataTypeDef* data,int,uint32_t*) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); if(p->mode==BM_CAN_OFF_MODE) return BM_ERROR_BUSOFF; if(p==&ports[0] && writeFailure)return static_cast<BM_StatusTypeDef>(writeFailure); ++writeCount; p->writes.push_back(*data); if(holdAck) p->pending.push_back(*data); else ack(*p,*data); return BM_ERROR_OK; }
BM_StatusTypeDef BM_SetBitrate(BM_ChannelHandle h,const BM_BitrateTypeDef* r) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); p->rate=*r; if(rounding) { --rounding; ++p->rate.nbitrate; } return BM_ERROR_OK; }
BM_StatusTypeDef BM_GetBitrate(BM_ChannelHandle h,BM_BitrateTypeDef* r) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); *r=p->rate; return BM_ERROR_OK; }
BM_StatusTypeDef BM_SetCanMode(BM_ChannelHandle h,BM_CanModeTypeDef m) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); if(modeFailure) { if(modeFailure>0) --modeFailure; return BM_ERROR_CONFIG; } p->mode=m; return BM_ERROR_OK; }
BM_StatusTypeDef BM_GetCanMode(BM_ChannelHandle h,BM_CanModeTypeDef* m) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); *m=p->mode; return BM_ERROR_OK; }
BM_StatusTypeDef BM_SetTerminalRegister(BM_ChannelHandle h,BM_TerminalResistorTypeDef t) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); p->term=t; return BM_ERROR_OK; }
BM_StatusTypeDef BM_GetTerminalRegister(BM_ChannelHandle h,BM_TerminalResistorTypeDef* t) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); *t=p->term; return BM_ERROR_OK; }
BM_StatusTypeDef BM_ClearBufferEx(BM_ChannelHandle h,BM_BufferId direction) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); if(clearFailure)return static_cast<BM_StatusTypeDef>(clearFailure); if(direction==BM_RXQ_BUFFER) { ++clearRx; std::deque<BM_DataTypeDef> keep; for(const auto& d:p->rx) if(d.header.type & BM_ACK_DATA) keep.push_back(d); p->rx.swap(keep); } else if(direction==BM_TXQ_BUFFER) { ++clearTx; p->pending.clear(); } else return BM_ERROR_ILLPARAMVAL; return BM_ERROR_OK; }
BM_StatusTypeDef BM_ClearBuffer(BM_ChannelHandle h) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); ++clearAllCalls;p->rx.clear(); p->pending.clear(); return BM_ERROR_OK; }
BM_StatusTypeDef BM_GetStatus(BM_ChannelHandle h,BM_StatusInfoHandle status) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); if(p==&ports[0] && statusFailure)return static_cast<BM_StatusTypeDef>(statusFailure);
memset(status,0,sizeof(BM_CanStatusInfoTypeDef));
if(p==&ports[0])static_cast<BM_CanStatusInfoTypeDef*>(status)->TXBO=busOff;
return BM_ERROR_OK; }
BM_StatusTypeDef BM_GetTimestamp(BM_ChannelHandle h,uint32_t* out) { std::lock_guard<std::mutex> l(mutex); auto* p=port(h); if(ready(p)) return ready(p); *out=stamp(); return BM_ERROR_OK; }
uint32_t BM_GetVersion() { return BM_API_VERSION; }
void BM_GetErrorText(BM_StatusTypeDef,char* out,int bytes,uint16_t) { if(bytes) *out=0; }
extern "C" __declspec(dllexport) void MockSet(int key,int value) { std::lock_guard<std::mutex> l(mutex); if(key==1) holdAck=value!=0; if(key==2) lost=value!=0; if(key==3) modeFailure=value; if(key==4) rounding=value; if(key==5 && value>=0 && value<=20) identityBias=value; }
extern "C" __declspec(dllexport) unsigned MockStats(int key) { std::lock_guard<std::mutex> l(mutex); switch(key) { case 0:return openedCount; case 90:return enumerationCalls; case 91:return lastCapacity; case 1:return readsAfterClose; case 2:return writeCount; case 3:return clearRx; case 4:return clearTx; case 5:return static_cast<unsigned>(ports[0].pending.size()); case 6:return static_cast<unsigned>(ports[0].rx.size()); case 7:return ports[0].rate.nsamplepos; case 8:return ports[0].rate.dsamplepos; default:return 0; } }
extern "C" __declspec(dllexport) void MockAck(int index) { std::lock_guard<std::mutex> l(mutex); auto& p=ports[index]; while(!p.pending.empty()) { ack(p,p.pending.front()); p.pending.pop_front(); } }
extern "C" __declspec(dllexport) void MockInject(int index,uint32_t id,unsigned flags,const unsigned char* bytes,unsigned size) {
    std::lock_guard<std::mutex> l(mutex); BM_DataTypeDef data={}; data.header.type=BM_CAN_FD_DATA; data.length=8+size; data.timestamp=stamp();
    auto* f=reinterpret_cast<BM_CanMessageTypeDef*>(data.payload); f->ctrl.rx.IDE=(flags&0x100)!=0; f->ctrl.rx.FDF=(flags&0x100000)!=0; f->ctrl.rx.BRS=(flags&0x80000)!=0;
    f->ctrl.rx.ESI=(flags&0x200000)!=0;
    unsigned lengths[]={0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64}; for(unsigned i=0;i<16;++i) if(lengths[i]==size) f->ctrl.rx.DLC=i;
    BM_SET_CAN_MSG_ID(*f,id); memcpy(f->payload,bytes,size); ports[index].rx.push_back(data);
}
extern "C" __declspec(dllexport) int MockPopWrite(int index,uint32_t* id,unsigned* flags,unsigned* size,unsigned char* bytes) {
    std::lock_guard<std::mutex> l(mutex); auto& p=ports[index]; if(p.writes.empty()) return 0;
    auto d=p.writes.front(); p.writes.pop_front(); auto* f=reinterpret_cast<BM_CanMessageTypeDef*>(d.payload);
    *id=f->ctrl.tx.IDE ? BM_GET_EXT_MSG_ID(f->id) : BM_GET_STD_MSG_ID(f->id);
    *flags=(f->ctrl.tx.IDE?0x100:0)|(f->ctrl.tx.FDF?0x100000:0)|(f->ctrl.tx.BRS?0x80000:0);
    unsigned lengths[]={0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64}; *size=lengths[f->ctrl.tx.DLC]; memcpy(bytes,f->payload,*size); return 1;
}

extern "C" __declspec(dllexport) void MockTopology(int devices,int channels,int reverse) {
    std::lock_guard<std::mutex> l(mutex);
    if(devices<0 || devices>3 || channels<1 || channels>32)return;
    deviceCount=devices;perDevice=channels;reversed=reverse!=0;
}

BM_StatusTypeDef BM_RecoverBusOff(BM_ChannelHandle h) {
std::lock_guard<std::mutex> l(mutex);auto* p=port(h);if(ready(p))return ready(p);
++recoverCalls;if(recoverFailure)return static_cast<BM_StatusTypeDef>(recoverFailure);
p->pending.clear();if(!persistent)busOff=false;return BM_ERROR_OK;
}
extern "C" __declspec(dllexport) void MockFault(int key,int value) {
std::lock_guard<std::mutex> l(mutex);
switch(key){case 1:clearFailure=value;break;case 2:recoverFailure=value;break;
case 3:busOff=value!=0;break;case 4:persistent=value!=0;break;
case 5:statusFailure=value;break;case 6:writeFailure=value;break;case 7:classicOnly=value!=0;break;}
}
extern "C" __declspec(dllexport) unsigned MockReviewStats(int key) {
std::lock_guard<std::mutex> l(mutex);switch(key){case 1:return recoverCalls;
case 2:return clearAllCalls;case 3:return ports[0].term;case 4:return ports[0].mode;
default:return 0;}}
