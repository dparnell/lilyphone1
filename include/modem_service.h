#ifndef __MODEM_SERVICE_H__
#define __MODEM_SERVICE_H__

/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include <stdint.h>
#include <stdbool.h>
#include "phone_store.h"

#ifdef __cplusplus
extern "C" {
#endif
/*********************************************************************************
 *                                   DEFINES
 * *******************************************************************************/
#define MODEM_OPERATOR_LEN 24
// A UCS2 body is four hex characters per character of message.
#define SMS_HEX_BODY_MAX   704

/*********************************************************************************
 *                                  TYPEDEFS
 * *******************************************************************************/
typedef enum {
    MODEM_CALL_IDLE = 0,
    MODEM_CALL_INCOMING,  // ringing, not answered yet
    MODEM_CALL_DIALING,   // we dialled out, not connected yet
    MODEM_CALL_ACTIVE,    // connected
} modem_call_state_t;

typedef enum {
    MODEM_SEND_IDLE = 0,
    MODEM_SEND_BUSY,
    MODEM_SEND_OK,
    MODEM_SEND_FAILED,
} modem_send_state_t;

typedef struct {
    char     number[CONTACT_NUMBER_LEN];
    char     text[SMS_TEXT_LEN];
    uint32_t ts;
} modem_sms_rx_t;

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
/* Takes ownership of SerialAT and starts the task that owns it. Nothing else
 * may touch SerialAT afterwards - route AT traffic through modem_request_at().
 * `alive` says whether the modem answered during power-up; when it did not the
 * service still runs so the modem can come back later. */
void modem_service_init(bool alive);

// --- calls ---
modem_call_state_t modem_get_call_state(void);
/* Copies the other party's number for the current call. Empty when the network
 * withheld it or no call is up. */
void     modem_get_call_number(char *buf, int len);
/* Milliseconds since the current call connected, 0 when it has not. */
uint32_t modem_get_call_duration(void);
void     modem_dial(const char *number);
void     modem_answer(void);
void     modem_hangup(void);

// --- messages ---
/* Queues an SMS. Returns a send id to hand to modem_get_send_state(), or 0 if
 * the request could not be queued. */
uint32_t modem_send_sms(const char *number, const char *text);
modem_send_state_t modem_get_send_state(uint32_t send_id);

/* Pops one received message, oldest first. Returns false when none is waiting.
 * Called from the LVGL task so that only that task writes to the store. */
bool modem_poll_sms(modem_sms_rx_t *out);

// --- network ---
bool     modem_is_registered(void);
/* 0..31 as reported by AT+CSQ, or 99 when unknown. */
uint8_t  modem_get_signal(void);
void     modem_get_operator(char *buf, int len);

/* Plays a short notification tone through the modem's audio output.
 *
 * The modem is the only speaker path on this board: the I2S pins in
 * utilities.h (BOARD_I2S_BCLK/DOUT/LRC, 7/8/9) are the same pins as the
 * modem's RI, ITR and RST lines, so driving an external DAC there would reset
 * the modem. Whether a speaker is actually fitted, and whether this firmware
 * implements AT+CPTONE, is not something the code can find out - an
 * unsupported module simply answers ERROR and nothing happens. */
void     modem_play_tone(void);

/* Runs a raw AT command on the modem task and logs the reply to the monitor.
 * Used by the screens that poke at modem features directly. */
void     modem_request_at(const char *cmd);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
