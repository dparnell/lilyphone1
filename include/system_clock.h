#ifndef __SYSTEM_CLOCK_H__
#define __SYSTEM_CLOCK_H__

/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
/*********************************************************************************
 *                                  TYPEDEFS
 * *******************************************************************************/
/* Ordered by preference: a source may set the clock only when it is at least as
 * good as the one that set it last, so a satellite fix overrides network time
 * but not the other way round. Each source may always refresh its own. */
typedef enum {
    CLOCK_SRC_NONE = 0,
    CLOCK_SRC_NETWORK,  // NITZ, read back off the modem with AT+CCLK?
    CLOCK_SRC_GPS,      // satellite time
} clock_source_t;

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
/* Sets the clock from a UTC calendar time. The conversion is done arithmetically
 * rather than with mktime(), which reads its input as local time and would be
 * wrong by the offset once a time zone is in force.
 *
 * `day` may be 0 or one past the end of the month, which a caller converting a
 * local time to UTC gets for free when the subtraction steps over midnight.
 *
 * Returns true if the time was applied. */
bool system_clock_set_utc(clock_source_t src, int year, int month, int day,
                          int hour, int minute, int second);

/* Applies the local offset in minutes east of Greenwich (+600 for UTC+10), so
 * that localtime_r() renders local time. Only the network knows this - a GPS
 * fix carries UTC and nothing about where the user is. */
void system_clock_set_utc_offset(int minutes);

/* Epoch seconds for a UTC calendar time, or 0 when it is out of range. Same
 * conversion and same relaxed `day` as system_clock_set_utc, for callers that
 * want a timestamp rather than to move the clock. */
uint32_t system_clock_epoch_from_utc(int year, int month, int day,
                                     int hour, int minute, int second);

bool            system_clock_is_set(void);
clock_source_t  system_clock_get_source(void);
const char     *system_clock_source_name(void);
/* Minutes east of Greenwich, or 0 when no offset has been reported. */
int             system_clock_get_utc_offset(void);
bool            system_clock_has_utc_offset(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
