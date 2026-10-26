// BUSMUST Windows J2534 04.04 interface. See third_party/ for upstream notices.
#pragma once
#include <windows.h>
#pragma pack(push, 1)
#include "third_party/op2j2534/j2534.h"
#define ISO15765_FRAME_PAD 0x40UL
#define CAN_PS 0x8004UL
#define ISO15765_PS 0x8005UL
#define FD_CAN_PS 0x8011UL
#define FD_ISO15765_PS 0x8012UL
#define CAN_FD_PS 0x1000DUL
#define ISO15765_FD_PS 0x1000CUL
#define FD_CAN_BRS 0x80000UL
#define FD_CAN_FORMAT 0x100000UL
#define FD_CAN_ESI 0x200000UL
#define CAN_FD_BRS 0x1000000UL
#define CAN_FD_FORMAT 0x2000000UL
#define CAN_FD_ESI 0x4000000UL
#define RX_MSG_TRUNCATED 0x8000000UL
#define ERROR_IND 0x10UL
#define CAN_MIXED_FORMAT 0x8000UL
#define J1962_PINS 0x8001UL
#define FD_CAN_DATA_PHASE_RATE 0x805CUL
#define FD_ISO15765_TX_DATA_LENGTH 0x805DUL
#define HS_CAN_TERMINATION 0x805EUL
#define N_CR_MAX 0x805FUL
#define ISO15765_PAD_VALUE 0x8060UL
#define CAN_FD_DATA_PHASE_RATE 0x10010UL
#define CAN_FD_TC_DATA_LENGTH 0x10011UL
#define CAN_FD_TERMINATION 0x10012UL
#define CAN_FD_TYPE 0x10013UL
#define N_CR_MAX_GM 0x10014UL
#define GET_DEVICE_INFO 0x800CUL
#define GET_PROTOCOL_INFO 0x800DUL
typedef struct { unsigned long Parameter, Value, Supported; } SPARAM;
typedef struct { unsigned long NumOfParams; SPARAM* ParamPtr; } SPARAM_LIST;
#pragma pack(pop)
#ifdef __cplusplus
extern "C" {
#endif
long WINAPI PassThruOpen(void*, unsigned long*);
long WINAPI PassThruClose(unsigned long);
long WINAPI PassThruConnect(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long*);
long WINAPI PassThruDisconnect(unsigned long);
long WINAPI PassThruReadMsgs(unsigned long, PASSTHRU_MSG*, unsigned long*, unsigned long);
long WINAPI PassThruWriteMsgs(unsigned long, PASSTHRU_MSG*, unsigned long*, unsigned long);
long WINAPI PassThruStartPeriodicMsg(unsigned long, PASSTHRU_MSG*, unsigned long*, unsigned long);
long WINAPI PassThruStopPeriodicMsg(unsigned long, unsigned long);
long WINAPI PassThruStartMsgFilter(unsigned long, unsigned long, PASSTHRU_MSG*, PASSTHRU_MSG*, PASSTHRU_MSG*, unsigned long*);
long WINAPI PassThruStopMsgFilter(unsigned long, unsigned long);
long WINAPI PassThruSetProgrammingVoltage(unsigned long, unsigned long, unsigned long);
long WINAPI PassThruReadVersion(unsigned long, char*, char*, char*);
long WINAPI PassThruGetLastError(char*);
long WINAPI PassThruIoctl(unsigned long, unsigned long, void*, void*);
#ifdef __cplusplus
}
#endif
