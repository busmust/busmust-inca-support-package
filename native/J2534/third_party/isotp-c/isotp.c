////////////////////////////////////////////////////////////////////////
//                  ___ ___  ___ _____ ___      ___                   //
//                 |_ _/ __|/ _ \_   _| _ \___ / __|                  //
//                  | |\__ \ (_) || | |  _/___| (__                   //
//                 |___|___/\___/ |_| |_|      \___|                  //
//                                                                    //
////////////////////////////////////////////////////////////////////////

#include <assert.h>
#include <stdint.h>

#include "isotp.h"

///////////////////////////////////////////////////////
///                 STATIC FUNCTIONS                ///
///////////////////////////////////////////////////////

/* CAN frame data lengths (CAN_DL) which may be used by CAN FD frames larger than
 * a Classical CAN frame. CAN FD frames can only carry these lengths, so frames
 * exceeding 8 bytes always have to be padded up to the next valid length.
 */
static const uint8_t isotp_can_fd_frame_sizes[] = {12, 16, 20, 24, 32, 48, 64};

/* Returns the smallest valid CAN_DL which is able to carry length bytes of data */
static uint8_t isotp_ceil_can_dl(uint8_t length) {
    if (length <= ISOTP_CAN_DL_CLASSIC) {
        /* every length up to 8 bytes maps directly to a DLC */
        return length;
    }

    for (uint8_t i = 0; i < (uint8_t)(sizeof(isotp_can_fd_frame_sizes) / sizeof(isotp_can_fd_frame_sizes[0])); ++i) {
        if (length <= isotp_can_fd_frame_sizes[i]) { return isotp_can_fd_frame_sizes[i]; }
    }

    return isotp_can_fd_frame_sizes[sizeof(isotp_can_fd_frame_sizes) / sizeof(isotp_can_fd_frame_sizes[0]) - 1];
}

/* Returns logic true if length is a CAN_DL which can be transmitted on a CAN(-FD) bus */
static uint8_t isotp_is_valid_can_dl(uint8_t length) { return (uint8_t)(length == isotp_ceil_can_dl(length)); }

/* Figure 9 checks CAN_DL before processing PCI or changing receive state. */
int isotp_validate_receive_frame(const IsoTpLink* link, const uint8_t* data, uint8_t len) {
    const uint8_t can_dl = (uint8_t)(len + link->address_length);
    if (len < 2 || can_dl > ISO_TP_MAX_CAN_FRAME_SIZE || !isotp_is_valid_can_dl(can_dl)) { return ISOTP_RET_LENGTH; }

    if ((data[0] >> 4) == ISOTP_PCI_TYPE_SINGLE) {
        const uint8_t compact_length = data[0] & 0x0F;
        if (can_dl <= ISOTP_CAN_DL_CLASSIC) {
            return compact_length && compact_length <= len - 1u ? ISOTP_RET_OK : ISOTP_RET_LENGTH;
        }
        /* Larger SFs use the escape byte and the smallest CAN FD DLC that
         * holds the PDU (ISO 15765-2:2016 Table 13). */
        if (compact_length || data[1] <= ISOTP_CAN_DL_CLASSIC - 1u - link->address_length || data[1] > len - 2u ||
            isotp_ceil_can_dl((uint8_t)(data[1] + 2u + link->address_length)) != can_dl) { return ISOTP_RET_LENGTH; }
        return ISOTP_RET_OK;
    }
    if ((data[0] >> 4) == ISOTP_PCI_TYPE_FIRST_FRAME) {
        uint32_t size;
        if (can_dl < ISOTP_CAN_DL_CLASSIC) { return ISOTP_RET_LENGTH; }
        size = ((uint32_t)(data[0] & 0x0F) << 8) | data[1];
        if (!size) {
            size = ((uint32_t)data[2] << 24) | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 8) | data[5];
            if (size <= 4095u) { return ISOTP_RET_LENGTH; }
        }
        return size > ISOTP_SF_MAX_PAYLOAD(can_dl) - link->address_length ? ISOTP_RET_OK : ISOTP_RET_LENGTH;
    }
    if ((data[0] >> 4) == ISOTP_PCI_TYPE_CONSECUTIVE_FRAME) {
        uint32_t remaining, capacity;
        if (link->receive_status != ISOTP_RECEIVE_STATUS_INPROGRESS || link->rx_dl < 2 || link->receive_offset >= link->receive_size) { return ISOTP_RET_LENGTH; }
        remaining = link->receive_size - link->receive_offset;
        capacity = (uint32_t)link->rx_dl - 1u;
        if (len > link->rx_dl || (remaining > capacity && len != link->rx_dl) ||
            (remaining < capacity ? remaining : capacity) > len - 1u) { return ISOTP_RET_LENGTH; }
        if (remaining <= capacity && can_dl > ISOTP_CAN_DL_CLASSIC &&
            isotp_ceil_can_dl((uint8_t)(remaining + 1u + link->address_length)) != can_dl) { return ISOTP_RET_LENGTH; }
        return ISOTP_RET_OK;
    }
    return ISOTP_RET_LENGTH;
}

/* Returns the TX_DL to use for the given link, falling back to Classical CAN for
 * links which weren't initialised through isotp_init_link()
 */
static uint8_t isotp_tx_dl(const IsoTpLink* link) {
    if (link->tx_dl < ISOTP_CAN_DL_CLASSIC || link->tx_dl > ISO_TP_MAX_CAN_FRAME_SIZE || !isotp_is_valid_can_dl(link->tx_dl)) { return ISOTP_CAN_DL_CLASSIC; }

    return link->tx_dl;
}

