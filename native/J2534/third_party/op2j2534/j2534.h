/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 CMS DriveTech AB
 *
 * SAE J2534-1 v04.04 API definitions.
 *
 * These are the definitions of an industry interface specification (SAE
 * J2534); they are reproduced here for interoperability, not copied from any
 * vendor implementation. */
#ifndef OP2_J2534_H
#define OP2_J2534_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- protocol IDs ---- */
#define J1850VPW        1
#define J1850PWM        2
#define ISO9141         3
#define ISO14230        4
#define CAN             5
#define ISO15765        6
#define SCI_A_ENGINE    7
#define SCI_A_TRANS     8
#define SCI_B_ENGINE    9
#define SCI_B_TRANS     10

/* ---- error codes ---- */
#define STATUS_NOERROR              0x00
#define ERR_NOT_SUPPORTED           0x01
#define ERR_INVALID_CHANNEL_ID      0x02
#define ERR_INVALID_PROTOCOL_ID     0x03
#define ERR_NULL_PARAMETER          0x04
#define ERR_INVALID_IOCTL_VALUE     0x05
#define ERR_INVALID_FLAGS           0x06
#define ERR_FAILED                  0x07
#define ERR_DEVICE_NOT_CONNECTED    0x08
#define ERR_TIMEOUT                 0x09
#define ERR_INVALID_MSG             0x0A
#define ERR_INVALID_TIME_INTERVAL   0x0B
#define ERR_EXCEEDED_LIMIT          0x0C
#define ERR_INVALID_MSG_ID          0x0D
#define ERR_DEVICE_IN_USE           0x0E
#define ERR_INVALID_IOCTL_ID        0x0F
#define ERR_BUFFER_EMPTY            0x10
#define ERR_BUFFER_FULL             0x11
#define ERR_BUFFER_OVERFLOW         0x12
#define ERR_PIN_INVALID             0x13
#define ERR_CHANNEL_IN_USE          0x14
#define ERR_MSG_PROTOCOL_ID         0x15
#define ERR_INVALID_FILTER_ID       0x16
#define ERR_NO_FLOW_CONTROL         0x17
#define ERR_NOT_UNIQUE              0x18
#define ERR_INVALID_BAUDRATE        0x19
#define ERR_INVALID_DEVICE_ID       0x1A

/* ---- ioctl IDs ---- */
#define GET_CONFIG                          0x01
#define SET_CONFIG                          0x02
#define READ_VBATT                          0x03
#define FIVE_BAUD_INIT                      0x04
#define FAST_INIT                           0x05
#define CLEAR_TX_BUFFER                     0x07
#define CLEAR_RX_BUFFER                     0x08
#define CLEAR_PERIODIC_MSGS                 0x09
#define CLEAR_MSG_FILTERS                   0x0A
#define CLEAR_FUNCT_MSG_LOOKUP_TABLE        0x0B
#define ADD_TO_FUNCT_MSG_LOOKUP_TABLE       0x0C
#define DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE  0x0D
#define READ_PROG_VOLTAGE                   0x0E

/* ---- config parameter IDs ---- */
#define DATA_RATE           0x01
#define LOOPBACK            0x03
#define NODE_ADDRESS        0x04
#define NETWORK_LINE        0x05
#define P1_MIN              0x06
#define P1_MAX              0x07
#define P2_MIN              0x08
#define P2_MAX              0x09
#define P3_MIN              0x0A
#define P3_MAX              0x0B
#define P4_MIN              0x0C
#define P4_MAX              0x0D
#define W1                  0x0E
#define W2                  0x0F
#define W3                  0x10
#define W4                  0x11
#define W5                  0x12
#define TIDLE               0x13
#define TINIL               0x14
#define TWUP                0x15
#define PARITY              0x16
#define BIT_SAMPLE_POINT    0x17
#define SYNC_JUMP_WIDTH     0x18
#define W0                  0x19
#define T1_MAX              0x1A
#define T2_MAX              0x1B
#define T4_MAX              0x1C
#define T5_MAX              0x1D
#define ISO15765_BS         0x1E
#define ISO15765_STMIN      0x1F
#define DATA_BITS           0x20
#define FIVE_BAUD_MOD       0x21
#define BS_TX               0x22
#define STMIN_TX            0x23
#define T3_MAX              0x24
#define ISO15765_WFT_MAX    0x25

/* ---- connect flags ---- */
#define CAN_29BIT_ID            0x00000100
#define ISO9141_NO_CHECKSUM     0x00000200
#define CAN_ID_BOTH             0x00000800
#define ISO9141_K_LINE_ONLY     0x00001000

/* ---- RxStatus / TxFlags ---- */
#define TX_MSG_TYPE             0x00000001
#define START_OF_MESSAGE        0x00000002
#define ISO15765_FIRST_FRAME    0x00000002
#define RX_BREAK                0x00000004
#define TX_INDICATION           0x00000008
#define ISO15765_PADDING_ERROR  0x00000010
#define ISO15765_ADDR_TYPE      0x00000080
#define CAN_29BIT_ID_RXTX       0x00000100
#define WAIT_P3_MIN_ONLY        0x00000200
#define SW_CAN_HV_TX            0x00000400
#define SCI_MODE                0x00400000
#define SCI_TX_VOLTAGE          0x00800000

/* ---- filter types ---- */
#define PASS_FILTER             0x00000001
#define BLOCK_FILTER            0x00000002
#define FLOW_CONTROL_FILTER     0x00000003

/* ---- programming voltage ---- */
#define SHORT_TO_GROUND         0xFFFFFFFE
#define VOLTAGE_OFF             0xFFFFFFFF

#define J2534_MAX_DATA          4128

typedef struct {
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;
    unsigned long DataSize;
    unsigned long ExtraDataIndex;
    unsigned char Data[J2534_MAX_DATA];
} PASSTHRU_MSG;

typedef struct {
    unsigned long Parameter;
    unsigned long Value;
} SCONFIG;

typedef struct {
    unsigned long NumOfParams;
    SCONFIG *ConfigPtr;
} SCONFIG_LIST;

typedef struct {
    unsigned long NumOfBytes;
    unsigned char *BytePtr;
} SBYTE_ARRAY;

#ifdef __cplusplus
}
#endif
#endif /* OP2_J2534_H */
