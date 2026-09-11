#ifndef __CALC_H__
#define __CALC_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A calculator with four personalities over one keypad.
 *
 * The screen hands every press in as the text on the button - "7", "sin",
 * "x<>y" - and reads four lines back to draw. Nothing here knows what a widget
 * is, which is what lets the same engine answer the physical keyboard and the
 * on-screen pad alike, and lets a recorded RPN program be replayed through the
 * exact path a finger would take.
 *
 * Modes:
 *   SIMPLE      - algebraic with the four operations and percent.
 *   SCI         - the same engine with precedence, brackets, trig, logs and
 *                 powers. Trig works in degrees or radians (the "DEG" key).
 *   PROG        - 64-bit two's complement integers in hex, decimal, octal or
 *                 binary, with bitwise operators and shifts, at a chosen word
 *                 width. "WID" cycles 8 / 16 / 32 / 64 bits.
 *   RPN         - a four-level stack (X Y Z T) with LASTx, ten registers, and a
 *                 keystroke program memory in the HP style: "PRGM" records, "R/S"
 *                 runs, "LBL n" / "GTO n" jump, "x=0?" and friends skip the next
 *                 step when false.
 *
 * Key vocabulary (all ASCII, since the mono fonts hold nothing else):
 *   digits "0".."9", hex "A".."F", ".", "EEX", "+/-", "BS", "CLR"
 *   "+", "-", "*", "/", "mod", "y^x" (or "^"), "=", "(", ")", "%"
 *   "sin" "cos" "tan" "asin" "acos" "atan" "ln" "log" "e^x" "10^x"
 *   "x^2" "sqrt" "1/x" "n!" "pi" "e" "DEG" "INV"
 *   "HEX" "DEC" "OCT" "BIN" "WID" "and" "or" "xor" "not" "<<" ">>"
 *   "ENT" "x<>y" "Rv" "LSTx" "CLx" "STO" "RCL" (each followed by a digit)
 *   "PRGM" "R/S" "SST" "BST" "CLP" "LBL" "GTO" "x=0?" "x<0?" "x=y?" "x<y?"
 */
enum {
    CALC_MODE_SIMPLE = 0,
    CALC_MODE_SCI,
    CALC_MODE_PROG,
    CALC_MODE_RPN,
    CALC_MODE_MAX,
};

void        calc_init(void);
int         calc_mode(void);
int         calc_base(void);             // 2, 8, 10 or 16, in the programmer mode
void        calc_set_mode(int mode);     // carries the current value across
const char *calc_mode_name(int mode);
const char *calc_mode_short(int mode);   // three letters, for a small button

/* One press. Unknown keys are ignored. */
void calc_key(const char *key);

/* What to draw, top to bottom. Every string is owned by the engine and valid
 * until the next call into it. */
const char *calc_line_status(void);   // annunciators: "DEG INV", "HEX 32-bit", "PRGM 012"
const char *calc_line_info(void);     // the value in another base; Z and T
const char *calc_line_top(void);      // the expression so far; the Y register
const char *calc_line_main(void);     // what is being typed, or the result; X
bool        calc_error(void);         // the main line is an error message

/* "INV" is held: the screen shows the inverse functions on its keys. */
bool calc_inv(void);

/* The RPN program, for listing it. */
int         calc_prog_len(void);
const char *calc_prog_step(int i);
int         calc_prog_pc(void);
bool        calc_prog_recording(void);

#ifdef __cplusplus
}
#endif
#endif