#ifdef ISO_TP_USER_SEND_CAN_FLAGS
/* Returns the frame format flags the user shim has to transmit the frames of this link with.
 * Links using a TX_DL of more than 8 bytes require CAN FD frames, so all of their frames are
 * flagged accordingly, irrespective of the length of the individual frame.
 */
static uint8_t isotp_frame_flags(const IsoTpLink* link) {
    uint8_t flags = link->frame_flags;
    return flags;
}
#endif

/* Pads a frame containing used_length bytes up to a transmittable CAN_DL and
 * returns the resulting frame length.
 *
 * Frames of more than 8 bytes are always padded, as CAN FD only supports a
 * discrete set of frame lengths. Smaller frames are only padded if
 * ISO_TP_FRAME_PADDING is enabled.
 */
static uint8_t isotp_pad_frame(const IsoTpLink* link, IsoTpCanMessage* message, uint8_t used_length) {
    uint8_t frame_length = (uint8_t)(used_length + link->address_length);
    if (link->padding && frame_length < ISOTP_CAN_DL_CLASSIC) frame_length = ISOTP_CAN_DL_CLASSIC;
    frame_length = (uint8_t)(isotp_ceil_can_dl(frame_length) - link->address_length);
    if (frame_length > used_length) memset(message->as.data_array.ptr + used_length, link->pad_value, frame_length - used_length);

    return frame_length;
}

/* st_min to microsecond */
static uint8_t isotp_us_to_st_min(uint32_t us) {
    // ISO 15765-2:2016 defines STmin encoding:
    // 0x00..0x7F: value in milliseconds (0..127 ms)
    // 0xF1..0xF9: value in 100 microsecond steps (100..900 us)
    const uint32_t STMIN_MS_MAX = 127000;      // 127 ms in us
    const uint32_t STMIN_US_MIN = 100;         // 100 us
    const uint32_t STMIN_US_MAX = 900;         // 900 us
    const uint8_t  STMIN_US_BASE = 0xF0;       // base for 100us steps

    if (us <= STMIN_MS_MAX) {
        if (us >= STMIN_US_MIN && us <= STMIN_US_MAX) {
            return (uint8_t)(STMIN_US_BASE + (us / 100));
        } else {
            return (uint8_t)(us / 1000u);
        }
    }

    return 0;
}

/* st_min to usec  */
static uint32_t isotp_st_min_to_us(uint8_t st_min) {
    // ISO 15765-2:2016 defines STmin encoding:
    // 0x00..0x7F: value in milliseconds (0..127 ms)
    // 0xF1..0xF9: value in 100 microsecond steps (100..900 us)
    const uint8_t  STMIN_MS_MAX      = 0x7F;   // 127 ms
    const uint8_t  STMIN_US_MIN_CODE = 0xF1;   // 100 us
    const uint8_t  STMIN_US_MAX_CODE = 0xF9;   // 900 us
    const uint8_t  STMIN_US_BASE     = 0xF0;   // base for 100us steps
    const uint32_t US_PER_MS         = 1000;
    const uint32_t US_STEP           = 100;

    if (st_min <= STMIN_MS_MAX) {
        return st_min * US_PER_MS;
    } else if (st_min >= STMIN_US_MIN_CODE && st_min <= STMIN_US_MAX_CODE) {
        return (st_min - STMIN_US_BASE) * US_STEP;
    }
    return 127000;
}

static int isotp_send_flow_control(IsoTpLink* link, uint8_t flow_status, uint8_t block_size, uint32_t st_min_us) {
    IsoTpCanMessage message;
    (void)memset(&message, 0, sizeof(message));
    int             ret;
    uint8_t         size = 0;

    /* setup message  */
    message.as.flow_control.type  = ISOTP_PCI_TYPE_FLOW_CONTROL_FRAME;
    message.as.flow_control.FS    = flow_status;
    message.as.flow_control.BS    = block_size;
    message.as.flow_control.STmin = isotp_us_to_st_min(st_min_us);

    /* send message */
    /* FC uses a full 8-byte wire frame even when the concurrent data transfer
     * has a larger FD TX_DL; its frame format comes from the received PDU. */
    IsoTpLink fc_config = *link;
    fc_config.tx_dl = 8;
    fc_config.padding = 1;
    size = isotp_pad_frame(&fc_config, &message, 3);

    ret = isotp_user_send_can(link->send_arbitration_id, message.as.data_array.ptr, size
#if defined(ISO_TP_USER_SEND_CAN_FLAGS)
                              , link->receive_frame_flags
#endif
#if defined(ISO_TP_USER_SEND_CAN_ARG)
                              , link->user_send_can_arg
#endif
    );

    link->fc_pending = ret == ISOTP_RET_NOSPACE;
    link->fc_status = flow_status; link->fc_bs = block_size; link->fc_st_min_us = st_min_us;
    if (ret != ISOTP_RET_OK && ret != ISOTP_RET_NOSPACE) {
        link->receive_status = ISOTP_RECEIVE_STATUS_IDLE;
        link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_ERROR;
    }
    return ret;
}

