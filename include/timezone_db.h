#ifndef __TIMEZONE_DB_H__
#define __TIMEZONE_DB_H__

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
#define TIMEZONE_NAME_MAX  32  // longest is America/Argentina/Buenos_Aires
#define TIMEZONE_POSIX_MAX 48

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
/* Accessors over the zone table in timezone_names.h, which is a single newline
 * separated string of IANA names alongside a parallel array of POSIX TZ strings.
 * Keeping the table behind these means the 17KB of it is included once. */
int         timezone_count(void);
bool        timezone_name_at(int idx, char *buf, int len);
/* The POSIX TZ string for `idx`, DST rules included, or NULL when out of range. */
const char *timezone_posix_at(int idx);
int         timezone_find(const char *name);

/* Fills `out` with the indexes of zones matching `filter`, which is matched
 * case insensitively as a substring, treating underscores and slashes in the
 * name as spaces so "new york" finds America/New_York.
 *
 * Returns how many indexes were written, and sets `total` to how many matched
 * altogether so a caller that caps the list can say how many it left out. An
 * empty filter matches everything. */
int         timezone_search(const char *filter, int *out, int max, int *total);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
