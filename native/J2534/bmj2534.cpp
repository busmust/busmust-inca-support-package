#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "bmj2534.h"
#include "bmj2534_backend.h"
#include "bmj2534_isotp.h"
#include "bmj2534_private.h"
#include "bmj2534_log.h"
#include "bmj2534_version.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <intrin.h>

namespace {
using Clock = std::chrono::steady_clock;
using U = unsigned long;
const size_t RX_LIMIT = 256, TX_LIMIT = 64, FILTER_LIMIT = 20, PERIODIC_LIMIT = 10;
thread_local char lastError[80] = "No error";
std::atomic<bool> traceEnabled{false};
std::atomic<unsigned> traceCount{0};
std::atomic<unsigned> readMetadataCount{0},writeMetadataCount{0};
void refreshTrace() {
    HKEY key=nullptr; DWORD value=0,bytes=sizeof(value),type=0;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\BUSMUST\\J2534",0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key)==ERROR_SUCCESS) {
        if(RegQueryValueExW(key,L"TraceEnabled",nullptr,&type,reinterpret_cast<BYTE*>(&value),&bytes)!=ERROR_SUCCESS || type!=REG_DWORD || bytes!=sizeof(value)) value=0;
        RegCloseKey(key);
    }
    traceEnabled=value==1;
}
void trace(const char* event,U a=0,U b=0,U c=0) {
    // File diagnostics are independent of optional debugger output. Keep
    // successful polling and per-message metadata out of the durable stream.
    if(strcmp(event,"ERROR") && strcmp(event,"GET_CONFIG") &&
       strcmp(event,"READ_REQUEST") && strcmp(event,"WRITE_METADATA") &&
       !(strcmp(event,"IOCTL")==0 && b==GET_CONFIG))
        bmj::diagnostic::event(event,a,b,c);
    if(!traceEnabled || traceCount.load()>=512 || traceCount.fetch_add(1)>=512) return;
    char line[256]={};
    snprintf(line,sizeof(line),"BMJ2534 pid=%lu %s a=0x%lX b=0x%lX c=0x%lX\n",GetCurrentProcessId(),event,a,b,c);
    OutputDebugStringA(line);
}
bool callerPacking(void* address,const char* operation) {
    char path[MAX_PATH]={};
    const char* name=bmj::detail::callerName(address,path);
    if(!name) return true;
    if(traceEnabled && traceCount.load()<512 && traceCount.fetch_add(1)<512) {
        char line[512]={};
        const unsigned packing=1;
        snprintf(line,sizeof(line),"BMJ2534 pid=%lu %s caller=%s packing=%u\n",GetCurrentProcessId(),operation,name,packing);
        OutputDebugStringA(line);
    }
    return bmj::detail::compatiblePacking(name);
}
long fail(long code, const char* text,bool record=true) {
    strncpy_s(lastError, text, _TRUNCATE);
    if(record) {bmj::diagnostic::error(code,text);trace("ERROR",static_cast<U>(code));}
    return code;
}
long nativeCode(BM_StatusTypeDef code) {
    if (code == BM_ERROR_OK) return STATUS_NOERROR;
    // Some legacy BMAPI handle errors are composite values, not independent bits.
    if (code == BM_ERROR_HWINUSE) return ERR_DEVICE_IN_USE;
    if (code == BM_ERROR_ILLHW || code == BM_ERROR_ILLCLIENT || code == BM_ERROR_ILLNET || code == BM_ERROR_NODRIVER || code == BM_ERROR_INITIALIZE) return ERR_DEVICE_NOT_CONNECTED;
    if (code & (BM_ERROR_QXMTFULL | BM_ERROR_XMTFULL)) return ERR_BUFFER_FULL;
    if (code & (BM_ERROR_BUSOFF | BM_ERROR_BUSTIMEOUT)) return ERR_TIMEOUT;
    if (code & BM_ERROR_QOVERRUN) return ERR_BUFFER_OVERFLOW;
    if (code & BM_ERROR_RESOURCE) return ERR_EXCEEDED_LIMIT;
    if (code & BM_ERROR_ILLOPERATION) return ERR_NOT_SUPPORTED;
    if (code & (BM_ERROR_ILLPARAMVAL | BM_ERROR_CONFIG)) return ERR_INVALID_IOCTL_VALUE;
    return ERR_FAILED;
}
long nativeFailure(BM_StatusTypeDef code, const char* operation) {
    char text[80] = {};
    snprintf(text, sizeof(text), "%s: BMAPI status 0x%lX", operation, static_cast<U>(code));
    return fail(nativeCode(code), text);
}
bool rawProtocol(U protocol) { return protocol == CAN || protocol == CAN_PS || protocol == FD_CAN_PS || protocol == CAN_FD_PS; }
bool isoProtocol(U protocol) { return protocol==ISO15765 || protocol==ISO15765_PS || protocol==FD_ISO15765_PS || protocol==ISO15765_FD_PS; }
bool fdProtocol(U protocol) { return protocol == FD_CAN_PS || protocol == CAN_FD_PS || protocol == FD_ISO15765_PS || protocol == ISO15765_FD_PS; }
bool gmProtocol(U protocol) { return protocol == CAN_FD_PS || protocol == ISO15765_FD_PS; }
U fdFormat(U protocol) { return gmProtocol(protocol) ? CAN_FD_FORMAT : FD_CAN_FORMAT; }
U fdBrs(U protocol) { return gmProtocol(protocol) ? CAN_FD_BRS : FD_CAN_BRS; }
U fdEsi(U protocol) { return gmProtocol(protocol) ? CAN_FD_ESI : FD_CAN_ESI; }
const uint8_t lengths[] = {0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
int dlc(size_t bytes) {
    for (unsigned i=0; i<_countof(lengths); ++i) if (bytes==lengths[i]) return static_cast<int>(i);
    return -1;
}
uint32_t canId(const PASSTHRU_MSG& msg) {
    return static_cast<uint32_t>(msg.Data[0])<<24 | static_cast<uint32_t>(msg.Data[1])<<16 |
        static_cast<uint32_t>(msg.Data[2])<<8 | msg.Data[3];
}
void putId(PASSTHRU_MSG& msg, uint32_t id) {
    msg.Data[0]=static_cast<uint8_t>(id>>24); msg.Data[1]=static_cast<uint8_t>(id>>16);
    msg.Data[2]=static_cast<uint8_t>(id>>8); msg.Data[3]=static_cast<uint8_t>(id);
}
struct Job {
    PASSTHRU_MSG message = {};
    U periodic = 0;
    bool submitted = false, complete = false;
    long result = STATUS_NOERROR;
};
struct Filter { U type; PASSTHRU_MSG mask, pattern; };
struct Periodic { PASSTHRU_MSG message; U interval; Clock::time_point next; };
struct Device;
struct Channel {
    U id=0, protocol=0, flags=0, rate=500000, dataRate=500000, loopback=0, pins=0, termination=0, samplePoint=75;
    uint8_t nextSequence=0;
    bool connected=false, stopped=false, overflow=false, busOffReported=false;
    bool recoveryAttempted=false, recoveryFailed=false;
    Clock::time_point recoveryReadyAt={};
    std::mutex mutex;
    std::condition_variable changed;
    std::weak_ptr<Device> device;
    std::thread thread;
    std::deque<PASSTHRU_MSG> received;
    std::deque<std::shared_ptr<Job>> transmit;
    std::shared_ptr<Job> active;
    std::shared_ptr<Job> periodicActive;
    uint8_t activeSequence=0;
    Clock::time_point submittedAt, nextStatus;
    std::map<U,Filter> filters;
    std::map<U,Periodic> periodic;
    bmj::IsoTransport iso;
    bmj::IsoTransport periodicIso;
    bool wireSubmitted=false, periodicWire=false;
    U pendingGmErrors=0;
    uint32_t pendingGmErrorTimestamp=0;
    uint64_t nativeReads=0,nativeWrites=0,completedJobs=0,failedJobs=0;
    U lastNativeError=0;
    Clock::time_point nextHealth={};
};
struct Device {
    U id=0;
    BM_ChannelInfoTypeDef info={};
    BM_ChannelHandle handle=nullptr;
    bool defaultTermination=false;
    bool customSampling=false, classicSampling=false;
    U nominalSample=75,dataSample=80;
    HMODULE selfReference=nullptr;
    std::atomic<bool> closing{false}, lost{false};
    std::mutex mutex;
    std::shared_ptr<Channel> channel;
    // A named kernel object is a process-lifetime lease, not a thread-owned
    // mutex. Ports of one physical device share it inside this provider.
    struct Lease { HANDLE handle=nullptr; ~Lease() { if(handle) CloseHandle(handle); } };
    std::shared_ptr<Lease> lease;
    bmj::diagnostic::Session diagnostics;
};
struct State {
    std::mutex mutex;
    bmj::Api api;
    U nextId=1;
    std::map<U,std::shared_ptr<Device>> devices;
    std::map<U,std::shared_ptr<Channel>> channels;
};
State*& stateSlot() { static State* instance=nullptr; return instance; }
State& state() {
    // Lifetime is explicit through PassThruClose, never a loader-lock destructor.
    static State* instance=(stateSlot()=new State);
    return *instance;
}
bool noDevices() {
    auto& value=state(); std::lock_guard<std::mutex> lock(value.mutex);
    return value.devices.empty();
}
struct UnusedApiCleanup {
    State& value;
    ~UnusedApiCleanup() { if(value.devices.empty()) value.api.unload(); }
};
U allocateId(State& value) {
    if (!value.nextId || value.nextId==0xFFFFFFFFUL) return 0;
    return value.nextId++;
}
std::shared_ptr<Device> device(U id) {
    auto& value=state(); std::lock_guard<std::mutex> lock(value.mutex);
    auto found=value.devices.find(id);
    return found==value.devices.end() ? nullptr : found->second;
}
std::shared_ptr<Channel> channel(U id) {
    auto& value=state(); std::lock_guard<std::mutex> lock(value.mutex);
    auto found=value.channels.find(id);
    return found==value.channels.end() ? nullptr : found->second;
}
long available(Channel& value, const std::shared_ptr<Device>& owner) {
    if (!owner || owner->lost) return fail(ERR_DEVICE_NOT_CONNECTED,"Device connection was lost; close and reopen");
    if (value.stopped || owner->closing) return fail(ERR_INVALID_CHANNEL_ID,"Channel is disconnected");
    return STATUS_NOERROR;
}
void queueReceived(Channel& value, const PASSTHRU_MSG& msg) {
    if (value.received.size()==RX_LIMIT) {
        value.overflow=true;
        if(gmProtocol(value.protocol)) {
            const U code=msg.RxStatus==ERROR_IND && msg.DataSize==4 ? msg.Data[3] : 1;
            if(code>=1 && code<=4) value.pendingGmErrors|=1UL<<code;
            value.pendingGmErrorTimestamp=msg.Timestamp;
        }
        value.changed.notify_all(); return;
    }
    value.received.push_back(msg);
    value.changed.notify_all();
}
void gmError(Channel& value,U code,uint32_t timestamp) {
    if(!gmProtocol(value.protocol)) return;
    PASSTHRU_MSG indication={}; indication.ProtocolID=value.protocol; indication.RxStatus=ERROR_IND;
    indication.DataSize=4; indication.Timestamp=timestamp; putId(indication,code);
    queueReceived(value,indication);
}
bool matches(const Filter& filter, const PASSTHRU_MSG& msg) {
    if ((filter.pattern.TxFlags & CAN_29BIT_ID) != (msg.RxStatus & CAN_29BIT_ID)) return false;
    if (msg.DataSize<filter.pattern.DataSize) return false;
    for (U i=0; i<filter.pattern.DataSize; ++i) {
        if ((msg.Data[i]&filter.mask.Data[i]) != (filter.pattern.Data[i]&filter.mask.Data[i])) return false;
    }
    return true;
}
bool accepted(Channel& value, const PASSTHRU_MSG& msg) {
    bool pass=false;
    for (const auto& entry:value.filters) {
        if (matches(entry.second,msg)) {
            if (entry.second.type==BLOCK_FILTER) return false;
            if (entry.second.type==PASS_FILTER) pass=true;
        }
    }
    return pass;
}
void finish(Channel& value, const std::shared_ptr<Job>& job, long result, uint32_t stamp) {
    if(result==STATUS_NOERROR)++value.completedJobs;else ++value.failedJobs;
    job->complete=true; job->result=result;
    if(result==STATUS_NOERROR && isoProtocol(value.protocol)) {
        PASSTHRU_MSG indication=job->message;
        indication.Timestamp=stamp;
        indication.RxStatus=TX_MSG_TYPE|TX_INDICATION|(indication.TxFlags&(CAN_29BIT_ID|ISO15765_ADDR_TYPE));
        if(fdProtocol(value.protocol) && (indication.TxFlags&fdFormat(value.protocol))) indication.RxStatus|=indication.TxFlags&(fdFormat(value.protocol)|fdBrs(value.protocol));
        indication.TxFlags=0; indication.DataSize=4+((indication.RxStatus&ISO15765_ADDR_TYPE)!=0); indication.ExtraDataIndex=0;
        queueReceived(value,indication);
    }
    if (result==STATUS_NOERROR && value.loopback) {
        PASSTHRU_MSG echo=job->message;
        echo.Timestamp=stamp; echo.RxStatus=TX_MSG_TYPE | (echo.TxFlags & (CAN_29BIT_ID|ISO15765_ADDR_TYPE));
        if (fdProtocol(value.protocol) && (echo.TxFlags&fdFormat(value.protocol))) echo.RxStatus |= echo.TxFlags & (fdFormat(value.protocol)|fdBrs(value.protocol));
        echo.TxFlags=0; echo.ExtraDataIndex=echo.DataSize;
        queueReceived(value,echo);
    }
    value.changed.notify_all();
}
void cancelTransmit(Channel& value, long result) {
    value.iso.clearTx(); value.periodicIso.clearTx(); value.wireSubmitted=false; value.periodicWire=false;
    if(value.active) finish(value,value.active,result,0);
    value.active.reset();
    if(value.periodicActive) finish(value,value.periodicActive,result,0);
    value.periodicActive.reset();
    for(auto& job:value.transmit) finish(value,job,result,0);
    value.transmit.clear();
}
void recoverBus(Channel& value,const std::shared_ptr<Device>& owner) {
    // All native operations for this port run under Channel::mutex. Recovery
    // therefore has one owner and never affects another Device/Channel object.
    if(value.recoveryFailed) return;
    if(value.recoveryAttempted) {
        value.recoveryFailed=true;
        cancelTransmit(value,ERR_TIMEOUT);
        trace("BUSOFF_PERSISTENT",value.id);
        return;
    }
    value.recoveryAttempted=true;
    const BM_StatusTypeDef result=state().api.recoverBusOff(owner->handle);
    trace("BUSOFF_RECOVERY",value.id,static_cast<U>(result));
    // Recovery may cancel native traffic (especially the old-firmware
    // fallback). Fail affected jobs honestly; never fabricate completion or
    // resume half an ISO-TP transfer. Subsequent requests may use the recovered port.
    cancelTransmit(value,result==BM_ERROR_OK?ERR_TIMEOUT:nativeCode(result));
    value.iso.clearRx(false);value.periodicIso.clearRx(false);
    if(result==BM_ERROR_OK) {
        value.recoveryReadyAt=Clock::now()+std::chrono::milliseconds(250);
    } else {
        value.recoveryFailed=true;
        if(nativeCode(result)==ERR_DEVICE_NOT_CONNECTED)owner->lost=true;
    }
    value.changed.notify_all();
}
long cancelPeriodicJobs(Channel& value,const std::shared_ptr<Device>& owner,U id) {
    const bool iso=isoProtocol(value.protocol);
    auto& active=iso ? value.periodicActive : value.active;
    if(active && active->periodic && (!id || active->periodic==id)) {
        const bool submitted=iso ? (value.wireSubmitted && value.periodicWire) : active->submitted;
        if(submitted) {
            const auto result=state().api.clearDirection(owner->handle,BM_TXQ_BUFFER);
            if(result!=BM_ERROR_OK) return nativeFailure(result,"Cancel periodic transmission");
            value.wireSubmitted=false; value.periodicWire=false;
        }
        if(iso) value.periodicIso.cancelSend();
        finish(value,active,ERR_INVALID_MSG_ID,0); active.reset();
    }
    value.transmit.erase(std::remove_if(value.transmit.begin(),value.transmit.end(),
        [id](const std::shared_ptr<Job>& job){return job->periodic && (!id || job->periodic==id);}),value.transmit.end());
    return STATUS_NOERROR;
}
long validateMessage(const Channel& value, const PASSTHRU_MSG& msg) {
    if(msg.ProtocolID!=value.protocol) return fail(ERR_MSG_PROTOCOL_ID,"Message protocol differs from the channel");
    const bool iso=isoProtocol(value.protocol);
    const U maximum=iso && !fdProtocol(value.protocol)
        ? 4UL+((msg.TxFlags&ISO15765_ADDR_TYPE)!=0)+bmj::ISO_CLASSIC_MAX_PDU
        : (iso ? 4128UL : 68UL);
    if(msg.DataSize<4 || msg.DataSize>maximum) return fail(ERR_INVALID_MSG,"Invalid CAN message size");
    U allowed=CAN_29BIT_ID;
    if(iso) allowed|=ISO15765_ADDR_TYPE|ISO15765_FRAME_PAD;
    if(fdProtocol(value.protocol)) allowed |= fdFormat(value.protocol)|fdBrs(value.protocol);
    if(msg.TxFlags & ~allowed) return fail(ERR_INVALID_MSG,"Unsupported CAN transmit flags");
    const bool extended=(msg.TxFlags & CAN_29BIT_ID)!=0;
    if(!(value.flags & CAN_ID_BOTH) && extended!=((value.flags & CAN_29BIT_ID)!=0))
        return fail(ERR_INVALID_MSG,"Message identifier format differs from the channel");
    if(canId(msg)>(extended ? 0x1FFFFFFFUL : 0x7FFUL)) return fail(ERR_INVALID_MSG,"CAN identifier out of range");
    if(iso) {
        if(msg.DataSize<=4UL+((msg.TxFlags&ISO15765_ADDR_TYPE)!=0)) return fail(ERR_INVALID_MSG,"Missing ISO-TP payload");
        return value.iso.validate(msg);
    }
    const U size=msg.DataSize-4;
    const bool fd=fdProtocol(value.protocol) && (msg.TxFlags & fdFormat(value.protocol));
    if((!fd && size>8) || dlc(size)<0) return fail(ERR_INVALID_MSG,"Invalid frame payload length");
    return STATUS_NOERROR;
}
void receiveFrame(Channel& value, const BM_DataTypeDef& data) {
    if((data.header.type & ~BM_ACK_DATA)!=BM_CAN_FD_DATA || data.length<8) return;
    const auto* frame=reinterpret_cast<const BM_CanMessageTypeDef*>(data.payload);
    if(data.header.type & BM_ACK_DATA) {
        if(isoProtocol(value.protocol)) {
            if(value.wireSubmitted && frame->ctrl.tx.SEQ==value.activeSequence) {
                value.recoveryAttempted=false;
                (value.periodicWire ? value.periodicIso : value.iso).acknowledged(data.timestamp);
                value.wireSubmitted=false; value.periodicWire=false;
            }
            return;
        }
        if(value.active && value.active->submitted && frame->ctrl.tx.SEQ==value.activeSequence) {
            value.recoveryAttempted=false;
            finish(value,value.active,STATUS_NOERROR,data.timestamp);
            value.active.reset();
        }
        return;
    }
    if(!value.connected) return;
    if(frame->ctrl.rx.RTR || (!fdProtocol(value.protocol) && frame->ctrl.rx.FDF)) return;
    if(!(value.flags & CAN_ID_BOTH) && (static_cast<bool>(frame->ctrl.rx.IDE)!=((value.flags & CAN_29BIT_ID)!=0))) return;
    const U bytes=lengths[frame->ctrl.rx.DLC];
    if(data.length<8+bytes) { value.overflow=true; return; }
    PASSTHRU_MSG msg={};
    msg.ProtocolID=value.protocol; msg.Timestamp=data.timestamp;
    msg.DataSize=4+bytes; msg.ExtraDataIndex=msg.DataSize;
    msg.RxStatus=frame->ctrl.rx.IDE ? CAN_29BIT_ID : 0;
    if(fdProtocol(value.protocol) && frame->ctrl.rx.FDF) {
        msg.RxStatus |= fdFormat(value.protocol);
        if(frame->ctrl.rx.BRS) msg.RxStatus |= fdBrs(value.protocol);
        if(frame->ctrl.rx.ESI) msg.RxStatus |= fdEsi(value.protocol);
    }
    putId(msg,frame->ctrl.rx.IDE ? BM_GET_EXT_MSG_ID(frame->id) : BM_GET_STD_MSG_ID(frame->id));
    memcpy(msg.Data+4,frame->payload,bytes);
    if(isoProtocol(value.protocol)) value.iso.receive(msg,data.timestamp);
    else if(accepted(value,msg)) queueReceived(value,msg);
}
void worker(std::shared_ptr<Channel> value) {
    bmj::diagnostic::ApiScope diagnosticScope("worker",value->id);
    auto owner=value->device.lock();
    try {
        std::unique_lock<std::mutex> lock(value->mutex);
        while(!value->stopped && owner && !owner->lost && !owner->closing) {
            bool activity=false;
            for(unsigned i=0; value->connected && i<128; ++i) {
                BM_DataTypeDef data={};
                const BM_StatusTypeDef result=state().api.read(owner->handle,&data);
                if(result==BM_ERROR_QRCVEMPTY) break;
                if(result!=BM_ERROR_OK) {
                    if(value->lastNativeError!=static_cast<U>(result))
                        bmj::diagnostic::event(owner->diagnostics,"NATIVE_READ_ERROR",value->id,static_cast<U>(result));
                    value->lastNativeError=static_cast<U>(result);
                    if(nativeCode(result)==ERR_DEVICE_NOT_CONNECTED) owner->lost=true;
                    else value->overflow=true;
                    break;
                }
                ++value->nativeReads;receiveFrame(*value,data); activity=true;
            }
            const auto now=Clock::now();
            if(value->connected && !value->recoveryFailed && now>=value->recoveryReadyAt) {
                for(auto& item:value->periodic) {
                    if(now<item.second.next) continue;
                    bool pending=(value->active && value->active->periodic==item.first) ||
                        (value->periodicActive && value->periodicActive->periodic==item.first);
                    for(const auto& job:value->transmit) pending=pending || job->periodic==item.first;
                    if(!pending && value->transmit.size()<TX_LIMIT) {
                        auto job=std::make_shared<Job>(); job->message=item.second.message; job->periodic=item.first;
                        // Preserve arrival order with ordinary writes. A late
                        // cycle is coalesced below rather than jumping the queue.
                        value->transmit.push_back(job);
                    }
                    item.second.next+=std::chrono::milliseconds(item.second.interval);
                    if(item.second.next<=now) item.second.next=now+std::chrono::milliseconds(item.second.interval);
                }
                if(isoProtocol(value->protocol) && !value->periodicActive) {
                    auto job=std::find_if(value->transmit.begin(),value->transmit.end(),[](const std::shared_ptr<Job>& item){return item->periodic!=0;});
                    if(job!=value->transmit.end()) { value->periodicActive=*job; value->transmit.erase(job); }
                }
                if(!value->active) {
                    auto job=isoProtocol(value->protocol) ?
                        std::find_if(value->transmit.begin(),value->transmit.end(),[](const std::shared_ptr<Job>& item){return item->periodic==0;}) : value->transmit.begin();
                    if(job!=value->transmit.end()) {
                        value->active=*job; value->transmit.erase(job);
                        if(!isoProtocol(value->protocol)) value->activeSequence=value->nextSequence++;
                        value->submittedAt=now;
                    }
                }
                if(isoProtocol(value->protocol)) {
                    if(value->active && !value->active->submitted) {
                        const long result=value->iso.start(value->active->message);
                        if(result==STATUS_NOERROR) value->active->submitted=true;
                        else if(result!=ERR_BUFFER_FULL) { finish(*value,value->active,result,0); value->active.reset(); }
                    }
                    if(value->periodicActive && !value->periodicActive->submitted) {
                        value->periodicIso.settings=value->iso.settings;
                        const long result=value->periodicIso.start(value->periodicActive->message);
                        if(result==STATUS_NOERROR) value->periodicActive->submitted=true;
                        else if(result!=ERR_BUFFER_FULL) { finish(*value,value->periodicActive,result,0); value->periodicActive.reset(); }
                    }
                    value->iso.poll();
                    value->periodicIso.poll();
                    while(!value->iso.received.empty()) {
                        queueReceived(*value,value->iso.received.front()); value->iso.received.pop_front();
                    }
                    if(value->iso.overflow) { value->overflow=true; value->iso.overflow=false; }
                    const auto collectResult=[&](bmj::IsoTransport& transport,std::shared_ptr<Job>& job) {
                        long result=STATUS_NOERROR; uint32_t stamp=0;
                        if(transport.result(result,stamp) && job) {
                            if(result==ERR_TIMEOUT || result==ERR_FAILED) {
                                uint32_t occurred=0; if(state().api.timestamp(owner->handle,&occurred)==BM_ERROR_OK) gmError(*value,4,occurred);
                            }
                            finish(*value,job,result,stamp); job.reset();
                        }
                    };
                    collectResult(value->iso,value->active); collectResult(value->periodicIso,value->periodicActive);
                    const bool periodicOutgoing=value->periodicIso.front()!=nullptr;
                    auto& outgoingTransport=periodicOutgoing ? value->periodicIso : value->iso;
                    const auto* outgoing=outgoingTransport.front();
                    if(outgoing && outgoing->flow && !value->wireSubmitted &&
                       IsoTpTimeAfter(isotp_user_get_us(),outgoing->confirmDeadline)) {
                        outgoingTransport.failed(ERR_TIMEOUT); outgoing=nullptr;
                    }
                    if(outgoing && !value->wireSubmitted) {
                        BM_DataTypeDef data={}; data.header.type=BM_CAN_FD_DATA; data.length=sizeof(BM_CanMessageTypeDef);
                        auto* frame=reinterpret_cast<BM_CanMessageTypeDef*>(data.payload);
                        frame->ctrl.tx.IDE=(outgoing->flags&CAN_29BIT_ID)!=0;
                        frame->ctrl.tx.FDF=fdProtocol(value->protocol) && (outgoing->flags&fdFormat(value->protocol));
                        frame->ctrl.tx.BRS=frame->ctrl.tx.FDF && (outgoing->flags&fdBrs(value->protocol));
                        frame->ctrl.tx.DLC=dlc(outgoing->size);
                        frame->ctrl.tx.SEQ=value->nextSequence;
                        BM_SET_CAN_MSG_ID(*frame,outgoing->id); memcpy(frame->payload,outgoing->data,outgoing->size);
                        const BM_StatusTypeDef written=state().api.write(owner->handle,&data,0,nullptr);
                        if(written!=BM_ERROR_OK && nativeCode(written)!=ERR_BUFFER_FULL && value->lastNativeError!=static_cast<U>(written)) {
                            bmj::diagnostic::event(owner->diagnostics,"NATIVE_WRITE_ERROR",value->id,static_cast<U>(written));value->lastNativeError=static_cast<U>(written);
                        }
                        if(written==BM_ERROR_OK) {
                            ++value->nativeWrites;
                            value->activeSequence=value->nextSequence++; value->wireSubmitted=true; value->periodicWire=periodicOutgoing;
                            outgoingTransport.submitted(); value->submittedAt=now; activity=true;
                        } else if(written & (BM_ERROR_BUSOFF|BM_ERROR_BUSTIMEOUT)) {
                            recoverBus(*value,owner);
                        } else if(nativeCode(written)!=ERR_BUFFER_FULL && nativeCode(written)!=ERR_TIMEOUT) {
                            if(nativeCode(written)==ERR_DEVICE_NOT_CONNECTED) owner->lost=true;
                            outgoingTransport.failed(nativeCode(written));
                        }
                    }
                    const auto* submitted=(value->periodicWire ? value->periodicIso : value->iso).front();
                    const bool flowExpired=submitted && submitted->flow &&
                        IsoTpTimeAfter(isotp_user_get_us(),submitted->confirmDeadline);
                    if(value->wireSubmitted && (flowExpired || now-value->submittedAt>=std::chrono::seconds(1))) {
                        const auto cleared=state().api.clearDirection(owner->handle,BM_TXQ_BUFFER);
                        if(cleared!=BM_ERROR_OK) owner->lost=true;
                        (value->periodicWire ? value->periodicIso : value->iso).failed(ERR_TIMEOUT);
                        value->wireSubmitted=false; value->periodicWire=false;
                    }
                } else if(value->active && !value->active->submitted) {
                    const PASSTHRU_MSG& msg=value->active->message;
                    BM_DataTypeDef data={}; data.header.type=BM_CAN_FD_DATA; data.length=sizeof(BM_CanMessageTypeDef);
                    auto* frame=reinterpret_cast<BM_CanMessageTypeDef*>(data.payload);
                    frame->ctrl.tx.IDE=(msg.TxFlags & CAN_29BIT_ID)!=0;
                    frame->ctrl.tx.FDF=fdProtocol(value->protocol) && (msg.TxFlags & fdFormat(value->protocol));
                    frame->ctrl.tx.BRS=frame->ctrl.tx.FDF && (msg.TxFlags & fdBrs(value->protocol));
                    frame->ctrl.tx.DLC=dlc(msg.DataSize-4); frame->ctrl.tx.SEQ=value->activeSequence;
                    BM_SET_CAN_MSG_ID(*frame,canId(msg));
                    memcpy(frame->payload,msg.Data+4,msg.DataSize-4);
                    const BM_StatusTypeDef result=state().api.write(owner->handle,&data,0,nullptr);
                    if(result!=BM_ERROR_OK && nativeCode(result)!=ERR_BUFFER_FULL && value->lastNativeError!=static_cast<U>(result)) {
                        bmj::diagnostic::event(owner->diagnostics,"NATIVE_WRITE_ERROR",value->id,static_cast<U>(result));value->lastNativeError=static_cast<U>(result);
                    }
                    if(result==BM_ERROR_OK) { ++value->nativeWrites;value->active->submitted=true; activity=true; }
                    else if(result & (BM_ERROR_BUSOFF|BM_ERROR_BUSTIMEOUT)) {
                        recoverBus(*value,owner);
                    }
                    else if(nativeCode(result)!=ERR_BUFFER_FULL && nativeCode(result)!=ERR_TIMEOUT) {
                        const long error=nativeCode(result);
                        if(error==ERR_DEVICE_NOT_CONNECTED) owner->lost=true;
                        finish(*value,value->active,error,0); value->active.reset();
                    }
                }
            }
            if(now>=value->nextStatus) {
                BM_CanStatusInfoTypeDef status={};
                const BM_StatusTypeDef result=state().api.status(owner->handle,&status);
                value->nextStatus=now+std::chrono::milliseconds(100);
                // BM_GetCanStatus wraps a failed control query in ILLOPERATION;
                // bus-state flags instead accompany a valid status structure.
                const bool statusAvailable=!(result & BM_ERROR_ILLOPERATION) && nativeCode(result)!=ERR_DEVICE_NOT_CONNECTED;
                if(result!=BM_ERROR_OK) {
                    if(value->lastNativeError!=static_cast<U>(result))
                        bmj::diagnostic::event(owner->diagnostics,"NATIVE_STATUS_ERROR",value->id,static_cast<U>(result));
                    value->lastNativeError=static_cast<U>(result);
                }
                if(!statusAvailable) owner->lost=true;
                if(statusAvailable && status.TXBO && value->connected && gmProtocol(value->protocol) && !value->busOffReported) {
                    PASSTHRU_MSG indication={}; indication.ProtocolID=value->protocol; indication.RxStatus=ERROR_IND;
                    indication.DataSize=4; indication.ExtraDataIndex=0; putId(indication,2);
                    uint32_t stamp=0; if(state().api.timestamp(owner->handle,&stamp)==BM_ERROR_OK) indication.Timestamp=stamp;
                    queueReceived(*value,indication); value->busOffReported=true;
                }
                if(statusAvailable && !status.TXBO) value->busOffReported=false;
                if(statusAvailable && status.TXBO && value->connected && !value->recoveryFailed &&
                   (!value->recoveryAttempted || now>=value->recoveryReadyAt)) recoverBus(*value,owner);
            }
            if(now>=value->nextHealth) {
                char detail[256]={};
                snprintf(detail,sizeof(detail),"native_rx_records=%llu native_tx_submissions=%llu completed_jobs=%llu failed_jobs=%llu rx_queue=%zu tx_queue=%zu overflow=%u lost=%u",
                    static_cast<unsigned long long>(value->nativeReads),static_cast<unsigned long long>(value->nativeWrites),
                    static_cast<unsigned long long>(value->completedJobs),static_cast<unsigned long long>(value->failedJobs),
                    value->received.size(),value->transmit.size(),value->overflow?1:0,owner->lost?1:0);
                bmj::diagnostic::event(owner->diagnostics,"CHANNEL_HEALTH",value->id,value->protocol,value->lastNativeError,detail);
                value->nextHealth=now+std::chrono::seconds(30);
            }
            if(owner->lost) break;
            if(!activity) value->changed.wait_for(lock,std::chrono::milliseconds(1));
            else { lock.unlock(); std::this_thread::yield(); lock.lock(); }
        }
        cancelTransmit(*value,owner && owner->lost ? ERR_DEVICE_NOT_CONNECTED : ERR_INVALID_CHANNEL_ID);
        value->changed.notify_all();
    } catch(...) {
        bmj::diagnostic::event(owner ? owner->diagnostics : bmj::diagnostic::Session(),"WORKER_EXCEPTION",value->id);
        std::lock_guard<std::mutex> lock(value->mutex);
        if(owner) owner->lost=true;
        cancelTransmit(*value,ERR_FAILED); value->changed.notify_all();
    }
}
void stop(const std::shared_ptr<Channel>& value) {
    { std::lock_guard<std::mutex> lock(value->mutex); value->stopped=true; value->changed.notify_all(); }
    if(value->thread.joinable()) value->thread.join();
    const auto owner=value->device.lock();
    char detail[192]={};
    snprintf(detail,sizeof(detail),"native_rx_records=%llu native_tx_submissions=%llu completed_jobs=%llu failed_jobs=%llu",
        static_cast<unsigned long long>(value->nativeReads),static_cast<unsigned long long>(value->nativeWrites),
        static_cast<unsigned long long>(value->completedJobs),static_cast<unsigned long long>(value->failedJobs));
    bmj::diagnostic::event(owner ? owner->diagnostics : bmj::diagnostic::Session(),"CHANNEL_STOPPED",value->id,value->protocol,value->lastNativeError,detail);
}
bool validRate(U rate) { return rate>=1000 && rate%1000==0 && rate/1000<=65535; }
long apply(Channel& value, const std::shared_ptr<Device>& owner, U rate, U dataRate, U term, bool connect, U samplePoint=75) {
    if(!validRate(rate) || !validRate(dataRate)) return fail(ERR_INVALID_BAUDRATE,"Bit rate cannot be represented by BMAPI");
    auto& api=state().api;
    BM_BitrateTypeDef previousRate={};
    BM_TerminalResistorTypeDef previousTerm=BM_TRESISTOR_DISABLED;
    BM_CanModeTypeDef previousMode=BM_CAN_OFF_MODE;
    BM_StatusTypeDef previous=api.getBitrate(owner->handle,&previousRate);
    if(previous==BM_ERROR_OK) previous=api.getTermination(owner->handle,&previousTerm);
    if(previous==BM_ERROR_OK) previous=api.getMode(owner->handle,&previousMode);
    if(previous!=BM_ERROR_OK) return nativeFailure(previous,"Read configuration before update");
    const bool custom=fdProtocol(value.protocol) && owner->customSampling;
    const U nominal=custom ? owner->nominalSample : samplePoint;
    const U data=fdProtocol(value.protocol) ? (custom ? owner->dataSample : 80) : 80;
    BM_BitrateTypeDef setting={}; setting.nbitrate=static_cast<uint16_t>(rate/1000);
    setting.dbitrate=static_cast<uint16_t>(dataRate/1000); setting.nsamplepos=static_cast<uint8_t>(nominal); setting.dsamplepos=static_cast<uint8_t>(data);
    BM_StatusTypeDef result=api.bitrate(owner->handle,&setting);
    if(result==BM_ERROR_OK) result=api.termination(owner->handle,term ? BM_TRESISTOR_120 : BM_TRESISTOR_DISABLED);
    if(result==BM_ERROR_OK) result=api.mode(owner->handle,connect ? BM_CAN_NORMAL_MODE : BM_CAN_OFF_MODE);
    BM_BitrateTypeDef applied={};
    bool readback=false;
    if(result==BM_ERROR_OK) { result=api.getBitrate(owner->handle,&applied);readback=result==BM_ERROR_OK; }
    if(result==BM_ERROR_OK && (applied.nbitrate!=setting.nbitrate || applied.dbitrate!=setting.dbitrate ||
        std::abs(static_cast<int>(applied.nsamplepos)-setting.nsamplepos)>2 ||
        std::abs(static_cast<int>(applied.dsamplepos)-setting.dsamplepos)>2)) result=BM_ERROR_CONFIG;
    if(result!=BM_ERROR_OK) {
        char detail[224]={};
        snprintf(detail,sizeof(detail),"sample_source=%s requested_nominal=%lu requested_data=%lu readback_valid=%u actual_nominal=%u actual_data=%u native_status=%lu",
            custom ? "Config" : "default_or_J2534",nominal,data,readback?1:0,applied.nsamplepos,applied.dsamplepos,static_cast<U>(result));
        bmj::diagnostic::event(owner->diagnostics,"CONFIG_REJECTED",rate,dataRate,term,detail);
        const BM_StatusTypeDef restoreRate=api.bitrate(owner->handle,&previousRate);
        const BM_StatusTypeDef restoreTerm=api.termination(owner->handle,previousTerm);
        const BM_StatusTypeDef restoreMode=api.mode(owner->handle,previousMode);
        BM_BitrateTypeDef restoredRate={};
        BM_TerminalResistorTypeDef restoredTerm=BM_TRESISTOR_DISABLED;
        BM_CanModeTypeDef restoredMode=BM_CAN_OFF_MODE;
        if(restoreRate!=BM_ERROR_OK || restoreTerm!=BM_ERROR_OK || restoreMode!=BM_ERROR_OK ||
           api.getBitrate(owner->handle,&restoredRate)!=BM_ERROR_OK || api.getTermination(owner->handle,&restoredTerm)!=BM_ERROR_OK ||
           api.getMode(owner->handle,&restoredMode)!=BM_ERROR_OK || restoredRate.nbitrate!=previousRate.nbitrate ||
           restoredRate.dbitrate!=previousRate.dbitrate || restoredRate.nsamplepos!=previousRate.nsamplepos ||
           restoredRate.dsamplepos!=previousRate.dsamplepos || restoredTerm!=previousTerm || restoredMode!=previousMode) {
            owner->lost=true;
            return fail(ERR_DEVICE_NOT_CONNECTED,"Configuration rollback failed; close and reopen");
        }
        return nativeFailure(result,"Apply CAN configuration");
    }
    value.rate=rate; value.dataRate=dataRate; value.termination=term; value.connected=connect; value.samplePoint=fdProtocol(value.protocol) ? samplePoint : applied.nsamplepos;
    value.recoveryAttempted=false;value.recoveryFailed=false;value.recoveryReadyAt=Clock::time_point();
    char detail[192]={};
    snprintf(detail,sizeof(detail),"nominal_sample=%u data_sample=%u can_enabled=%u sample_source=%s requested_nominal=%lu requested_data=%lu",
        applied.nsamplepos,applied.dsamplepos,connect?1:0,custom ? "Config" : "default_or_J2534",nominal,data);
    bmj::diagnostic::event(owner->diagnostics,"CONFIG_APPLIED",rate,dataRate,term,detail);
    return STATUS_NOERROR;
}
long configure(Channel& value, const std::shared_ptr<Device>& owner, SCONFIG_LIST* list, bool set) {
    if(!list || (list->NumOfParams && !list->ConfigPtr)) return fail(ERR_NULL_PARAMETER,"Missing SCONFIG_LIST");
    if(list->NumOfParams>64) return fail(ERR_EXCEEDED_LIMIT,"Too many configuration parameters");
    U rate=value.rate, dataRate=value.dataRate, term=value.termination, pins=value.pins, loop=value.loopback, samplePoint=value.samplePoint;
    auto iso=value.iso.settings;
    bool hardware=false;
    for(U i=0; i<list->NumOfParams; ++i) {
        auto& item=list->ConfigPtr[i];
        U current=0;
        if(item.Parameter==DATA_RATE) { current=rate; if(set) { rate=item.Value; hardware=true; } }
        else if(item.Parameter==BIT_SAMPLE_POINT && (value.protocol==CAN || value.protocol==CAN_PS)) {
            current=samplePoint;
            if(set) { if(item.Value>100) return fail(ERR_INVALID_IOCTL_VALUE,"BIT_SAMPLE_POINT must be 0..100 percent"); samplePoint=item.Value; hardware=true; }
        }
        else if(item.Parameter==LOOPBACK) { current=loop; if(set) { if(item.Value>1) return fail(ERR_INVALID_IOCTL_VALUE,"LOOPBACK must be 0 or 1"); loop=item.Value; } }
        else if(item.Parameter==J1962_PINS && value.protocol!=CAN && value.protocol!=ISO15765) {
            current=pins;
            if(set) {
                if(value.connected && fdProtocol(value.protocol)) return fail(ERR_CHANNEL_IN_USE,"Pins are already connected");
                if(item.Value!=0x060E) return fail(ERR_PIN_INVALID,"Only the selected port's fixed CAN pins 6/14 are supported");
                if(fdProtocol(value.protocol) && !dataRate) return fail(ERR_INVALID_IOCTL_VALUE,"Set the data phase rate before pins");
                pins=item.Value; hardware=true;
            }
        }
        else if(fdProtocol(value.protocol) && item.Parameter==(gmProtocol(value.protocol) ? CAN_FD_DATA_PHASE_RATE : FD_CAN_DATA_PHASE_RATE)) {
            current=dataRate; if(set) { dataRate=item.Value; hardware=true; }
        }
        else if(item.Parameter==HS_CAN_TERMINATION) {
            current=term?3:0;
            if(set) {
                if(item.Value!=0 && item.Value!=3) return fail(ERR_INVALID_IOCTL_VALUE,"HS_CAN_TERMINATION must be 0 (off) or 3 (120 ohms)");
                term=item.Value==3?1:0;hardware=true;
            }
        }
        else if(gmProtocol(value.protocol) && item.Parameter==CAN_FD_TERMINATION) {
            current=term;
            if(set) {
                if(item.Value>1)return fail(ERR_INVALID_IOCTL_VALUE,"GM CAN FD termination must be 0 or 1");
                term=item.Value;hardware=true;
            }
        }
        else if(gmProtocol(value.protocol) && item.Parameter==CAN_FD_TYPE) {
            current=0; if(set && item.Value) return fail(ERR_NOT_SUPPORTED,"Only ISO CAN FD format is supported");
        }
        else if(isoProtocol(value.protocol) && (item.Parameter==ISO15765_BS || item.Parameter==ISO15765_STMIN ||
            item.Parameter==ISO15765_WFT_MAX || item.Parameter==BS_TX || item.Parameter==STMIN_TX ||
            item.Parameter==ISO15765_PAD_VALUE || item.Parameter==N_CR_MAX || item.Parameter==N_CR_MAX_GM ||
            item.Parameter==FD_ISO15765_TX_DATA_LENGTH || item.Parameter==CAN_FD_TC_DATA_LENGTH)) {
            U* setting=nullptr;
            if(item.Parameter==ISO15765_BS) setting=&iso.bs;
            else if(item.Parameter==ISO15765_STMIN) setting=&iso.stmin;
            else if(item.Parameter==ISO15765_WFT_MAX) setting=&iso.wft;
            else if(item.Parameter==BS_TX) setting=&iso.bsTx;
            else if(item.Parameter==STMIN_TX) setting=&iso.stminTx;
            else if(item.Parameter==ISO15765_PAD_VALUE) setting=&iso.pad;
            else if(item.Parameter==(gmProtocol(value.protocol) ? N_CR_MAX_GM : N_CR_MAX)) setting=&iso.nCr;
            else if(fdProtocol(value.protocol) && item.Parameter==(gmProtocol(value.protocol) ? CAN_FD_TC_DATA_LENGTH : FD_ISO15765_TX_DATA_LENGTH)) setting=&iso.txDl;
            if(!setting) return fail(ERR_NOT_SUPPORTED,"Parameter belongs to another FD extension");
            current=*setting;
            if(set) {
                const U v=item.Value;
                const bool stmin=item.Parameter==ISO15765_STMIN || item.Parameter==STMIN_TX;
                if(setting==&iso.txDl) { if(v<8 || dlc(v)<0) return fail(ERR_INVALID_IOCTL_VALUE,"Invalid ISO-TP TX data length"); }
                else if(setting==&iso.nCr) { if(v<1 || v>0x7FFFFFFFUL) return fail(ERR_INVALID_IOCTL_VALUE,"N_Cr must be 1..2147483647 microseconds"); }
                else if(stmin) { if(v>127 && !(v>=0xF1 && v<=0xF9) && !(setting==&iso.stminTx && v==0xFFFF)) return fail(ERR_INVALID_IOCTL_VALUE,"Invalid STmin encoding"); }
                else if(v>255 && !(setting==&iso.bsTx && v==0xFFFF)) return fail(ERR_INVALID_IOCTL_VALUE,"ISO-TP parameter is out of range");
                *setting=v;
            }
        }
        else {
            // Keep the rejected input visible before returning the unchanged
            // error. Successful polling remains outside durable diagnostics.
            trace(set ? "SET_CONFIG_UNSUPPORTED" : "GET_CONFIG_UNSUPPORTED",
                item.Parameter,set ? item.Value : 0,value.protocol);
            return fail(ERR_NOT_SUPPORTED,"Configuration parameter is unsupported for this protocol");
        }
        if(!set) item.Value=current;
        trace(set ? "SET_CONFIG" : "GET_CONFIG",item.Parameter,set ? item.Value : current);
    }
    if(set) {
        if((hardware || isoProtocol(value.protocol)) && (value.active || value.periodicActive || value.wireSubmitted || !value.transmit.empty())) return fail(ERR_CHANNEL_IN_USE,"Stop pending traffic before reconfiguring");
        if(hardware) {
            const bool connect=value.protocol==CAN || value.protocol==ISO15765 || (pins!=0 && (!fdProtocol(value.protocol) || dataRate!=0));
            const long result=apply(value,owner,rate,dataRate ? dataRate : rate,term,connect,samplePoint);
            if(result!=STATUS_NOERROR) return result;
        }
        value.dataRate=dataRate; value.pins=pins; value.loopback=loop; value.changed.notify_all();
        value.iso.settings=iso;
    }
    return STATUS_NOERROR;
}
// Inspect the caller's in/out count before acquiring channel/queue locks.
// The caller retains ownership and must keep the buffer alive for the call.
bool resetMessageCount(U* count,U* requested) {
    __try { *requested=*count; *count=0; return true; }
    __except((GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR)
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool messageExtentValid(PASSTHRU_MSG* messages,U requested) {
    const uintptr_t maximum=static_cast<uintptr_t>(-1);
    return requested<=maximum/sizeof(PASSTHRU_MSG) &&
        reinterpret_cast<uintptr_t>(messages)<=maximum-static_cast<uintptr_t>(requested)*sizeof(PASSTHRU_MSG);
}
// Keep access faults inside a plain copy helper so channel locks still unwind
// normally. Caller buffers must remain alive throughout the API call.
__declspec(noinline) bool copyClientMessage(PASSTHRU_MSG* output,const PASSTHRU_MSG* input) {
    __try { memcpy(output,input,sizeof(*output)); return true; }
    __except((GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR)
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
long messageError(const char* api,U id,long code) noexcept {
    bmj::diagnostic::ApiScope scope(api,id);bmj::diagnostic::error(code,lastError);return code;
}
long readImpl(U id, PASSTHRU_MSG* messages, U* count, U timeout) {
    U requested=0;
    if(noDevices()) {
        if(count && !resetMessageCount(count,&requested)) return messageError("PassThruReadMsgs",id,fail(ERR_NULL_PARAMETER,"Inaccessible read count"));
        return messageError("PassThruReadMsgs",id,fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first"));
    }
    if(!count || !resetMessageCount(count,&requested)) return messageError("PassThruReadMsgs",id,fail(ERR_NULL_PARAMETER,"Inaccessible read count"));
    if(!messages) return messageError("PassThruReadMsgs",id,fail(ERR_NULL_PARAMETER,"Missing read buffer"));
    if(!messageExtentValid(messages,requested)) return messageError("PassThruReadMsgs",id,fail(ERR_NULL_PARAMETER,"Read message extent overflows address space"));
    if(traceEnabled && readMetadataCount.fetch_add(1)<4) trace("READ_REQUEST",id,requested,timeout);
    auto value=channel(id);
    if(!value) return messageError("PassThruReadMsgs",id,fail(ERR_INVALID_CHANNEL_ID,"Unknown channel"));
    auto owner=value->device.lock(); std::unique_lock<std::mutex> lock(value->mutex);
    const auto deadline=Clock::now()+std::chrono::milliseconds(timeout);
    for(;;) {
        const long ready=available(*value,owner); if(ready) return messageError("PassThruReadMsgs",id,ready);
        while(*count<requested && !value->received.empty()) {
            if(!copyClientMessage(messages+*count,&value->received.front())) return messageError("PassThruReadMsgs",id,fail(ERR_NULL_PARAMETER,"Inaccessible read message buffer"));
            ++*count; value->received.pop_front();
        }
        for(U code=1; code<=4 && value->received.size()<RX_LIMIT; ++code) {
            if(value->pendingGmErrors&(1UL<<code)) {
                value->pendingGmErrors&=~(1UL<<code); gmError(*value,code,value->pendingGmErrorTimestamp);
            }
        }
        if(value->overflow) { value->overflow=false; return messageError("PassThruReadMsgs",id,fail(ERR_BUFFER_OVERFLOW,"Receive queue overflowed")); }
        if(*count==requested || (!timeout && *count)) return STATUS_NOERROR;
        if(!timeout || Clock::now()>=deadline)
            return fail(*count ? ERR_TIMEOUT : ERR_BUFFER_EMPTY,"Requested receive count was not available",false);
        value->changed.wait_until(lock,deadline);
    }
}
long writeImpl(U id, PASSTHRU_MSG* messages, U* count, U timeout) {
    U requested=0;
    if(noDevices()) {
        if(count && !resetMessageCount(count,&requested)) return messageError("PassThruWriteMsgs",id,fail(ERR_NULL_PARAMETER,"Inaccessible write count"));
        return messageError("PassThruWriteMsgs",id,fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first"));
    }
    if(!count || !resetMessageCount(count,&requested)) return messageError("PassThruWriteMsgs",id,fail(ERR_NULL_PARAMETER,"Inaccessible write count"));
    if(!messages) return messageError("PassThruWriteMsgs",id,fail(ERR_NULL_PARAMETER,"Missing write buffer"));
    if(!messageExtentValid(messages,requested)) return messageError("PassThruWriteMsgs",id,fail(ERR_NULL_PARAMETER,"Write message extent overflows address space"));
    auto value=channel(id); if(!value) return messageError("PassThruWriteMsgs",id,fail(ERR_INVALID_CHANNEL_ID,"Unknown channel"));
    auto owner=value->device.lock(); std::unique_lock<std::mutex> lock(value->mutex);
    long ready=available(*value,owner); if(ready) return messageError("PassThruWriteMsgs",id,ready);
    if(!value->connected) return messageError("PassThruWriteMsgs",id,fail(ERR_CHANNEL_IN_USE,"Configure the channel pins and bit rates before transmission"));
    if(value->recoveryFailed) return messageError("PassThruWriteMsgs",id,fail(ERR_TIMEOUT,"Bus-off recovery failed; reinitialize or reconnect the channel"));
    const auto deadline=Clock::now()+std::chrono::milliseconds(timeout);
    for(U i=0; i<requested; ++i) {
        PASSTHRU_MSG message;
        if(!copyClientMessage(&message,messages+i)) return messageError("PassThruWriteMsgs",id,fail(ERR_NULL_PARAMETER,"Inaccessible write message buffer"));
        ready=validateMessage(*value,message); if(ready) return messageError("PassThruWriteMsgs",id,ready);
        if(traceEnabled && writeMetadataCount.fetch_add(1)<8) trace("WRITE_METADATA",message.ProtocolID,message.TxFlags,message.DataSize);
        while(value->transmit.size()+(value->active ? 1 : 0)>=TX_LIMIT) {
            if(!timeout) return messageError("PassThruWriteMsgs",id,fail(ERR_BUFFER_FULL,"Transmit queue is full"));
            ready=available(*value,owner); if(ready) return messageError("PassThruWriteMsgs",id,ready);
            if(Clock::now()>=deadline) return messageError("PassThruWriteMsgs",id,fail(ERR_TIMEOUT,"Transmit queue did not accept all messages"));
            value->changed.wait_until(lock,deadline);
        }
        auto job=std::make_shared<Job>(); job->message=message;
        value->transmit.push_back(job); value->changed.notify_all();
        if(!timeout) { ++*count; continue; }
        while(!job->complete) {
            ready=available(*value,owner); if(ready) return messageError("PassThruWriteMsgs",id,ready);
            if(Clock::now()>=deadline) return messageError("PassThruWriteMsgs",id,fail(ERR_TIMEOUT,"Message has not completed on the bus; it remains queued"));
            value->changed.wait_until(lock,deadline);
        }
        if(job->result!=STATUS_NOERROR) return messageError("PassThruWriteMsgs",id,fail(job->result,"Message transmission failed or was cancelled"));
        ++*count;
    }
    return STATUS_NOERROR;
}
}
#define API_GUARD(expression) try { return (expression); } catch(const std::bad_alloc&) { return fail(ERR_EXCEEDED_LIMIT,"Memory limit exceeded"); } catch(...) { return fail(ERR_FAILED,"Internal API failure"); }

long WINAPI PassThruOpen(void* name, U* id) {
    const auto diagnosticSession=bmj::diagnostic::start();
    bmj::diagnostic::ApiScope diagnosticScope("PassThruOpen");
    bmj::diagnostic::event(diagnosticSession,"OPEN_BEGIN");
    try {
        refreshTrace();
        if(!id) return fail(ERR_NULL_PARAMETER,"Missing device ID output"); *id=0;
        if(!callerPacking(_ReturnAddress(),"OPEN_CALL")) return fail(ERR_NOT_SUPPORTED,"INCA J2534 backend packing differs from this provider");
        std::string selected=name ? static_cast<const char*>(name) : std::string();
        bool defaultTermination=false;
        // J2534-2 clients may request extension mode without a vendor selector.
        // Resolve the empty suffix through configuration, never by enumeration order.
        if(selected.compare(0,8,"J2534-2:")==0) {
            selected.erase(0,8);
        }
        const bool defaultSelection=!name || strcmp(static_cast<const char*>(name),"J2534-2:")==0;
        // Read once per Open: later Config saves never alter a live session.
        bmj::Selection configured;
        const auto status=bmj::readSelection(configured);
        if(status==bmj::SelectionStatus::InvalidSnapshot || (status==bmj::SelectionStatus::Invalid && defaultSelection))
            return fail(ERR_FAILED,"The saved selection is invalid or unreadable; use BUSMUST J2534 Configuration");
        if(defaultSelection) {
            if(configured.mode==bmj::SelectionMode::Auto) {
                // Auto is resolved once per Open, never persisted as a UID.
                // The opened Device owns this exact identity until Close.
                auto& initial=state();std::lock_guard<std::mutex> lock(initial.mutex);
                UnusedApiCleanup cleanup{initial};std::string error;
                if(!initial.api.load(error))return fail(ERR_DEVICE_NOT_CONNECTED,error.c_str());
                std::vector<BM_ChannelInfoTypeDef> candidates;
                const auto enumerated=initial.api.list(candidates);
                if(enumerated!=BM_ERROR_OK)return nativeFailure(enumerated,"Enumerate first selection");
                if(candidates.empty())return fail(ERR_DEVICE_NOT_CONNECTED,"No local CAN/CAN FD channel is connected");
                if(candidates.size()!=1)return fail(ERR_FAILED,"Select a device and channel in BUSMUST J2534 Configuration");
                configured.device=bmj::identity(candidates.front());
            }
            selected=configured.device;
            defaultTermination=configured.termination;
        }
        if(selected.empty()) return fail(ERR_FAILED,"An empty explicit device selector is invalid");
        bmj::diagnostic::event(diagnosticSession,"DEVICE_SELECTION",0,0,0,selected.c_str());
        if(!defaultSelection && !bmj::savedTermination(selected,defaultTermination,&configured))
            return fail(ERR_FAILED,"Cannot read this device/port's saved termination preference");
        auto& value=state(); std::lock_guard<std::mutex> lock(value.mutex);
        UnusedApiCleanup cleanup{value};
        std::shared_ptr<Device::Lease> lease;
        // Coordinate before BMAPI enumeration, which cannot inspect a device
        // owned by another process. Never turn that known owner into "absent".
        if(selected.size()>24 && selected[24]==':' &&
            selected.find_first_not_of("0123456789ABCDEF",0)>=24) {
            const std::string uid=selected.substr(0,24);
            for(const auto& entry:value.devices) {
                if(bmj::identity(entry.second->info).compare(0,24,uid)==0) { lease=entry.second->lease; break; }
            }
            if(!lease) {
                lease=std::make_shared<Device::Lease>();
                const std::wstring leaseName=L"Global\\BUSMUST.BMJ2534.Device."+std::wstring(uid.begin(),uid.end());
                SetLastError(ERROR_SUCCESS);
                lease->handle=CreateMutexW(nullptr,FALSE,leaseName.c_str());
                const DWORD error=GetLastError();
                if(!lease->handle) return fail(ERR_FAILED,"Cannot coordinate access to the selected device");
                if(error==ERROR_ALREADY_EXISTS) return fail(ERR_DEVICE_IN_USE,"The selected physical device is open in another J2534 process");
            }
        }
        std::string error;
        if(!value.api.load(error)) return fail(ERR_DEVICE_NOT_CONNECTED,error.c_str());
        bmj::diagnostic::event(diagnosticSession,"BMAPI_RUNTIME_VERSION",value.api.version(),0,0,"");
        std::vector<BM_ChannelInfoTypeDef> infos;
        BM_StatusTypeDef result=value.api.list(infos);
        if(result!=BM_ERROR_OK) return nativeFailure(result,"Enumerate");
        auto found=std::find_if(infos.begin(),infos.end(),[&](const BM_ChannelInfoTypeDef& info){ return bmj::identity(info)==selected; });
        if(found==infos.end()) return fail(ERR_DEVICE_NOT_CONNECTED,"The selected device/port is absent or cannot be accessed");
        for(const auto& entry:value.devices) if(bmj::identity(entry.second->info)==selected) return fail(ERR_DEVICE_IN_USE,"Device/port is already open");
        auto owner=std::make_shared<Device>(); owner->info=*found; owner->id=allocateId(value); owner->lease=lease;
        owner->diagnostics=diagnosticSession;
        char identityDetail[256]={};
        snprintf(identityDetail,sizeof(identityDetail),"model=%.*s sn=%.*s firmware=%u.%u.%u.%u",static_cast<int>(sizeof(owner->info.name)),owner->info.name,
            static_cast<int>(sizeof(owner->info.sn)),owner->info.sn,owner->info.version[0],owner->info.version[1],owner->info.version[2],owner->info.version[3]);
        bmj::diagnostic::event(diagnosticSession,"DEVICE_IDENTITY",owner->id,owner->info.port,owner->info.cap,identityDetail);
        owner->defaultTermination=defaultTermination;
        owner->customSampling=configured.mode==bmj::SelectionMode::Manual && configured.device==selected && configured.customSampling;
        owner->classicSampling=owner->customSampling && configured.classicSampling;
        owner->nominalSample=configured.nominalSample;owner->dataSample=configured.dataSample;
        if(!owner->id) return fail(ERR_EXCEEDED_LIMIT,"Device ID space exhausted");
        BM_BitrateTypeDef rates={}; rates.nbitrate=500; rates.dbitrate=500; rates.nsamplepos=75; rates.dsamplepos=80;
        result=value.api.open(&owner->handle,&owner->info,BM_CAN_OFF_MODE,BM_TRESISTOR_DISABLED,&rates,nullptr,0);
        if(result!=BM_ERROR_OK) return nativeFailure(result,"Open");
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&PassThruOpen),&owner->selfReference)) {
            value.api.close(owner->handle); return fail(ERR_FAILED,"Cannot retain the active provider module");
        }
        try { value.devices.emplace(owner->id,owner); }
        catch(...) { value.api.close(owner->handle); FreeLibrary(owner->selfReference); throw; }
        *id=owner->id; trace("OPEN_OK",*id); return STATUS_NOERROR;
    } catch(const std::bad_alloc&) { return fail(ERR_EXCEEDED_LIMIT,"Memory limit exceeded"); }
    catch(...) { return fail(ERR_FAILED,"Open failed"); }
}
long WINAPI PassThruClose(U id) {
    bmj::diagnostic::ApiScope diagnosticScope("PassThruClose",id);
    try {
        if(noDevices()) return fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first");
        trace("CLOSE",id);
        auto owner=device(id); if(!owner) return fail(ERR_INVALID_DEVICE_ID,"Unknown device");
        std::shared_ptr<Channel> active;
        { std::lock_guard<std::mutex> lock(owner->mutex);
          if(owner->closing.exchange(true)) return fail(ERR_INVALID_DEVICE_ID,"Device is closing");
          active=owner->channel; }
        if(active) stop(active);
        auto& value=state(); std::lock_guard<std::mutex> lock(value.mutex);
        if(active) value.channels.erase(active->id);
        const BM_StatusTypeDef result=value.api.close(owner->handle);
        bmj::diagnostic::event(owner->diagnostics,"NATIVE_CLOSE_RESULT",id,static_cast<U>(result));
        value.devices.erase(id);
        if(value.devices.empty()) value.api.unload();
        FreeLibrary(owner->selfReference);
        return result==BM_ERROR_OK ? STATUS_NOERROR : nativeFailure(result,"Close");
    } catch(...) { return fail(ERR_FAILED,"Close failed"); }
}
long WINAPI PassThruConnect(U id,U protocol,U flags,U rate,U* output) {
    bmj::diagnostic::ApiScope diagnosticScope("PassThruConnect",id);
    try {
        if(noDevices()) { if(output) *output=0; return fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first"); }
        trace("CONNECT",protocol,flags,rate);
        if(!output) return fail(ERR_NULL_PARAMETER,"Missing channel ID output"); *output=0;
        auto owner=device(id); if(!owner) return fail(ERR_INVALID_DEVICE_ID,"Unknown device");
        std::lock_guard<std::mutex> ownerLock(owner->mutex);
        if(owner->lost) return fail(ERR_DEVICE_NOT_CONNECTED,"Device connection was lost");
        if(owner->closing) return fail(ERR_INVALID_DEVICE_ID,"Device is closing");
        if(!rawProtocol(protocol) && !isoProtocol(protocol)) return fail(ERR_INVALID_PROTOCOL_ID,"Protocol is not implemented");
        if(fdProtocol(protocol) && !(owner->info.cap & BM_CAN_FD_CAP)) return fail(ERR_INVALID_PROTOCOL_ID,"The selected physical port does not support CAN FD");
        if(flags & ~(CAN_29BIT_ID|CAN_ID_BOTH)) return fail(ERR_INVALID_FLAGS,"Invalid connect flags");
        if(!validRate(rate)) return fail(ERR_INVALID_BAUDRATE,"Invalid nominal bit rate");
        if(owner->channel) return fail(ERR_CHANNEL_IN_USE,"The selected physical port already has a channel");
        auto active=std::make_shared<Channel>(); active->protocol=protocol; active->flags=flags;
        if(isoProtocol(protocol) && fdProtocol(protocol)) {
            active->iso.settings.txDl=64;
            if(gmProtocol(protocol)) { active->iso.settings.nCr=150000; active->iso.settings.pad=0xCC; }
        }
        active->rate=rate; active->dataRate=gmProtocol(protocol) ? rate*10 : (fdProtocol(protocol) ? 0 : rate);
        active->pins=protocol==CAN || protocol==ISO15765 ? 0x060E : 0;
        active->device=owner;
        const U defaultData=active->dataRate;
        // Config supplies the initial Classic CAN sample point; subsequent
        // standard J2534 SET_CONFIG remains authoritative and GET reports readback.
        if(!fdProtocol(protocol) && owner->classicSampling) active->samplePoint=owner->nominalSample;
        long result=apply(*active,owner,rate,defaultData ? defaultData : rate,owner->defaultTermination?1:0,active->pins!=0,active->samplePoint);
        if(result) return result;
        struct Rollback {
            std::shared_ptr<Device> owner; bool armed=true;
            explicit Rollback(const std::shared_ptr<Device>& device) : owner(device) {}
            ~Rollback() { if(armed && state().api.mode(owner->handle,BM_CAN_OFF_MODE)!=BM_ERROR_OK) owner->lost=true; }
        } rollback{owner};
        active->dataRate=defaultData;
        // Keep the native free-running clock across channel reconnects.
        // Preserve the existing clock-read failure check without rebasing events.
        uint32_t initialTimestamp=0;
        BM_StatusTypeDef stampResult=state().api.timestamp(owner->handle,&initialTimestamp);
        if(stampResult!=BM_ERROR_OK) return nativeFailure(stampResult,"Read initial timestamp");
        {
            auto& value=state(); std::lock_guard<std::mutex> lock(value.mutex);
            active->id=allocateId(value); if(!active->id) return fail(ERR_EXCEEDED_LIMIT,"Channel ID space exhausted");
            value.channels.emplace(active->id,active);
        }
        try { active->thread=std::thread(worker,active); }
        catch(...) {
            auto& value=state(); std::lock_guard<std::mutex> lock(value.mutex); value.channels.erase(active->id);
            value.api.mode(owner->handle,BM_CAN_OFF_MODE); throw;
        }
        owner->channel=active; *output=active->id; rollback.armed=false; trace("CONNECT_OK",*output); return STATUS_NOERROR;
    } catch(const std::bad_alloc&) { return fail(ERR_EXCEEDED_LIMIT,"Memory limit exceeded"); }
    catch(...) { return fail(ERR_FAILED,"Connect failed"); }
}
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if(reason==DLL_PROCESS_DETACH && !reserved) {
        // Active devices retain a module reference. Normal unload therefore has
        // no workers or USB resources, and needs only heap bookkeeping cleanup.
        delete stateSlot(); stateSlot()=nullptr;
    }
    return TRUE;
}
long WINAPI PassThruDisconnect(U id) {
    bmj::diagnostic::ApiScope diagnosticScope("PassThruDisconnect",id);
    try {
        if(noDevices()) return fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first");
        auto active=channel(id); if(!active) return fail(ERR_INVALID_CHANNEL_ID,"Unknown channel");
        auto owner=active->device.lock(); if(!owner) return fail(ERR_DEVICE_NOT_CONNECTED,"Device is absent");
        std::lock_guard<std::mutex> ownerLock(owner->mutex);
        if(owner->closing || owner->channel!=active) return fail(ERR_INVALID_CHANNEL_ID,"Channel is no longer active");
        stop(active);
        // Stop the native writer generation first, then discard both old RX
        // and completion records. No worker can submit to this handle now.
        auto& api=state().api;
        const BM_StatusTypeDef tx=api.clear(owner->handle,BM_TXQ_BUFFER);
        const BM_StatusTypeDef buffers=api.clearAll(owner->handle);
        const BM_StatusTypeDef mode=api.mode(owner->handle,BM_CAN_OFF_MODE);
        const BM_StatusTypeDef effectiveTx=tx==BM_ERROR_ILLOPERATION ? BM_ERROR_OK : tx;
        const BM_StatusTypeDef result=effectiveTx!=BM_ERROR_OK ? effectiveTx : (buffers!=BM_ERROR_OK ? buffers : mode);
        bmj::diagnostic::event(owner->diagnostics,"DISCONNECT_CLEAR_RESULT",static_cast<U>(tx),static_cast<U>(buffers),static_cast<U>(mode));
        if(result!=BM_ERROR_OK) owner->lost=true;
        { auto& value=state(); std::lock_guard<std::mutex> lock(value.mutex); value.channels.erase(id); }
        owner->channel.reset();
        return result==BM_ERROR_OK ? STATUS_NOERROR : nativeFailure(result,"Disconnect");
    } catch(...) { return fail(ERR_FAILED,"Disconnect failed"); }
}
long WINAPI PassThruReadMsgs(U id,PASSTHRU_MSG* messages,U* count,U timeout) {
    try {return readImpl(id,messages,count,timeout);}
    catch(const std::bad_alloc&) {return messageError("PassThruReadMsgs",id,fail(ERR_EXCEEDED_LIMIT,"Memory limit exceeded"));}
    catch(...) {return messageError("PassThruReadMsgs",id,fail(ERR_FAILED,"Internal API failure"));}
}
long WINAPI PassThruWriteMsgs(U id,PASSTHRU_MSG* messages,U* count,U timeout) {
    try {return writeImpl(id,messages,count,timeout);}
    catch(const std::bad_alloc&) {return messageError("PassThruWriteMsgs",id,fail(ERR_EXCEEDED_LIMIT,"Memory limit exceeded"));}
    catch(...) {return messageError("PassThruWriteMsgs",id,fail(ERR_FAILED,"Internal API failure"));}
}
long WINAPI PassThruStartPeriodicMsg(U id,PASSTHRU_MSG* message,U* output,U interval) {
    bmj::diagnostic::ApiScope diagnosticScope("PassThruStartPeriodicMsg",id);
    try {
        if(noDevices()) { if(output) *output=0; return fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first"); }
        if(!message || !output) return fail(ERR_NULL_PARAMETER,"Missing periodic message/output"); *output=0;
        auto value=channel(id); if(!value) return fail(ERR_INVALID_CHANNEL_ID,"Unknown channel");
        auto owner=value->device.lock(); std::lock_guard<std::mutex> lock(value->mutex);
        long result=available(*value,owner); if(result) return result;
        if(value->recoveryFailed) return fail(ERR_TIMEOUT,"Bus recovery failed; reconnect the channel before writing");
        result=validateMessage(*value,*message); if(result) return result;
        if(isoProtocol(value->protocol) && message->DataSize-4-((message->TxFlags&ISO15765_ADDR_TYPE)!=0)>value->iso.singleFrameCapacity(*message))
            return fail(ERR_INVALID_MSG,"Periodic ISO-TP messages must fit in a single frame");
        if(interval<5 || interval>65535) return fail(ERR_INVALID_TIME_INTERVAL,"Periodic interval must be 5..65535 ms");
        if(value->periodic.size()==PERIODIC_LIMIT) return fail(ERR_EXCEEDED_LIMIT,"Periodic message limit reached");
        auto& global=state(); U periodicId;
        { std::lock_guard<std::mutex> stateLock(global.mutex); periodicId=allocateId(global); }
        if(!periodicId) return fail(ERR_EXCEEDED_LIMIT,"Message ID space exhausted");
        value->periodic.emplace(periodicId,Periodic{*message,interval,Clock::now()});
        *output=periodicId; value->changed.notify_all(); return STATUS_NOERROR;
    } catch(...) { return fail(ERR_EXCEEDED_LIMIT,"Cannot create a periodic message"); }
}
long WINAPI PassThruStopPeriodicMsg(U id,U message) {
    bmj::diagnostic::ApiScope diagnosticScope("PassThruStopPeriodicMsg",id);
    try {
        if(noDevices()) return fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first");
        auto value=channel(id); if(!value) return fail(ERR_INVALID_CHANNEL_ID,"Unknown channel");
        auto owner=value->device.lock(); std::lock_guard<std::mutex> lock(value->mutex);
        long result=available(*value,owner); if(result) return result;
        if(value->periodic.find(message)==value->periodic.end()) return fail(ERR_INVALID_MSG_ID,"Unknown periodic message");
        result=cancelPeriodicJobs(*value,owner,message); if(result) return result;
        value->periodic.erase(message); value->changed.notify_all(); return STATUS_NOERROR;
    } catch(...) { return fail(ERR_FAILED,"Stop periodic message failed"); }
}
long WINAPI PassThruStartMsgFilter(U id,U type,PASSTHRU_MSG* mask,PASSTHRU_MSG* pattern,PASSTHRU_MSG* flow,U* output) {
    bmj::diagnostic::ApiScope diagnosticScope("PassThruStartMsgFilter",id);
    try {
        if(noDevices()) { if(output) *output=0; return fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first"); }
        trace("FILTER",id,type);
        if(!mask || !pattern || !output) return fail(ERR_NULL_PARAMETER,"Missing filter parameters"); *output=0;
        auto value=channel(id); if(!value) return fail(ERR_INVALID_CHANNEL_ID,"Unknown channel");
        auto owner=value->device.lock(); std::lock_guard<std::mutex> lock(value->mutex);
        long result=available(*value,owner); if(result) return result;
        if(isoProtocol(value->protocol)) {
            if(type!=FLOW_CONTROL_FILTER) return fail(ERR_INVALID_FILTER_ID,"ISO-TP requires a flow control filter");
            if(!flow && !gmProtocol(value->protocol)) return fail(ERR_NULL_PARAMETER,"Missing flow control message");
            if(mask->ProtocolID!=value->protocol || pattern->ProtocolID!=value->protocol || (flow && flow->ProtocolID!=value->protocol)) return fail(ERR_MSG_PROTOCOL_ID,"Filter protocol mismatch");
            if(!(value->flags&CAN_ID_BOTH) && ((pattern->TxFlags&CAN_29BIT_ID)!=0)!=((value->flags&CAN_29BIT_ID)!=0)) return fail(ERR_INVALID_MSG,"Filter identifier format differs from the channel");
            auto& global=state(); U filterId;
            { std::lock_guard<std::mutex> stateLock(global.mutex); filterId=allocateId(global); }
            if(!filterId) return fail(ERR_EXCEEDED_LIMIT,"Filter ID space exhausted");
            result=value->iso.add(filterId,*mask,*pattern,flow ? *flow : *pattern,flow!=nullptr);
            if(result) return fail(result,"Invalid, conflicting or excessive flow control filter");
            *output=filterId; return STATUS_NOERROR;
        }
        if(type!=PASS_FILTER && type!=BLOCK_FILTER) return fail(ERR_INVALID_FILTER_ID,"Filter type is unsupported for raw CAN");
        if(flow) return fail(ERR_INVALID_MSG,"Raw CAN filters must not specify flow control");
        const U maximum=fdProtocol(value->protocol) ? 68 : 12;
        if(mask->ProtocolID!=value->protocol || pattern->ProtocolID!=value->protocol) return fail(ERR_MSG_PROTOCOL_ID,"Filter protocol mismatch");
        if(mask->DataSize<4 || mask->DataSize>maximum || mask->DataSize!=pattern->DataSize ||
            mask->TxFlags!=pattern->TxFlags || (pattern->TxFlags & ~CAN_29BIT_ID)) return fail(ERR_INVALID_MSG,"Invalid filter size or flags");
        if(!(value->flags&CAN_ID_BOTH) && ((pattern->TxFlags&CAN_29BIT_ID)!=0)!=((value->flags&CAN_29BIT_ID)!=0))
            return fail(ERR_INVALID_MSG,"Filter identifier format differs from the channel");
        if(value->filters.size()==FILTER_LIMIT) return fail(ERR_EXCEEDED_LIMIT,"Filter limit reached");
        size_t sameType=0;
        for(const auto& entry:value->filters) if(entry.second.type==type) ++sameType;
        if(sameType>=10) return fail(ERR_EXCEEDED_LIMIT,"Filter type limit reached");
        auto& global=state(); U filterId;
        { std::lock_guard<std::mutex> stateLock(global.mutex); filterId=allocateId(global); }
        if(!filterId) return fail(ERR_EXCEEDED_LIMIT,"Filter ID space exhausted");
        value->filters.emplace(filterId,Filter{type,*mask,*pattern}); *output=filterId; return STATUS_NOERROR;
    } catch(...) { return fail(ERR_EXCEEDED_LIMIT,"Cannot create a filter"); }
}
long WINAPI PassThruStopMsgFilter(U id,U filter) {
    bmj::diagnostic::ApiScope diagnosticScope("PassThruStopMsgFilter",id);
    try {
        if(noDevices()) return fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first");
        auto value=channel(id); if(!value) return fail(ERR_INVALID_CHANNEL_ID,"Unknown channel");
        auto owner=value->device.lock(); std::lock_guard<std::mutex> lock(value->mutex);
        long result=available(*value,owner); if(result) return result;
        if(isoProtocol(value->protocol)) {
            if(!value->iso.contains(filter)) return fail(ERR_INVALID_FILTER_ID,"Unknown filter");
            const auto* front=value->iso.front();
            if(value->wireSubmitted && !value->periodicWire && front && front->filterId==filter) {
                // Only the one submitted frame can be in the BMAPI TX queue.
                // Other conversations' queued frames remain in the transport.
                const auto cleared=state().api.clearDirection(owner->handle,BM_TXQ_BUFFER);
                if(cleared!=BM_ERROR_OK) return nativeFailure(cleared,"Stop active flow control filter");
                value->wireSubmitted=false;
            }
            value->iso.remove(filter);
        } else if(!value->filters.erase(filter)) return fail(ERR_INVALID_FILTER_ID,"Unknown filter");
        return STATUS_NOERROR;
    } catch(...) { return fail(ERR_FAILED,"Stop filter failed"); }
}
long WINAPI PassThruSetProgrammingVoltage(U id,U,U) {
    bmj::diagnostic::ApiScope diagnosticScope("PassThruSetProgrammingVoltage",id);
    API_GUARD(([&]() -> long {
        auto owner=device(id); if(!owner) return fail(ERR_INVALID_DEVICE_ID,"Unknown device");
        if(owner->lost || owner->closing) return fail(ERR_DEVICE_NOT_CONNECTED,"Device is disconnected");
        return fail(ERR_NOT_SUPPORTED,"Programming voltage is not available");
    })());
}
long WINAPI PassThruReadVersion(U id,char* firmware,char* dll,char* api) {
    bmj::diagnostic::ApiScope diagnosticScope("PassThruReadVersion",id);
    try {
        if(noDevices()) return fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first");
        if(!firmware || !dll || !api) return fail(ERR_NULL_PARAMETER,"Missing version buffers");
        auto& value=state(); std::lock_guard<std::mutex> lock(value.mutex);
        auto found=value.devices.find(id); if(found==value.devices.end()) return fail(ERR_INVALID_DEVICE_ID,"Unknown device");
        const auto& owner=found->second;
        if(owner->lost || owner->closing) return fail(ERR_DEVICE_NOT_CONNECTED,"Device is disconnected");
        sprintf_s(firmware,80,"%u.%u.%u.%u",owner->info.version[0],owner->info.version[1],owner->info.version[2],owner->info.version[3]);
        strcpy_s(dll,80,BMJ_FILEVERSION_STRING);
        strcpy_s(api,80,"04.04"); return STATUS_NOERROR;
    } catch(...) { return fail(ERR_FAILED,"Read version failed"); }
}
long WINAPI PassThruGetLastError(char* output) {
    if(!output) return ERR_NULL_PARAMETER;
    strcpy_s(output,80,lastError); return STATUS_NOERROR;
}
long WINAPI PassThruIoctl(U id,U ioctl,void* input,void* output) {
    bmj::diagnostic::ApiScope diagnosticScope("PassThruIoctl",id);
    try {
        if(noDevices()) return fail(ERR_INVALID_DEVICE_ID,"Call PassThruOpen first");
        trace("IOCTL",id,ioctl);
        if(!callerPacking(_ReturnAddress(),"IOCTL_CALL")) return fail(ERR_NOT_SUPPORTED,"INCA J2534 backend packing differs from this provider");
        if(ioctl==GET_DEVICE_INFO) {
            auto owner=device(id); if(!owner) return fail(ERR_INVALID_DEVICE_ID,"Unknown device");
            if(owner->lost || owner->closing) return fail(ERR_DEVICE_NOT_CONNECTED,"Device is disconnected");
            auto* list=static_cast<SPARAM_LIST*>(input);
            if(!list || (list->NumOfParams && !list->ParamPtr)) return fail(ERR_NULL_PARAMETER,"Missing SPARAM_LIST");
            if(list->NumOfParams>128) return fail(ERR_EXCEEDED_LIMIT,"Too many capability parameters");
            for(U i=0;i<list->NumOfParams;++i) {
                auto& item=list->ParamPtr[i]; item.Supported=0;
                if(item.Parameter==6 || item.Parameter==7 || item.Parameter==0x4C || item.Parameter==0x4D) {
                    item.Supported=1; item.Value=(item.Parameter==6 || item.Parameter==7 || (owner->info.cap & BM_CAN_FD_CAP)) ? 1 : 0;
                }
                else if(item.Parameter==1) {
                    item.Supported=1; item.Value=static_cast<U>(owner->info.uid[8])<<24 |
                        static_cast<U>(owner->info.uid[9])<<16 | static_cast<U>(owner->info.uid[10])<<8 | owner->info.uid[11];
                }
            }
            return STATUS_NOERROR;
        }
        auto value=channel(id);
        if(!value) {
            if(ioctl==READ_VBATT || ioctl==READ_PROG_VOLTAGE) {
                auto owner=device(id);
                if(owner) {
                    if(owner->lost || owner->closing) return fail(ERR_DEVICE_NOT_CONNECTED,"Device is disconnected");
                    return fail(ERR_NOT_SUPPORTED,"Voltage measurement is unavailable");
                }
            }
            return fail(ERR_INVALID_CHANNEL_ID,"Unknown channel");
        }
        auto owner=value->device.lock(); std::lock_guard<std::mutex> lock(value->mutex);
        long result=available(*value,owner); if(result) return result;
        if(ioctl==GET_CONFIG || ioctl==SET_CONFIG) return configure(*value,owner,static_cast<SCONFIG_LIST*>(input),ioctl==SET_CONFIG);
        if(ioctl==GET_PROTOCOL_INFO) {
            auto* list=static_cast<SPARAM_LIST*>(input);
            if(!list || (list->NumOfParams && !list->ParamPtr)) return fail(ERR_NULL_PARAMETER,"Missing SPARAM_LIST");
            if(list->NumOfParams>128) return fail(ERR_EXCEEDED_LIMIT,"Too many capability parameters");
            for(U i=0;i<list->NumOfParams;++i) {
                auto& item=list->ParamPtr[i]; item.Supported=1;
                switch(item.Parameter) {
                    case 1:item.Value=static_cast<U>(RX_LIMIT)*(isoProtocol(value->protocol) ? 4128 : (fdProtocol(value->protocol) ? 68 : 12));break;
                    case 2:case 3:item.Value=isoProtocol(value->protocol) ? 0 : 10;break;
                    case 4:item.Value=isoProtocol(value->protocol) ? 5 : (fdProtocol(value->protocol) ? 68 : 12);break;
                    case 5:item.Value=PERIODIC_LIMIT;break;
                    case 6:item.Value=isoProtocol(value->protocol) ? (fdProtocol(value->protocol) ? 66 : 11) : (fdProtocol(value->protocol) ? 68 : 12);break;
                    case 0x12:item.Value=isoProtocol(value->protocol) ? bmj::ISO_FLOW_FILTER_LIMIT : 0;break;
                    case 0x13:item.Value=isoProtocol(value->protocol) ? 255 : 0;break;
                    case 0x10:item.Value=1;break;
                    case 0x11:item.Value=0;break;
                    case 0x1B:item.Value=1;break;
                    case 0x6C:item.Supported=fdProtocol(value->protocol) ? 1 : 0;item.Value=1;break;
                    default:item.Supported=0;break;
                }
            }
            return STATUS_NOERROR;
        }
        if(ioctl==CLEAR_RX_BUFFER || ioctl==CLEAR_TX_BUFFER) {
            const BM_StatusTypeDef clear=state().api.clearDirection(owner->handle,ioctl==CLEAR_RX_BUFFER ? BM_RXQ_BUFFER : BM_TXQ_BUFFER);
            if(clear!=BM_ERROR_OK) return nativeFailure(clear,"Clear buffer");
            if(ioctl==CLEAR_RX_BUFFER) { value->received.clear(); value->overflow=false; value->pendingGmErrors=0; value->iso.clearRx(value->wireSubmitted && !value->periodicWire); }
            else cancelTransmit(*value,ERR_TIMEOUT);
            value->changed.notify_all(); return STATUS_NOERROR;
        }
        if(ioctl==CLEAR_PERIODIC_MSGS) {
            result=cancelPeriodicJobs(*value,owner,0); if(result) return result;
            value->periodic.clear(); value->changed.notify_all(); return STATUS_NOERROR;
        }
        if(ioctl==CLEAR_MSG_FILTERS) {
            const auto* front=value->iso.front();
            if(isoProtocol(value->protocol) && value->wireSubmitted && !value->periodicWire && front && front->filterId!=0) {
                const auto clear=state().api.clearDirection(owner->handle,BM_TXQ_BUFFER);
                if(clear!=BM_ERROR_OK) return nativeFailure(clear,"Clear flow control filters");
                value->wireSubmitted=false;
            }
            value->filters.clear(); value->iso.clearFilters(); return STATUS_NOERROR;
        }
        if(ioctl==READ_VBATT || ioctl==READ_PROG_VOLTAGE) return fail(ERR_NOT_SUPPORTED,"Voltage measurement is unavailable");
        (void)output;
        return fail(ERR_INVALID_IOCTL_ID,"Unsupported IOCTL");
    } catch(const std::bad_alloc&) { return fail(ERR_EXCEEDED_LIMIT,"Memory limit exceeded"); }
    catch(...) { return fail(ERR_FAILED,"IOCTL failed"); }
}
