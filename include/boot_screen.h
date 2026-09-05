#ifndef __BOOT_SCREEN_H__
#define __BOOT_SCREEN_H__

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
/* The systems the boot screen accounts for, in the order they are brought up.
 *
 * This is a presentation list rather than the peripheral enum: it covers things
 * that are not peripherals at all (the filesystem, the mesh) and leaves out one
 * that is but never starts (the microphone). Twelve of them, which is what fits
 * the grid four across.
 */
enum {
    BOOT_DISPLAY = 0,
    BOOT_STORAGE,
    BOOT_TOUCH,
    BOOT_KEYBOARD,
    BOOT_CHARGER,
    BOOT_GAUGE,
    BOOT_SDCARD,
    BOOT_GPS,
    BOOT_MOTION,
    BOOT_LIGHT,
    BOOT_MODEM,
    BOOT_MESH,
    BOOT_SYSTEM_MAX,
};

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
/* Draws the grid with every system still to come. Call once the panel is up -
 * this is drawn straight onto it with GxEPD2, because LVGL does not exist yet. */
void boot_screen_begin(void);

/* About to start this one. Names it at the foot of the screen, which is the
 * part worth having: the slow steps are where a phone looks like it has hung,
 * and this says which one it is waiting on. */
void boot_screen_busy(int system);

/* Records how it went, and passes the result back so a call site can stay one
 * line: `peri_init_st[x] = boot_screen_done(BOOT_x, thing_init());` */
bool boot_screen_done(int system, bool ok);

/* The last paint before the UI takes the panel over. */
void boot_screen_finish(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