static int isotp_send_single_frame(const IsoTpLink* link, uint32_t id) {
    IsoTpCanMessage message;
    int             ret;
    uint8_t         size = 0;

    (void)memset(&message, 0, sizeof(message));

    /* payloads which don't fit into a single frame must be segmented */
    assert(link->send_size <= (ISOTP_SF_MAX_PAYLOAD(isotp_tx_dl(link)) - link->address_length));

    /* setup message  */
    if (link->send_size + link->address_length < ISOTP_CAN_DL_CLASSIC) {
        message.as.single_frame.type  = ISOTP_PCI_TYPE_SINGLE;
        message.as.single_frame.SF_DL = (uint8_t)link->send_size;
        (void)memcpy(message.as.single_frame.data, link->send_buffer, link->send_size);

        size = isotp_pad_frame(link, &message, (uint8_t)(link->send_size + 1u));
    } else { // ISO15765-2:2016, CAN FD only
        /* setup message using the SF_DL escape sequence */
        message.as.single_frame_escape.type        = ISOTP_PCI_TYPE_SINGLE;
        message.as.single_frame_escape.set_to_zero = 0;
        message.as.single_frame_escape.SF_DL       = (uint8_t)link->send_size;
        (void)memcpy(message.as.single_frame_escape.data, link->send_buffer, link->send_size);

        size = isotp_pad_frame(link, &message, (uint8_t)(link->send_size + 2u));
    }

    /* send message using the identifier requested by the caller, which
     * isotp_send_with_id() may override for a functional request */
    ret = isotp_user_send_can(id, message.as.data_array.ptr, size
#if defined(ISO_TP_USER_SEND_CAN_FLAGS)
                              , isotp_frame_flags(link)
#endif
#if defined(ISO_TP_USER_SEND_CAN_ARG)
                              , link->user_send_can_arg
#endif
    );

    return ret;
}
static int isotp_send_first_frame(IsoTpLink* link, uint32_t id) {
    IsoTpCanMessage message;
    int             ret   = 0;
    const uint8_t   tx_dl = isotp_tx_dl(link);
    uint32_t        data_length;

    (void)memset(&message, 0, sizeof(message));

    /* payloads which fit into a single frame must not be segmented */
    assert(link->send_size > (ISOTP_SF_MAX_PAYLOAD(tx_dl) - link->address_length));

    /* first frames always use the full frame length of the sender (TX_DL) */
    if (link->send_size <= 4095) {
        /* setup 'short' message */
        data_length                             = (uint32_t)tx_dl - link->address_length - 2u;
        message.as.first_frame_short.type       = ISOTP_PCI_TYPE_FIRST_FRAME;
        message.as.first_frame_short.FF_DL_low  = (uint8_t)link->send_size;
        message.as.first_frame_short.FF_DL_high = (uint8_t)(0x0F & (link->send_size >> 8));
        (void)memcpy(message.as.first_frame_short.data, link->send_buffer, data_length);
    } else { // ISO15765-2:2016
        /* setup 'long' message */
        data_length                                  = (uint32_t)tx_dl - link->address_length - 6u;
        message.as.first_frame_long.set_to_zero_high = 0;
        message.as.first_frame_long.set_to_zero_low  = 0;
        message.as.first_frame_long.type             = ISOTP_PCI_TYPE_FIRST_FRAME;
        message.as.first_frame_long.FF_DL            = LE32TOH(link->send_size);
        (void)memcpy(message.as.first_frame_long.data, link->send_buffer, data_length);
    }

    /* send message */
    ret = isotp_user_send_can(id, message.as.data_array.ptr, (uint8_t)(tx_dl - link->address_length)
#if defined(ISO_TP_USER_SEND_CAN_FLAGS)
                              , isotp_frame_flags(link)
#endif
#if defined(ISO_TP_USER_SEND_CAN_ARG)
                              , link->user_send_can_arg
#endif
    );

    if (ISOTP_RET_OK == ret) { link->send_offset += data_length; }

    link->send_sn = 1;

    return ret;
}

static int isotp_send_consecutive_frame(IsoTpLink* link) {
    IsoTpCanMessage message;
    uint32_t        data_length;
    uint32_t        max_data_length;
    int             ret;
    uint8_t         size = 0;

    (void)memset(&message, 0, sizeof(message));

    /* payloads which fit into a single frame must not be segmented */
    assert(link->send_size > (ISOTP_SF_MAX_PAYLOAD(isotp_tx_dl(link)) - link->address_length));

    /* setup message  */
    message.as.consecutive_frame.type = ISOTP_PCI_TYPE_CONSECUTIVE_FRAME;
    message.as.consecutive_frame.SN   = link->send_sn;
    max_data_length                   = (uint32_t)isotp_tx_dl(link) - link->address_length - 1u;
    data_length                       = link->send_size - link->send_offset;
    if (data_length > max_data_length) { data_length = max_data_length; }
    (void)memcpy(message.as.consecutive_frame.data, link->send_buffer + link->send_offset, data_length);

    /* send message */
    size = isotp_pad_frame(link, &message, (uint8_t)(data_length + 1u));

    ret = isotp_user_send_can(link->send_arbitration_id, message.as.data_array.ptr, size
#if defined(ISO_TP_USER_SEND_CAN_FLAGS)
                              , isotp_frame_flags(link)
#endif
#if defined(ISO_TP_USER_SEND_CAN_ARG)
                              , link->user_send_can_arg
#endif
    );

    if (ISOTP_RET_OK == ret) {
        link->send_offset += data_length;
        if (++(link->send_sn) > 0x0F) { link->send_sn = 0; }
    }

    return ret;
}

