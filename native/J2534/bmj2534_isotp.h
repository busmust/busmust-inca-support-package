#pragma once
#include "bmj2534.h"
#include "third_party/isotp-c/isotp.h"
#include <array>
#include <deque>
#include <map>
#include <memory>

namespace bmj {
// SAE J2534-1 04.04 7.2.9 requires at least ten filters per protocol.
constexpr unsigned long ISO_FLOW_FILTER_LIMIT=10;
// SAE J2534-1 04.04 Figure 42: the base ISO15765 PDU is at most 4095 bytes.
constexpr unsigned long ISO_CLASSIC_MAX_PDU=4095;
struct IsoSettings {
    unsigned long bs=0, stmin=0, wft=0, bsTx=0xFFFF, stminTx=0xFFFF;
    unsigned long txDl=8, nCr=1000000, pad=0;
};
struct IsoSession;
struct IsoFrame {
    uint32_t id=0;
    unsigned long filterId=0;
    unsigned long flags=0;
    uint8_t size=0, data[64]={};
    uint64_t receiveGeneration=0;
    uint32_t receiveOffset=0;
    uint32_t confirmDeadline=0;
    bool flow=false;
    bool submitted=false;
    std::shared_ptr<IsoSession> session;
};
// All access belongs to the physical port's one RX/scheduler owner, under its
// channel mutex. The core never consumes BMAPI queues or opens a device.
class IsoTransport {
public:
    IsoSettings settings;
    long add(unsigned long id,const PASSTHRU_MSG& mask,const PASSTHRU_MSG& pattern,const PASSTHRU_MSG& flow,bool sendFlow=true);
    bool contains(unsigned long id) const;
    void remove(unsigned long id);
    void clearFilters();
    long validate(const PASSTHRU_MSG& message) const;
    size_t singleFrameCapacity(const PASSTHRU_MSG& message) const;
    long start(const PASSTHRU_MSG& message);
    void receive(const PASSTHRU_MSG& frame,uint32_t wireTimestamp);
    void poll();
    const IsoFrame* front() const { return frames.empty() ? nullptr : &frames.front(); }
    void acknowledged(uint32_t timestamp);
    void submitted() { if(!frames.empty()) frames.front().submitted=true; }
    void failed(long error);
    bool result(long& error,uint32_t& timestamp);
    void clearRx(bool wireSubmitted=false);
    void clearTx();
    void cancelSend();
    std::deque<PASSTHRU_MSG> received;
    bool overflow=false;
    int enqueue(IsoSession* session,uint32_t id,const uint8_t* bytes,uint8_t size,uint8_t flags);
private:
    std::map<unsigned long,std::shared_ptr<IsoSession>> sessions;
    std::deque<IsoFrame> frames;
    std::shared_ptr<IsoSession> sending;
    bool completed=false;
    long sendError=STATUS_NOERROR;
    uint32_t sendTimestamp=0;
    void complete(long error,uint32_t timestamp);
    void configure(IsoSession& session,const PASSTHRU_MSG& message);
    void collect(IsoSession& session,const PASSTHRU_MSG& frame,bool first);
    void receiveError(IsoSession& session,unsigned long code,unsigned long timestamp);
    void abortReceive(IsoSession& session);
};
}
