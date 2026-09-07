#ifndef __STORE_EXPORT_H__
#define __STORE_EXPORT_H__

/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
/*********************************************************************************
 *                                   DEFINES
 * *******************************************************************************/
// Where exports land on the card, and how much of a result there is to report.
#define STORE_EXPORT_DIR    "/lilyphone"
#define STORE_EXPORT_DETAIL 96

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
/* Writes the contacts and the message log to the SD card as CSV, under a name
 * stamped with the time so an export never overwrites an earlier one.
 *
 * CSV rather than the TSV the device keeps internally, because the point of an
 * export is to open it somewhere else, and a spreadsheet will not ask questions
 * about a quoted field. Message bodies carry the sender's own line breaks, and
 * those survive inside the quotes.
 *
 * `detail` receives either what was written or why nothing was, and is what the
 * screen shows. Returns false when there is no card, or the card is not
 * writable, or there is nothing to write.
 */
bool store_export_to_sd(char *detail, int detail_len);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
