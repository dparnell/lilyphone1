#ifndef __PHONE_STORE_H__
#define __PHONE_STORE_H__

/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
/*********************************************************************************
 *                                   DEFINES
 * *******************************************************************************/
#define CONTACT_NAME_LEN    24
#define CONTACT_NUMBER_LEN  24
#define CONTACTS_MAX        64

#define SMS_TEXT_LEN        161 // one concatenated GSM-7 segment plus terminator
#define SMS_MAX             120

// sms_msg_t::dir
#define SMS_DIR_IN          0
#define SMS_DIR_OUT         1

// sms_msg_t::status
#define SMS_ST_OK           0
#define SMS_ST_PENDING      1
#define SMS_ST_FAILED       2

/*********************************************************************************
 *                                  TYPEDEFS
 * *******************************************************************************/
typedef struct {
    char name[CONTACT_NAME_LEN];
    char number[CONTACT_NUMBER_LEN];
} contact_t;

typedef struct {
    char     number[CONTACT_NUMBER_LEN];
    char     text[SMS_TEXT_LEN];
    uint32_t ts;      // seconds since the epoch, 0 when the clock was not set
    uint8_t  dir;     // SMS_DIR_*
    uint8_t  status;  // SMS_ST_*
    uint8_t  unread;
} sms_msg_t;

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
/* Allocates the in-RAM tables and reads both files back from SPIFFS. Safe to
 * call more than once; later calls are ignored. */
bool phone_store_init(void);

// --- contacts, kept sorted by name so the list screen never has to sort ---
int              contacts_count(void);
const contact_t *contacts_get(int idx);
/* Returns the index the contact ended up at, or -1 when the book is full or
 * the number is blank. An existing entry with the same number is updated. */
int              contacts_add(const char *name, const char *number);
bool             contacts_update(int idx, const char *name, const char *number);
bool             contacts_remove(int idx);
int              contacts_find_by_number(const char *number);
/* The contact name for a number, or the number itself when it is unknown. */
const char      *contacts_display_name(const char *number);
bool             contacts_save(void);

// --- messages, newest last ---
int              sms_count(void);
const sms_msg_t *sms_get(int idx);
/* Appends a message, evicting the oldest one when the log is full. Returns the
 * index it was stored at, or -1 if it could not be stored. */
int              sms_add(const char *number, const char *text, uint32_t ts, int dir, int status, bool unread);
bool             sms_set_status(int idx, int status);
bool             sms_save(void);

/* Conversations are the distinct numbers in the log, most recently active
 * first. Threads are recomputed on demand, so any index is only valid until
 * the log changes. */
int              sms_thread_count(void);
const char      *sms_thread_number(int thread);
const sms_msg_t *sms_thread_last(int thread);
int              sms_thread_unread(const char *number);
int              sms_thread_msg_count(const char *number);
const sms_msg_t *sms_thread_msg(const char *number, int idx);
void             sms_thread_mark_read(const char *number);
void             sms_thread_delete(const char *number);
int              sms_unread_total(void);

/* Numbers arrive from the network in many shapes ("+61412...", "0412...").
 * Two numbers match when their last MATCH_DIGITS digits agree. */
bool             phone_number_match(const char *a, const char *b);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