static int isotp_receive_single_frame(IsoTpLink* link, const IsoTpCanMessage* message, uint8_t len) {
    const uint8_t* payload;
    uint32_t       payload_length;
    uint32_t       max_payload_length;

    if (0 == message->as.single_frame.SF_DL) {
        payload            = message->as.single_frame_escape.data;
        payload_length     = message->as.single_frame_escape.SF_DL;
        max_payload_length = (uint32_t)len - 2u;
    } else {
        payload            = message->as.single_frame.data;
        payload_length     = message->as.single_frame.SF_DL;
        max_payload_length = (uint32_t)len - 1u;
    }

    /* check data length */
    if ((0 == payload_length) || (payload_length > max_payload_length)) {
        isotp_user_debug("Single-frame length too small.");
        return ISOTP_RET_LENGTH;
    }

    if (payload_length > link->receive_buf_size) {
        isotp_user_debug("Single-frame message too large for receiving buffer.");
        return ISOTP_RET_OVERFLOW;
    }

    /* copying data */
    (void)memcpy(link->receive_buffer, payload, payload_length);
    link->receive_size   = payload_length;
    link->receive_offset = link->receive_size;

#ifdef ISO_TP_ENABLE_STREAMING
    link->receive_stream_size       = link->receive_size;
    link->receive_streaming         = 0;
    link->receive_stream_carry_size = 0;
#endif

    return ISOTP_RET_OK;
}

static int isotp_receive_first_frame(IsoTpLink* link, IsoTpCanMessage* message, uint8_t len) {
    const uint8_t* first_frame_data;
    uint8_t        is_long_packet = 0;
    uint32_t       first_frame_data_length;
    uint32_t       payload_length;

    /* first frames are sent using the full frame length of the sender, which
     * determines the frame length of the following consecutive frames (RX_DL)
     */
    if (len + link->address_length < ISOTP_CAN_DL_CLASSIC || !isotp_is_valid_can_dl((uint8_t)(len + link->address_length))) {
        isotp_user_debug("First frame should be a full CAN frame of at least 8 bytes in length.");
        return ISOTP_RET_LENGTH;
    }

    /* check data length */
    payload_length = message->as.first_frame_short.FF_DL_high;
    payload_length = (payload_length << 8) + message->as.first_frame_short.FF_DL_low;

    /* if length is ZERO we get a long message > 4095bytes of payload */
    if (payload_length == 0) {
        is_long_packet          = 1;
        payload_length          = LE32TOH(message->as.first_frame_long.FF_DL);
        first_frame_data_length = (uint32_t)len - 6u;
    } else {
        first_frame_data_length = (uint32_t)len - 2u;
    }

    /* should not use multiple frame transmition */
    if (payload_length <= (ISOTP_SF_MAX_PAYLOAD(len + link->address_length) - link->address_length)) {
        isotp_user_debug("Should not use multiple frame transmission.");
        return ISOTP_RET_LENGTH;
    }

#ifndef ISO_TP_ENABLE_STREAMING
    if (payload_length > link->receive_buf_size) {
        isotp_user_debug("Multi-frame response too large for receiving buffer.");
        return ISOTP_RET_OVERFLOW;
    }
#else
    if (link->receive_buf_size == 0) {
        isotp_user_debug("Receiving buffer must not be empty.");
        return ISOTP_RET_OVERFLOW;
    }

    link->receive_streaming         = payload_length > link->receive_buf_size;
    link->receive_stream_size       = 0;
    link->receive_stream_carry_size = 0;
#endif

    /* copying data */
    if (is_long_packet) {
        first_frame_data = message->as.first_frame_long.data;
    } else {
        first_frame_data = message->as.first_frame_short.data;
    }

#ifdef ISO_TP_ENABLE_STREAMING
    if (first_frame_data_length > link->receive_buf_size) {
        (void)memcpy(link->receive_buffer, first_frame_data, link->receive_buf_size);
        link->receive_stream_size       = link->receive_buf_size;
        link->receive_stream_carry_size = (uint8_t)(first_frame_data_length - link->receive_buf_size);
        (void)memcpy(link->receive_stream_carry, first_frame_data + link->receive_buf_size, link->receive_stream_carry_size);
    } else {
        (void)memcpy(link->receive_buffer, first_frame_data, first_frame_data_length);
        link->receive_stream_size = first_frame_data_length;
    }
#else
    (void)memcpy(link->receive_buffer, first_frame_data, first_frame_data_length);
#endif

    link->receive_offset = first_frame_data_length;
    link->receive_size   = payload_length;
    link->receive_sn     = 1;
    link->rx_dl          = len;

    return ISOTP_RET_OK;
}

static int isotp_receive_consecutive_frame(IsoTpLink* link, const IsoTpCanMessage* message, uint8_t len) {
    uint32_t remaining_bytes;
    uint32_t max_data_length;

    /* check sn */
    if (link->receive_sn != message->as.consecutive_frame.SN) { return ISOTP_RET_WRONG_SN; }

    /* consecutive frames use the frame length announced by the first frame (RX_DL) */
    max_data_length = (uint32_t)link->rx_dl - 1u;

    /* CAN_DL was checked before the sequence number or any state mutation. */
    remaining_bytes = link->receive_size - link->receive_offset;
    if (remaining_bytes > max_data_length) { remaining_bytes = max_data_length; }
    (void)len;

#ifdef ISO_TP_ENABLE_STREAMING
    if (link->receive_streaming) {
        uint32_t available = link->receive_buf_size - link->receive_stream_size;
        uint32_t copy_size = remaining_bytes < available ? remaining_bytes : available;

        (void)memcpy(link->receive_buffer + link->receive_stream_size, message->as.consecutive_frame.data, copy_size);
        link->receive_stream_size += copy_size;

        link->receive_stream_carry_size = (uint8_t)(remaining_bytes - copy_size);
        if (link->receive_stream_carry_size > 0) {
            (void)memcpy(link->receive_stream_carry, message->as.consecutive_frame.data + copy_size, link->receive_stream_carry_size);
        }
    } else
#endif
    {
        /* copying data */
        (void)memcpy(link->receive_buffer + link->receive_offset, message->as.consecutive_frame.data, remaining_bytes);
    }

    link->receive_offset += remaining_bytes;
    if (++(link->receive_sn) > 0x0F) { link->receive_sn = 0; }

    return ISOTP_RET_OK;
}

