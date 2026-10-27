#include "bmj2534_isotp.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace bmj {
namespace {
uint32_t identifier(const PASSTHRU_MSG& m) {
    return uint32_t(m.Data[0])<<24 | uint32_t(m.Data[1])<<16 | uint32_t(m.Data[2])<<8 | m.Data[3];
}
bool gm(unsigned long p) { return p==ISO15765_FD_PS; }
bool fd(unsigned long p) { return p==ISO15765_FD_PS || p==FD_ISO15765_PS; }
unsigned long format(unsigned long p) { return gm(p) ? CAN_FD_FORMAT : FD_CAN_FORMAT; }
unsigned long brs(unsigned long p) { return gm(p) ? CAN_FD_BRS : FD_CAN_BRS; }
uint32_t separation(unsigned long encoded) { return encoded<=127 ? encoded*1000 : (encoded-0xF0)*100; }
bool sameHeader(const PASSTHRU_MSG& a,const PASSTHRU_MSG& b,bool ignorePriority=false) {
    const auto bits=CAN_29BIT_ID|ISO15765_ADDR_TYPE;
    if((a.TxFlags&bits)!=(b.TxFlags&bits) || a.DataSize<b.DataSize) return false;
    for(unsigned long i=0;i<b.DataSize;++i) {
        const auto mask=i==0 && ignorePriority ? 0xE3 : 0xFF;
        if((a.Data[i]&mask)!=(b.Data[i]&mask)) return false;
    }
    return true;
}
}
struct IsoSession : std::enable_shared_from_this<IsoSession> {
    IsoTransport* owner;
    IsoTpLink link={};
    PASSTHRU_MSG pattern={}, flow={}, metadata={};
    std::array<uint8_t,4124> tx,rx;
    unsigned long id=0, txFlags=0;
    size_t pending=0;
    bool paddingError=false;
    bool canSendFlow=true;
    bool ignorePriority=false, functional=false;
    uint32_t startedAt=0;
    uint64_t receiveGeneration=0;
    uint32_t receiveFlowDeadline=0;
    uint32_t receiveFlowOffset=0;
    bool receiveFlowPending=false;
    uint32_t receiveWireTimestamp=0, sendWireTimestamp=0;
    bool receiveWireTimerActive=false, sendWireTimerActive=false;
    IsoSession(IsoTransport* value,const PASSTHRU_MSG& p,const PASSTHRU_MSG& f) : owner(value),pattern(p),flow(f) {
        const auto capacity=static_cast<uint32_t>(fd(f.ProtocolID) ? 4128-f.DataSize : ISO_CLASSIC_MAX_PDU);
        isotp_init_link(&link,identifier(f),tx.data(),capacity,rx.data(),capacity);
        link.address_length=(f.TxFlags&ISO15765_ADDR_TYPE)!=0;
        link.user_send_can_arg=this;
    }
};
void IsoTransport::configure(IsoSession& s,const PASSTHRU_MSG& m) {
    auto& l=s.link;
    l.block_size=static_cast<uint8_t>(settings.bs); l.st_min_us=separation(settings.stmin);
    l.wft_max=static_cast<uint8_t>(settings.wft); l.bs_tx=static_cast<uint16_t>(settings.bsTx);
    l.stmin_tx=static_cast<uint16_t>(settings.stminTx); l.receive_timeout_us=settings.nCr;
    l.padding=(m.TxFlags&ISO15765_FRAME_PAD)!=0; l.pad_value=static_cast<uint8_t>(settings.pad);
    l.frame_flags=fd(m.ProtocolID) && (m.TxFlags&format(m.ProtocolID)) ? ISOTP_CAN_FRAME_FLAG_FD : 0;
    if(l.frame_flags && (m.TxFlags&brs(m.ProtocolID))) l.frame_flags|=ISOTP_CAN_FRAME_FLAG_BRS;
    l.tx_dl=l.frame_flags ? static_cast<uint8_t>(settings.txDl) : 8;
    s.txFlags=m.TxFlags;
}
long IsoTransport::add(unsigned long id,const PASSTHRU_MSG& mask,const PASSTHRU_MSG& pattern,const PASSTHRU_MSG& flow,bool sendFlow) {
    const unsigned long header=4+((pattern.TxFlags&ISO15765_ADDR_TYPE)!=0);
    if(mask.DataSize!=header || pattern.DataSize!=header || flow.DataSize!=header ||
       mask.TxFlags!=pattern.TxFlags ||
       (flow.TxFlags&(CAN_29BIT_ID|ISO15765_ADDR_TYPE))!=(pattern.TxFlags&(CAN_29BIT_ID|ISO15765_ADDR_TYPE)) ||
       (pattern.TxFlags&~(CAN_29BIT_ID|ISO15765_ADDR_TYPE|ISO15765_FRAME_PAD))) return ERR_INVALID_MSG;
    if(!fd(pattern.ProtocolID)) {
        if(flow.TxFlags!=pattern.TxFlags) return ERR_INVALID_MSG;
    } else if(flow.TxFlags&~(CAN_29BIT_ID|ISO15765_ADDR_TYPE|ISO15765_FRAME_PAD|format(pattern.ProtocolID)|brs(pattern.ProtocolID))) return ERR_INVALID_MSG;
    const bool priority=(pattern.TxFlags&CAN_29BIT_ID) && mask.Data[0]==0xE3;
    for(unsigned long i=0;i<header;++i) if(mask.Data[i]!=255 && !(i==0 && priority)) return ERR_INVALID_MSG;
    const auto maximum=(pattern.TxFlags&CAN_29BIT_ID) ? 0x1FFFFFFFUL : 0x7FFUL;
    if(identifier(pattern)>maximum || identifier(flow)>maximum) return ERR_INVALID_MSG;
    for(const auto& item:sessions) {
        const auto& old=*item.second; const bool ignorePriority=priority || old.ignorePriority;
        // The priority mask belongs to the RX pattern; TX IDs remain exact.
        if(sameHeader(pattern,old.pattern,ignorePriority) ||
           (old.canSendFlow && sameHeader(pattern,old.flow,priority)) ||
           (sendFlow && (sameHeader(flow,old.pattern,old.ignorePriority) ||
                        (old.canSendFlow && sameHeader(flow,old.flow))))) return ERR_NOT_UNIQUE;
    }
    if(sessions.size()>=ISO_FLOW_FILTER_LIMIT) return ERR_EXCEEDED_LIMIT;
    auto session=std::make_shared<IsoSession>(this,pattern,flow); session->id=id; session->canSendFlow=sendFlow;
    session->ignorePriority=priority; session->functional=sendFlow && sameHeader(pattern,flow);
    configure(*session,flow); sessions.emplace(id,session); return STATUS_NOERROR;
}
bool IsoTransport::contains(unsigned long id) const { return sessions.find(id)!=sessions.end(); }
void IsoTransport::remove(unsigned long id) {
    auto found=sessions.find(id); if(found==sessions.end()) return;
    auto session=found->second;
    if(sending==session) complete(ERR_NO_FLOW_CONTROL,0);
    frames.erase(std::remove_if(frames.begin(),frames.end(),[id](const IsoFrame& f){return f.session->id==id;}),frames.end());
    sessions.erase(found);
}
void IsoTransport::clearFilters() { while(!sessions.empty()) remove(sessions.begin()->first); }
size_t IsoTransport::singleFrameCapacity(const PASSTHRU_MSG& message) const {
    const auto address=(message.TxFlags&ISO15765_ADDR_TYPE)!=0;
    const auto txDl=fd(message.ProtocolID) && (message.TxFlags&format(message.ProtocolID)) ? settings.txDl : 8;
    return (txDl>8 ? txDl-2 : txDl-1)-address;
}
long IsoTransport::validate(const PASSTHRU_MSG& message) const {
    for(const auto& item:sessions) if(item.second->canSendFlow && !item.second->functional && sameHeader(message,item.second->flow)) return STATUS_NOERROR;
    const auto address=(message.TxFlags&ISO15765_ADDR_TYPE)!=0;
    return message.DataSize-4-address<=singleFrameCapacity(message) ? STATUS_NOERROR : ERR_NO_FLOW_CONTROL;
}
long IsoTransport::start(const PASSTHRU_MSG& message) {
    if(sending) return ERR_CHANNEL_IN_USE;
    const long valid=validate(message); if(valid) return valid;
    for(const auto& item:sessions) if(item.second->canSendFlow && sameHeader(message,item.second->flow)) { sending=item.second; break; }
    if(!sending) {
        PASSTHRU_MSG header=message; header.DataSize=4+((message.TxFlags&ISO15765_ADDR_TYPE)!=0);
        sending=std::make_shared<IsoSession>(this,header,header);
    }
    configure(*sending,message);
    sending->sendWireTimerActive=false;
    completed=false; sendError=STATUS_NOERROR; sendTimestamp=0;
    const auto header=4+sending->link.address_length;
    const int result=isotp_send(&sending->link,message.Data+header,message.DataSize-header);
    if(result!=ISOTP_RET_OK) { sending.reset(); return result==ISOTP_RET_NOSPACE ? ERR_BUFFER_FULL : ERR_FAILED; }
    return STATUS_NOERROR;
}
int IsoTransport::enqueue(IsoSession* s,uint32_t id,const uint8_t* bytes,uint8_t size,uint8_t flags) {
    if((bytes[0]>>4)==3 && !s->canSendFlow) return ISOTP_RET_OK;
    // N_Cr starts at CTS L_Data.con, not at FF/block arrival or queue admission.
    // Passive conversations have no local FC confirmation to wait for.
    if(bytes[0]==0x30 && s->link.receive_status==ISOTP_RECEIVE_STATUS_INPROGRESS) {
        // Default N_Bs is 1 s. Bound request-to-confirmation below its 0.9
        // performance limit (and N_Ar=1 s), including queue/admission retries.
        if(!s->link.fc_pending) {
            s->receiveFlowDeadline=isotp_user_get_us()+899000;
            s->receiveFlowOffset=s->link.receive_offset; s->receiveFlowPending=true;
        }
        s->link.receive_timer_cr=0;
    }
    if(frames.size()>=32) return ISOTP_RET_NOSPACE;
    IsoFrame frame; frame.session=s->shared_from_this(); frame.id=id; frame.filterId=s->id;
    frame.flow=(bytes[0]>>4)==3;
    frame.receiveGeneration=s->receiveGeneration; frame.receiveOffset=s->link.receive_offset;
    if(frame.flow) frame.confirmDeadline=bytes[0]==0x30 ? s->receiveFlowDeadline : isotp_user_get_us()+899000;
    frame.flags=s->flow.TxFlags&(CAN_29BIT_ID|ISO15765_ADDR_TYPE);
    if(flags&ISOTP_CAN_FRAME_FLAG_FD) frame.flags|=format(s->flow.ProtocolID);
    if(flags&ISOTP_CAN_FRAME_FLAG_BRS) frame.flags|=brs(s->flow.ProtocolID);
    const auto address=s->link.address_length;
    frame.size=size+address;
    if(address) frame.data[0]=s->flow.Data[4];
    memcpy(frame.data+address,bytes,size);
    // Queue insertion must succeed before advancing the transport's state.
    frames.push_back(frame); ++s->pending; s->link.send_pending=1;
    return ISOTP_RET_OK;
}
void IsoTransport::collect(IsoSession& s,const PASSTHRU_MSG& frame,bool first) {
    if(first) { s.metadata=frame; s.metadata.RxStatus|=s.pattern.TxFlags&ISO15765_ADDR_TYPE; s.paddingError=false; }
    // The complete PDU ends at its last frame; START_OF_MESSAGE is queued
    // separately with the first frame's timestamp.
    s.metadata.Timestamp=frame.Timestamp;
    s.startedAt=isotp_user_get_us();
    s.metadata.RxStatus|=frame.RxStatus;
    if(!(frame.RxStatus&format(frame.ProtocolID)) && frame.DataSize!=12) s.paddingError=true;
    if(s.link.receive_status!=ISOTP_RECEIVE_STATUS_FULL) return;
    PASSTHRU_MSG message=s.metadata;
    const auto header=s.pattern.DataSize;
    uint32_t size=0;
    if(isotp_receive(&s.link,message.Data+header,4128-header,&size)!=ISOTP_RET_OK) return;
    message.DataSize=header+size; message.ExtraDataIndex=message.DataSize;
    // GM uses bit 4 for ERROR_IND; it cannot mark normal data as a padding
    // indication using the base protocol's overlapping bit.
    if(s.paddingError && !gm(message.ProtocolID)) message.RxStatus|=ISO15765_PADDING_ERROR;
    if(received.size()<256) received.push_back(message); else overflow=true;
}
void IsoTransport::abortReceive(IsoSession& s) {
    s.link.receive_status=ISOTP_RECEIVE_STATUS_IDLE; s.link.fc_pending=0;
    s.link.receive_timer_cr=0; s.receiveFlowDeadline=0; s.receiveFlowPending=false; s.receiveWireTimerActive=false; ++s.receiveGeneration;
    frames.erase(std::remove_if(frames.begin(),frames.end(),[&](const IsoFrame& frame){
        if(frame.session.get()==&s && frame.flow && !frame.submitted) { if(s.pending) --s.pending; return true; }
        return false;
    }),frames.end());
    s.link.send_pending=s.pending!=0;
}
void IsoTransport::receiveError(IsoSession& s,unsigned long code,unsigned long timestamp) {
    if(code==3) abortReceive(s);
    if(!gm(s.pattern.ProtocolID)) return;
    PASSTHRU_MSG indication={}; indication.ProtocolID=s.pattern.ProtocolID; indication.RxStatus=ERROR_IND;
    indication.Timestamp=timestamp; indication.DataSize=4; indication.Data[3]=static_cast<uint8_t>(code);
    if(received.size()<256) received.push_back(indication); else overflow=true;
}
void IsoTransport::receive(const PASSTHRU_MSG& frame,uint32_t wireTimestamp) {
    for(auto& item:sessions) {
        auto& s=*item.second;
        PASSTHRU_MSG header=frame; header.TxFlags=frame.RxStatus&(CAN_29BIT_ID|ISO15765_ADDR_TYPE);
        // Addressing is selected by each conversation, not by an on-wire flag.
        header.TxFlags|=s.pattern.TxFlags&ISO15765_ADDR_TYPE;
        if(!sameHeader(header,s.pattern,s.ignorePriority) || frame.DataSize<=s.pattern.DataSize) continue;
        auto& l=s.link; const auto offset=s.pattern.DataSize;
        const auto pci=frame.Data[offset]>>4;
        if(pci>3) continue;
        // The core ignores incomplete FCs. They must not renew the wire-time
        // anchor used to decide whether a later valid FC is still timely.
        if(pci==3 && frame.DataSize-offset<3) break;
        // Explicit functional filters admit only SFs. GM's null-flow passive
        // conversations are distinct and may still receive segmented PDUs.
        if(s.functional && pci!=0) continue;
        if(pci<=2 && isotp_validate_receive_frame(&l,frame.Data+offset,static_cast<uint8_t>(frame.DataSize-offset))!=ISOTP_RET_OK) break;
        const bool inProgress=l.receive_status==ISOTP_RECEIVE_STATUS_INPROGRESS;
        const bool incomingFd=(frame.RxStatus&format(frame.ProtocolID))!=0;
        // Wrong N_AI frame types are ignored before changing RX or FC state.
        if((pci==2 && inProgress && ((s.metadata.RxStatus&format(frame.ProtocolID))!=0)!=incomingFd) ||
           (pci==3 && l.send_status==ISOTP_SEND_STATUS_INPROGRESS && ((l.frame_flags&ISOTP_CAN_FRAME_FLAG_FD)!=0)!=incomingFd)) break;
        // Compare arrival intervals in the native BM timestamp domain before
        // the core refreshes its host timers. Dispatch can be delayed while
        // timely frames are already queued; polling first would discard them.
        if(pci==2 && inProgress && s.receiveWireTimerActive &&
           IsoTpTimeAfter(wireTimestamp,s.receiveWireTimestamp+l.receive_timeout_us)) {
            receiveError(s,3,frame.Timestamp); break;
        }
        if(pci==3 && l.send_status==ISOTP_SEND_STATUS_INPROGRESS && l.send_bs_remain==0 &&
           s.sendWireTimerActive && IsoTpTimeAfter(wireTimestamp,s.sendWireTimestamp+l.response_timeout_us)) {
            l.send_protocol_result=ISOTP_PROTOCOL_RESULT_TIMEOUT_BS; l.send_status=ISOTP_SEND_STATUS_ERROR;
            if(sending.get()==&s) complete(ERR_TIMEOUT,0);
            break;
        }
        if((pci==0 || pci==1) && inProgress) {
            receiveError(s,3,frame.Timestamp);
            // The new PDU can start after terminating the abandoned old one.
            l.receive_status=ISOTP_RECEIVE_STATUS_IDLE; l.fc_pending=0;
        }
        if(pci!=3) {
            l.block_size=static_cast<uint8_t>(settings.bs); l.st_min_us=separation(settings.stmin);
            l.receive_timeout_us=settings.nCr; l.pad_value=static_cast<uint8_t>(settings.pad);
            l.receive_frame_flags=frame.RxStatus&format(frame.ProtocolID) ? ISOTP_CAN_FRAME_FLAG_FD : 0;
            if(l.receive_frame_flags && (frame.RxStatus&brs(frame.ProtocolID))) l.receive_frame_flags|=ISOTP_CAN_FRAME_FLAG_BRS;
        }
        const bool first=pci==0 || pci==1;
        if(first) { ++s.receiveGeneration; l.receive_timer_cr=0; s.receiveWireTimerActive=false; }
        isotp_on_can_message(&l,frame.Data+offset,static_cast<uint8_t>(frame.DataSize-offset));
        if(pci!=3 && (l.receive_status==ISOTP_RECEIVE_STATUS_INPROGRESS || l.receive_status==ISOTP_RECEIVE_STATUS_FULL)) {
            s.receiveWireTimestamp=wireTimestamp; s.receiveWireTimerActive=l.receive_timer_cr!=0;
        } else if(pci==3 && l.send_status==ISOTP_SEND_STATUS_INPROGRESS && l.send_protocol_result==ISOTP_PROTOCOL_RESULT_OK) {
            s.sendWireTimestamp=wireTimestamp; s.sendWireTimerActive=true;
        }
        if(pci==1 && l.receive_status==ISOTP_RECEIVE_STATUS_INPROGRESS) {
            PASSTHRU_MSG start=frame; start.RxStatus|=START_OF_MESSAGE|(s.pattern.TxFlags&ISO15765_ADDR_TYPE);
            start.DataSize=offset; start.ExtraDataIndex=0;
            if(gm(frame.ProtocolID)) {
                const uint32_t needed=offset+l.receive_size;
                start.Data[offset]=static_cast<uint8_t>(needed>>24); start.Data[offset+1]=static_cast<uint8_t>(needed>>16);
                start.Data[offset+2]=static_cast<uint8_t>(needed>>8); start.Data[offset+3]=static_cast<uint8_t>(needed);
                start.DataSize+=4;
            }
            if(received.size()<256) received.push_back(start); else overflow=true;
        }
        if(pci!=3 && (l.receive_status==ISOTP_RECEIVE_STATUS_INPROGRESS || l.receive_status==ISOTP_RECEIVE_STATUS_FULL)) collect(s,frame,first);
        if(l.receive_protocol_result==ISOTP_PROTOCOL_RESULT_BUFFER_OVFLW) {
            overflow=true; receiveError(s,1,frame.Timestamp); l.receive_protocol_result=ISOTP_PROTOCOL_RESULT_OK;
        } else if(pci!=3 && (inProgress || pci==1) && l.receive_protocol_result!=ISOTP_PROTOCOL_RESULT_OK) {
            receiveError(s,3,frame.Timestamp); l.receive_protocol_result=ISOTP_PROTOCOL_RESULT_OK;
        }
        break;
    }
}
void IsoTransport::complete(long error,uint32_t timestamp) {
    if(!sending || completed) return;
    completed=true; sendError=error; sendTimestamp=timestamp;
}
void IsoTransport::poll() {
    for(auto& item:sessions) {
        auto& s=*item.second;
        if(s.link.receive_status==ISOTP_RECEIVE_STATUS_INPROGRESS && s.receiveFlowPending &&
           IsoTpTimeAfter(isotp_user_get_us(),s.receiveFlowDeadline))
            receiveError(s,3,s.metadata.Timestamp+(isotp_user_get_us()-s.startedAt));
        isotp_poll(&s.link);
        if(s.link.receive_protocol_result==ISOTP_PROTOCOL_RESULT_TIMEOUT_CR) {
            receiveError(s,3,s.metadata.Timestamp+(isotp_user_get_us()-s.startedAt));
            s.link.receive_protocol_result=ISOTP_PROTOCOL_RESULT_OK;
        }
    }
    if(sending && !contains(sending->id)) isotp_poll(&sending->link);
    if(sending && !completed && sending->link.send_status==ISOTP_SEND_STATUS_ERROR) {
        const auto result=sending->link.send_protocol_result;
        complete(result==ISOTP_PROTOCOL_RESULT_TIMEOUT_BS ? ERR_TIMEOUT : ERR_FAILED,0);
    }
}
void IsoTransport::acknowledged(uint32_t timestamp) {
    if(frames.empty()) return;
    auto frame=frames.front(); frames.pop_front(); auto& s=*frame.session;
    if(s.pending) --s.pending; s.link.send_pending=s.pending!=0;
    if(frame.flow) {
        const auto current=sessions.find(s.id);
        if(frame.data[s.link.address_length]==0x30 && current!=sessions.end() && current->second==frame.session &&
           frame.receiveGeneration==s.receiveGeneration && frame.receiveOffset==s.receiveFlowOffset &&
           s.link.receive_status==ISOTP_RECEIVE_STATUS_INPROGRESS && s.receiveFlowPending) {
            if(IsoTpTimeAfter(isotp_user_get_us(),frame.confirmDeadline)) receiveError(s,3,s.metadata.Timestamp+(isotp_user_get_us()-s.startedAt));
            else {
                s.receiveFlowDeadline=0; s.receiveFlowPending=false;
                // A CF may be delivered before its FC confirmation. Preserve
                // that CF's N_Cr deadline rather than extending it at the ACK.
                if(!s.link.receive_timer_cr) {
                    s.link.receive_timer_cr=isotp_user_get_us()+s.link.receive_timeout_us;
                    s.receiveWireTimestamp=timestamp; s.receiveWireTimerActive=true;
                }
            }
        }
    } else {
        s.sendWireTimestamp=timestamp; s.sendWireTimerActive=true;
        s.link.send_timer_st=isotp_user_get_us()+s.link.send_st_min_us;
        s.link.send_timer_bs=isotp_user_get_us()+s.link.response_timeout_us;
        if(sending==frame.session && !s.pending && s.link.send_status==ISOTP_SEND_STATUS_IDLE) complete(STATUS_NOERROR,timestamp);
    }
}
void IsoTransport::failed(long error) {
    if(frames.empty()) return;
    auto frame=frames.front(); frames.pop_front(); auto& s=*frame.session;
    if(s.pending) --s.pending; s.link.send_pending=s.pending!=0;
    if(!frame.flow && sending==frame.session) { s.link.send_status=ISOTP_SEND_STATUS_ERROR; complete(error,0); }
    else if(frame.flow && frame.receiveGeneration==s.receiveGeneration && s.link.receive_status==ISOTP_RECEIVE_STATUS_INPROGRESS) {
        abortReceive(s); s.link.receive_protocol_result=ISOTP_PROTOCOL_RESULT_ERROR;
    }
}
bool IsoTransport::result(long& error,uint32_t& timestamp) {
    if(!completed) return false;
    error=sendError; timestamp=sendTimestamp;
    if(sending) {
        auto session=sending;
        frames.erase(std::remove_if(frames.begin(),frames.end(),[&](const IsoFrame& f){return f.session==session && !f.flow;}),frames.end());
        session->pending=std::count_if(frames.begin(),frames.end(),[&](const IsoFrame& f){return f.session==session;});
        session->link.send_pending=session->pending!=0; session->link.send_status=ISOTP_SEND_STATUS_IDLE;
    }
    sending.reset(); completed=false; return true;
}
void IsoTransport::clearRx(bool wireSubmitted) {
    received.clear(); overflow=false;
    for(auto& item:sessions) {
        auto& s=*item.second; ++s.receiveGeneration; s.link.receive_timer_cr=0; s.receiveFlowDeadline=0; s.receiveFlowPending=false; s.receiveWireTimerActive=false;
        s.link.receive_status=ISOTP_RECEIVE_STATUS_IDLE; s.link.receive_protocol_result=ISOTP_PROTOCOL_RESULT_OK; s.link.fc_pending=0;
    }
    // An already submitted FC belongs to TX until its ACK. RX clearing may
    // discard future FCs, but must not revoke that physical transmission.
    bool first=true;
    frames.erase(std::remove_if(frames.begin(),frames.end(),[&](const IsoFrame& f){
        const bool keep=first && wireSubmitted; first=false;
        if(f.flow && !keep) { if(f.session->pending) --f.session->pending; f.session->link.send_pending=f.session->pending!=0; return true; }
        return false;
    }),frames.end());
}
void IsoTransport::clearTx() {
    // Cancelling an unconfirmed FC must not leave RX waiting without a timer.
    for(auto& item:sessions) if(item.second->link.receive_status==ISOTP_RECEIVE_STATUS_INPROGRESS && item.second->receiveFlowPending)
        abortReceive(*item.second);
    for(auto& frame:frames) { frame.session->pending=0; frame.session->link.send_pending=0; }
    frames.clear();
    if(sending) { sending->link.send_status=ISOTP_SEND_STATUS_IDLE; sending.reset(); }
    completed=false;
}
void IsoTransport::cancelSend() {
    long error; uint32_t timestamp;
    if(sending) { complete(ERR_TIMEOUT,0); result(error,timestamp); }
}
}
extern "C" int isotp_user_send_can(uint32_t id,const uint8_t* bytes,uint8_t size,uint8_t flags,void* arg) {
    auto* session=static_cast<bmj::IsoSession*>(arg);
    return session->owner->enqueue(session,id,bytes,size,flags);
}
extern "C" uint32_t isotp_user_get_us() {
    using Clock=std::chrono::steady_clock;
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}
extern "C" void isotp_user_debug(const char*,...) {}