static int isotp_receive_flow_control_frame(IsoTpLink* link, IsoTpCanMessage* message, uint8_t len) {
    /* unused args */
    (void)link;
    (void)message;

    /* check message length */
    if (len < 3) {
        isotp_user_debug("Flow control frame too short.");
        return ISOTP_RET_LENGTH;
    }

    return ISOTP_RET_OK;
}

///////////////////////////////////////////////////////
///                 PUBLIC FUNCTIONS                ///
///////////////////////////////////////////////////////

int isotp_send(IsoTpLink* link, const uint8_t payload[], uint32_t size) { return isotp_send_with_id(link, link->send_arbitration_id, payload, size); }

int isotp_send_with_id(IsoTpLink* link, uint32_t id, const uint8_t payload[], uint32_t size) {
    int ret;

    if (link == 0x0) {
        isotp_user_debug("Link is null!");
        return ISOTP_RET_ERROR;
    }

    if (size > link->send_buf_size) {
        isotp_user_debug("Message size too large. Increase ISO_TP_MAX_MESSAGE_SIZE to set a larger buffer\n");

#ifndef ISO_TP_NO_FORMATTED_ERRORS
        char    message[ISOTP_MAX_ERROR_MSG_SIZE] = {0};
        int32_t writtenChars = snprintf(&message[0], ISOTP_MAX_ERROR_MSG_SIZE, "Attempted to send %u bytes; max size is %u!\n", (unsigned int)size,
                                        (unsigned int)link->send_buf_size);

        assert(writtenChars <= ISOTP_MAX_ERROR_MSG_SIZE);
        (void)writtenChars;

        isotp_user_debug(message);
#endif
        return ISOTP_RET_OVERFLOW;
    }

    if (ISOTP_SEND_STATUS_INPROGRESS == link->send_status) {
        isotp_user_debug("Abort previous message, transmission in progress.\n");
        return ISOTP_RET_INPROGRESS;
    }

    /* copy into local buffer */
    link->send_size   = size;
    link->send_offset = 0;
    (void)memcpy(link->send_buffer, payload, size);

    if (link->send_size <= (ISOTP_SF_MAX_PAYLOAD(isotp_tx_dl(link)) - link->address_length)) {
        /* send single frame */
        ret = isotp_send_single_frame(link, id);
#ifdef ISO_TP_TRANSMIT_COMPLETE_CALLBACK
        if (ret == ISOTP_RET_OK && link->tx_done_cb) { link->tx_done_cb(link, link->send_size, link->tx_done_cb_arg); }
#endif
    } else {
        /* send multi-frame */
        ret = isotp_send_first_frame(link, id);

        /* init multi-frame control flags */
        if (ISOTP_RET_OK == ret) {
            link->send_bs_remain       = 0;
            link->send_st_min_us       = 0;
            link->send_st_min_reserved = 0;
            link->send_wtf_count       = 0;
            link->send_timer_st        = isotp_user_get_us();
            link->send_timer_bs        = isotp_user_get_us() + link->response_timeout_us;
            link->send_protocol_result = ISOTP_PROTOCOL_RESULT_OK;
            link->send_status          = ISOTP_SEND_STATUS_INPROGRESS;
        }
    }

    return ret;
}

void isotp_on_can_message(IsoTpLink* link, const uint8_t* data, uint8_t len) {
    IsoTpCanMessage message;
    int             ret;

    if (len < 2 || len + link->address_length > ISO_TP_MAX_CAN_FRAME_SIZE ||
        !isotp_is_valid_can_dl((uint8_t)(len + link->address_length))) { return; }
    if ((data[0] >> 4) <= ISOTP_PCI_TYPE_CONSECUTIVE_FRAME && isotp_validate_receive_frame(link, data, len) != ISOTP_RET_OK) { return; }

    memcpy(message.as.data_array.ptr, data, len);
    memset(message.as.data_array.ptr + len, 0, sizeof(message.as.data_array.ptr) - len);

    switch (message.as.common.type) {
        case ISOTP_PCI_TYPE_SINGLE: {
            /* update protocol result */
            if (ISOTP_RECEIVE_STATUS_INPROGRESS == link->receive_status) {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_UNEXP_PDU;
            } else {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_OK;
            }

            /* handle message */
            ret = isotp_receive_single_frame(link, &message, len);

            if (ISOTP_RET_OVERFLOW == ret) {
                /* update protocol result */
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_BUFFER_OVFLW;
            } else if (ISOTP_RET_OK == ret) {
                /* change status */
                link->receive_status = ISOTP_RECEIVE_STATUS_FULL;
            }
            break;
        }
        case ISOTP_PCI_TYPE_FIRST_FRAME: {
            /* update protocol result */
            if (ISOTP_RECEIVE_STATUS_INPROGRESS == link->receive_status) {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_UNEXP_PDU;
            } else {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_OK;
            }

            /* handle message */
            ret = isotp_receive_first_frame(link, &message, len);

            /* if overflow happened */
            if (ISOTP_RET_OVERFLOW == ret) {
                /* update protocol result */
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_BUFFER_OVFLW;
                /* change status */
                link->receive_status = ISOTP_RECEIVE_STATUS_IDLE;
                /* send error message */
                isotp_send_flow_control(link, PCI_FLOW_STATUS_OVERFLOW, 0, 0);
                break;
            }

            /* if receive successful */
            if (ISOTP_RET_OK == ret) {
                /* The asynchronous shim pauses this timer until FC confirmation;
                 * a synchronous/passive shim retains the immediate deadline. */
                link->receive_timer_cr = isotp_user_get_us() + link->receive_timeout_us;
                /* change status and send fc frame */
#ifdef ISO_TP_ENABLE_STREAMING
                if (link->receive_streaming && link->receive_stream_size >= link->receive_buf_size) {
                    link->receive_status = ISOTP_RECEIVE_STATUS_FULL;
                } else {
                    link->receive_status = ISOTP_RECEIVE_STATUS_INPROGRESS;
                    link->receive_bs_count = link->receive_streaming ? 1 : link->block_size;
                    isotp_send_flow_control(link, PCI_FLOW_STATUS_CONTINUE, link->receive_bs_count, link->st_min_us);
                }
#else
                link->receive_status   = ISOTP_RECEIVE_STATUS_INPROGRESS;
                link->receive_bs_count = link->block_size;
                isotp_send_flow_control(link, PCI_FLOW_STATUS_CONTINUE, link->receive_bs_count, link->st_min_us);
#endif
            }

            break;
        }
        case ISOTP_PCI_TYPE_CONSECUTIVE_FRAME: {
            /* check if in receiving status */
            if (ISOTP_RECEIVE_STATUS_INPROGRESS != link->receive_status) {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_UNEXP_PDU;
                break;
            }

            /* handle message */
            ret = isotp_receive_consecutive_frame(link, &message, len);

            /* if wrong sn */
            if (ISOTP_RET_WRONG_SN == ret) {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_WRONG_SN;
                link->receive_status          = ISOTP_RECEIVE_STATUS_IDLE;
                break;
            }

            if (ISOTP_RET_LENGTH == ret) {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_ERROR;
                link->receive_status = ISOTP_RECEIVE_STATUS_IDLE;
                break;
            }
            /* if success */
            if (ISOTP_RET_OK == ret) {
                /* refresh timer cs */
                link->receive_timer_cr = isotp_user_get_us() + link->receive_timeout_us;

                /* receive finished */
                if (link->receive_offset >= link->receive_size
#ifdef ISO_TP_ENABLE_STREAMING
                    || (link->receive_streaming && link->receive_stream_size >= link->receive_buf_size)
#endif
                ) {
                    link->receive_status = ISOTP_RECEIVE_STATUS_FULL;
                } else {
                    /* send fc when bs reaches limit */
                    if (link->block_size && 0 == --link->receive_bs_count) {
                        link->receive_bs_count =
#ifdef ISO_TP_ENABLE_STREAMING
                            link->receive_streaming ? 1 :
#endif
                            link->block_size;
                        isotp_send_flow_control(link, PCI_FLOW_STATUS_CONTINUE, link->receive_bs_count, link->st_min_us);
                    }
                }
            }

            break;
        }
        case ISOTP_PCI_TYPE_FLOW_CONTROL_FRAME:
            /* handle fc frame only when sending in progress  */
            if (ISOTP_SEND_STATUS_INPROGRESS != link->send_status) { break; }

            /* handle message */
            ret = isotp_receive_flow_control_frame(link, &message, len);

            if (ISOTP_RET_OK == ret) {
                /* refresh bs timer */
                link->send_timer_bs = isotp_user_get_us() + link->response_timeout_us;

                /* overflow */
                if (PCI_FLOW_STATUS_OVERFLOW == message.as.flow_control.FS) {
                    link->send_protocol_result = ISOTP_PROTOCOL_RESULT_BUFFER_OVFLW;
                    link->send_status          = ISOTP_SEND_STATUS_ERROR;
                }

                /* wait */
                else if (PCI_FLOW_STATUS_WAIT == message.as.flow_control.FS) {
                    link->send_wtf_count += 1;
                    /* wait exceed allowed count */
                    if (link->send_wtf_count > link->wft_max) {
                        link->send_protocol_result = ISOTP_PROTOCOL_RESULT_WFT_OVRN;
                        link->send_status          = ISOTP_SEND_STATUS_ERROR;
                    }
                }

                /* permit send */
                else if (PCI_FLOW_STATUS_CONTINUE == message.as.flow_control.FS) {
                    const uint8_t st_min = (uint8_t)(link->stmin_tx == 0xFFFF ? message.as.flow_control.STmin : link->stmin_tx);
                    if (0 == (link->bs_tx == 0xFFFF ? message.as.flow_control.BS : link->bs_tx)) {
                        link->send_bs_remain = ISOTP_INVALID_BS;
                    } else {
                        link->send_bs_remain = link->bs_tx == 0xFFFF ? message.as.flow_control.BS : link->bs_tx;
                    }
                    if (link->stmin_tx == 0xFFFF && st_min > 0x7F && (st_min < 0xF1 || st_min > 0xF9)) {
                        link->send_st_min_reserved = 1;
                    }
                    link->send_st_min_us = link->send_st_min_reserved ? 127000 : isotp_st_min_to_us(st_min);
                    link->send_timer_st = isotp_user_get_us() + link->send_st_min_us;
                    link->send_wtf_count       = 0;
                } else {
                    link->send_protocol_result = ISOTP_PROTOCOL_RESULT_INVALID_FS;
                    link->send_status = ISOTP_SEND_STATUS_ERROR;
                }
            }
            break;
        default: break;
    };

#ifdef ISO_TP_RECEIVE_COMPLETE_CALLBACK
    /* Notify user via callback if registered */
    if (link->receive_status == ISOTP_RECEIVE_STATUS_FULL && link->rx_done_cb != NULL
#ifdef ISO_TP_ENABLE_STREAMING
        && !link->receive_streaming
#endif
    ) {
        link->rx_done_cb(link, link->receive_buffer, link->receive_size, link->rx_done_cb_arg);
        link->receive_status = ISOTP_RECEIVE_STATUS_IDLE;
    }
#endif
    return;
}

int isotp_receive(IsoTpLink* link, uint8_t* payload, const uint32_t payload_size, uint32_t* out_size) {
    uint32_t copylen;

#ifdef ISO_TP_RECEIVE_COMPLETE_CALLBACK
    /* If callback is registered, isotp_receive should not be used */
    if (link->rx_done_cb != NULL) { return ISOTP_RET_ERROR; /* Callback mode active, use callback instead */ }
#endif

#ifdef ISO_TP_ENABLE_STREAMING
    if (link->receive_streaming) { return ISOTP_RET_ERROR; }
#endif

    if (ISOTP_RECEIVE_STATUS_FULL != link->receive_status) { return ISOTP_RET_NO_DATA; }

    copylen = link->receive_size;
    if (copylen > payload_size) { copylen = payload_size; }

    memcpy(payload, link->receive_buffer, copylen);
    *out_size            = copylen;

    link->receive_status = ISOTP_RECEIVE_STATUS_IDLE;

    return ISOTP_RET_OK;
}

#ifdef ISO_TP_ENABLE_STREAMING
int isotp_receive_streaming(IsoTpLink* link, uint8_t* payload, const uint32_t payload_size, uint32_t* out_size, bool* is_complete) {
    uint32_t copylen;

    if (link == NULL || payload == NULL || out_size == NULL || is_complete == NULL) { return ISOTP_RET_ERROR; }
    if (ISOTP_RECEIVE_STATUS_FULL != link->receive_status) { return ISOTP_RET_NO_DATA; }

    copylen = link->receive_streaming ? link->receive_stream_size : link->receive_size;
    if (payload_size < copylen) { return ISOTP_RET_NOSPACE; }

    (void)memcpy(payload, link->receive_buffer, copylen);
    *out_size    = copylen;
    *is_complete = link->receive_offset >= link->receive_size && link->receive_stream_carry_size == 0;

    if (!link->receive_streaming || *is_complete) {
        link->receive_status    = ISOTP_RECEIVE_STATUS_IDLE;
        link->receive_streaming = 0;
        return ISOTP_RET_OK;
    }

    link->receive_stream_size = link->receive_stream_carry_size;
    if (link->receive_stream_size > link->receive_buf_size) {
        link->receive_stream_size = link->receive_buf_size;
    }
    if (link->receive_stream_size > 0) {
        (void)memcpy(link->receive_buffer, link->receive_stream_carry, link->receive_stream_size);
        link->receive_stream_carry_size -= (uint8_t)link->receive_stream_size;
        if (link->receive_stream_carry_size > 0) {
            (void)memmove(link->receive_stream_carry, link->receive_stream_carry + link->receive_stream_size, link->receive_stream_carry_size);
        }
    }

    if (link->receive_stream_carry_size > 0 || link->receive_offset >= link->receive_size) {
        link->receive_status = ISOTP_RECEIVE_STATUS_FULL;
    } else {
        link->receive_status   = ISOTP_RECEIVE_STATUS_INPROGRESS;
        link->receive_bs_count = 1;
        link->receive_timer_cr = isotp_user_get_us() + link->receive_timeout_us;
        isotp_send_flow_control(link, PCI_FLOW_STATUS_CONTINUE, link->receive_bs_count, link->st_min_us);
    }

    return ISOTP_RET_OK;
}
#endif

void isotp_init_link(IsoTpLink* link, uint32_t sendid, uint8_t* sendbuf, uint32_t sendbufsize, uint8_t* recvbuf, uint32_t recvbufsize) {
    memset(link, 0, sizeof(*link));
    link->receive_status      = ISOTP_RECEIVE_STATUS_IDLE;
    link->send_status         = ISOTP_SEND_STATUS_IDLE;
    link->send_arbitration_id = sendid;
    link->send_buffer         = sendbuf;
    link->send_buf_size       = sendbufsize;
    link->receive_buffer      = recvbuf;
    link->receive_buf_size    = recvbufsize;
    link->tx_dl               = ISO_TP_DEFAULT_TX_DL;
    link->rx_dl               = ISOTP_CAN_DL_CLASSIC;
    link->pad_value = 0; link->block_size = 0; link->wft_max = 0;
    link->bs_tx = 0xFFFF; link->stmin_tx = 0xFFFF;
    link->response_timeout_us = 1000000; link->receive_timeout_us = 1000000;

#ifdef ISO_TP_TRANSMIT_COMPLETE_CALLBACK
    link->tx_done_cb     = NULL;
    link->tx_done_cb_arg = NULL;
#endif

#ifdef ISO_TP_RECEIVE_COMPLETE_CALLBACK
    link->rx_done_cb     = NULL;
    link->rx_done_cb_arg = NULL;
#endif

    return;
}

int isotp_set_tx_dl(IsoTpLink* link, uint8_t tx_dl) {
    if (link == NULL) {
        isotp_user_debug("Link is null!");
        return ISOTP_RET_ERROR;
    }

    if (tx_dl < ISOTP_CAN_DL_CLASSIC || tx_dl > ISO_TP_MAX_CAN_FRAME_SIZE || !isotp_is_valid_can_dl(tx_dl)) {
#ifndef ISO_TP_NO_FORMATTED_ERRORS
        char    message[ISOTP_MAX_ERROR_MSG_SIZE] = {0};
        int32_t writtenChars =
            snprintf(&message[0], ISOTP_MAX_ERROR_MSG_SIZE, "Invalid TX_DL of %u bytes; must be a CAN frame length between 8 and %u!\n",
                     (unsigned int)tx_dl, (unsigned int)ISO_TP_MAX_CAN_FRAME_SIZE);

        assert(writtenChars <= ISOTP_MAX_ERROR_MSG_SIZE);
        (void)writtenChars;

        isotp_user_debug(message);
#else
        isotp_user_debug("Invalid TX_DL; must be a valid CAN frame length.\n");
#endif
        return ISOTP_RET_ERROR;
    }

    if (ISOTP_SEND_STATUS_INPROGRESS == link->send_status) {
        isotp_user_debug("Cannot change TX_DL while a transmission is in progress.\n");
        return ISOTP_RET_INPROGRESS;
    }

    link->tx_dl = tx_dl;

    return ISOTP_RET_OK;
}

uint8_t isotp_get_tx_dl(const IsoTpLink* link) {
    if (link == NULL) {
        isotp_user_debug("Link is null!");
        return 0;
    }

    return isotp_tx_dl(link);
}

void isotp_destroy_link(IsoTpLink* link) {
    if (link == NULL) { return; }

    // Clear callbacks
#ifdef ISO_TP_TRANSMIT_COMPLETE_CALLBACK
    link->tx_done_cb     = NULL;
    link->tx_done_cb_arg = NULL;
#endif

#ifdef ISO_TP_RECEIVE_COMPLETE_CALLBACK
    link->rx_done_cb     = NULL;
    link->rx_done_cb_arg = NULL;
#endif

    // Reset link state (optional, but good practice)
    memset(link, 0, sizeof(IsoTpLink));
}

void isotp_poll(IsoTpLink* link) {
    int ret;
    if (link->fc_pending) isotp_send_flow_control(link, link->fc_status, link->fc_bs, link->fc_st_min_us);

    /* only polling when operation in progress */
    if (ISOTP_SEND_STATUS_INPROGRESS == link->send_status && !link->send_pending) {
        /* continue send data */
        if (/* send data if bs_remain is invalid or bs_remain large than zero */
            (ISOTP_INVALID_BS == link->send_bs_remain || link->send_bs_remain > 0) &&
            /* and if st_min is zero or go beyond interval time */
            (0 == link->send_st_min_us || IsoTpTimeAfter(isotp_user_get_us(), link->send_timer_st))) {
            ret = isotp_send_consecutive_frame(link);
            if (ISOTP_RET_OK == ret) {
                if (ISOTP_INVALID_BS != link->send_bs_remain) { link->send_bs_remain -= 1; }
                link->send_timer_bs = isotp_user_get_us() + link->response_timeout_us;
                link->send_timer_st = isotp_user_get_us() + link->send_st_min_us;

                /* check if send finish */
                if (link->send_offset >= link->send_size) {
                    link->send_status = ISOTP_SEND_STATUS_IDLE;
#ifdef ISO_TP_TRANSMIT_COMPLETE_CALLBACK
                    if (link->tx_done_cb != NULL) { link->tx_done_cb(link, link->send_size, link->tx_done_cb_arg); }
#endif
                }
            } else if (ISOTP_RET_NOSPACE == ret) {
                /* shim reported that it isn't able to send a frame at present, retry on next call */
            } else {
                link->send_protocol_result = ISOTP_PROTOCOL_RESULT_ERROR;
                link->send_status = ISOTP_SEND_STATUS_ERROR;
            }
        }

        /* check timeout */
        if (IsoTpTimeAfter(isotp_user_get_us(), link->send_timer_bs)) {
            link->send_protocol_result = ISOTP_PROTOCOL_RESULT_TIMEOUT_BS;
            link->send_status          = ISOTP_SEND_STATUS_ERROR;
        }
    }

    /* only polling when operation in progress */
    if (ISOTP_RECEIVE_STATUS_INPROGRESS == link->receive_status) {
        /* check timeout */
        if ((link->receive_timer_cr > 0) && IsoTpTimeAfter(isotp_user_get_us(), link->receive_timer_cr)) {
            link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_TIMEOUT_CR;
            link->receive_status          = ISOTP_RECEIVE_STATUS_IDLE;
        }
    }

    return;
}

#ifdef ISO_TP_TRANSMIT_COMPLETE_CALLBACK
void isotp_set_tx_done_cb(IsoTpLink* link, isotp_tx_done_cb cb, void* arg) {
    if (link != NULL) {
        link->tx_done_cb     = cb;
        link->tx_done_cb_arg = arg;
    }
}
#endif

#ifdef ISO_TP_RECEIVE_COMPLETE_CALLBACK
void isotp_set_rx_done_cb(IsoTpLink* link, isotp_rx_done_cb cb, void* arg) {
    if (link != NULL) {
        link->rx_done_cb     = cb;
        link->rx_done_cb_arg = arg;
    }
}
#endif
