/**
 * The calculator engine. See calc.h for the shape of it.
 *
 * Two evaluators live here. The algebraic one - simple, scientific and
 * programmer modes - is a shunting-yard over a value stack and an operator
 * stack, so "2 + 3 * 4" comes out as 14 and brackets nest. The programmer mode
 * is the same evaluator running on 64-bit integers instead of doubles: the
 * stacks hold both and the mode says which half is live. The RPN evaluator is
 * the classic four-register stack, and its program memory is nothing more than
 * the keys that were pressed, replayed.
 *
 * Every key arrives as the text on its button, and every step of a program is
 * stored the same way, so the listing a user reads back is exactly what they
 * typed and there is no second vocabulary to keep in step with the first.
 */
#include "calc.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define ENTRY_MAX   22      // characters of a number being typed
#define STACK_MAX   16      // nesting depth of the algebraic evaluator
#define EXPR_MAX    120     // the expression text kept for the top line
#define LINE_MAX    48
#define PROG_MAX    200     // steps of RPN program memory
#define STEP_LEN    8       // the longest key name, plus a digit, plus NUL
#define RUN_MAX     20000   // steps before a running program is called runaway

#define CALC_PRG_PATH   "/calc.prg"
#define CALC_PREFS      "calc"

//************************************[ state ]*********************************
static int  mode    = CALC_MODE_SIMPLE;
static bool degrees = true;
static bool inv     = false;
static int  base    = 10;
static int  width   = 32;

static char entry[ENTRY_MAX + 1];
static bool entering = false;
static char err[LINE_MAX];      // non-empty while the main line shows an error

/* One value, both ways. The float modes read .f and the programmer mode reads
 * .i; both are kept current so a mode switch carries the number across. */
typedef struct { double f; int64_t i; } val_t;

// algebraic
static val_t vs[STACK_MAX];  static int vsp;
static char  os[STACK_MAX][4]; static int osp;
static val_t x;
static bool  op_pending;     // the last key was a binary operator
static bool  fresh;          // "=" was pressed; the next operand starts afresh
static bool  x_shown;        // x came out of a bracket, so it is in expr already
static char  expr[EXPR_MAX];

// rpn
static double rs[4];         // X Y Z T
static double lastx;
static double regs[10];
static bool   lift = true;   // whether the next number pushes the stack up
static char   pending[STEP_LEN];   // STO / RCL / GTO / LBL waiting for a digit
static char   prog[PROG_MAX][STEP_LEN];
static int    prog_len;
static int    pc;
static bool   recording;
static bool   running;
static char   test_note[8];  // "YES" / "NO" after a test key pressed by hand

// what the screen reads
static char out_status[LINE_MAX], out_info[LINE_MAX], out_top[LINE_MAX], out_main[LINE_MAX];

//************************************[ helpers ]*******************************
static void set_err(const char *what)
{
    snprintf(err, sizeof(err), "%s", what);
}

static int64_t wrap(int64_t v)
{
    if(width >= 64) return v;
    uint64_t m = (1ULL << width) - 1;
    uint64_t u = (uint64_t)v & m;
    if(u & (1ULL << (width - 1))) u |= ~m;   // sign extend to 64
    return (int64_t)u;
}

static void fmt_float(char *out, int n, double v)
{
    if(isnan(v))      { snprintf(out, n, "Error"); return; }
    if(isinf(v))      { snprintf(out, n, v < 0 ? "-Inf" : "Inf"); return; }
    if(v == 0.0) v = 0.0;   // no "-0"
    snprintf(out, n, "%.12g", v);
}

static void fmt_int(char *out, int n, int64_t v, int b)
{
    if(b == 10) { snprintf(out, n, "%lld", (long long)v); return; }

    uint64_t u = (uint64_t)v;
    if(width < 64) u &= (1ULL << width) - 1;

    char tmp[72];
    int  i = 0;
    if(u == 0) tmp[i++] = '0';
    while(u && i < (int)sizeof(tmp) - 1) {
        int d = (int)(u % (uint64_t)b);
        tmp[i++] = d < 10 ? '0' + d : 'A' + d - 10;
        u /= (uint64_t)b;
    }
    int j = 0;
    while(i > 0 && j < n - 1) out[j++] = tmp[--i];
    out[j] = '\0';
}

static void fmt_val(char *out, int n, val_t v)
{
    if(mode == CALC_MODE_PROG) fmt_int(out, n, v.i, base);
    else                       fmt_float(out, n, v.f);
}

/* A double too big for an int64 is undefined to cast, so it becomes zero on the
 * integer side rather than whatever the FPU felt like. */
static int64_t clamp_i(double f)
{
    if(!isfinite(f) || f > 9.2e18 || f < -9.2e18) return 0;
    return (int64_t)f;
}

static val_t val_f(double f) { val_t v; v.f = f; v.i = clamp_i(f); return v; }
static val_t val_i(int64_t i) { val_t v; v.i = wrap(i); v.f = (double)v.i; return v; }

static bool is_digit_key(const char *k)
{
    if(k[1] != '\0') return false;
    char c = k[0];
    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if(d < 0) return false;
    return d < (mode == CALC_MODE_PROG ? base : 10);
}

//************************************[ number entry ]**************************
static void entry_start(void)
{
    entry[0] = '\0';
    entering = true;
}

static void entry_append(char c)
{
    int l = strlen(entry);
    if(l < ENTRY_MAX) { entry[l] = c; entry[l + 1] = '\0'; }
}

static void entry_point(void)
{
    if(mode == CALC_MODE_PROG) return;
    if(strchr(entry, '.') || strchr(entry, 'e')) return;
    if(entry[0] == '\0') entry_append('0');
    entry_append('.');
}

static void entry_eex(void)
{
    if(mode == CALC_MODE_PROG) return;
    if(!entering) { entry_start(); entry_append('1'); }
    if(strchr(entry, 'e')) return;
    if(entry[0] == '\0') entry_append('1');
    entry_append('e');
}

/* Toggles the sign of the exponent if one is being typed, else of the number. */
static void entry_negate(void)
{
    char *e = strchr(entry, 'e');
    if(e) {
        if(e[1] == '-') memmove(e + 1, e + 2, strlen(e + 2) + 1);
        else { memmove(e + 2, e + 1, strlen(e + 1) + 1); e[1] = '-'; }
        return;
    }
    if(entry[0] == '-') memmove(entry, entry + 1, strlen(entry));
    else if(strlen(entry) < ENTRY_MAX) { memmove(entry + 1, entry, strlen(entry) + 1); entry[0] = '-'; }
}

static void entry_backspace(void)
{
    int l = strlen(entry);
    if(l > 0) entry[l - 1] = '\0';
    if(entry[0] == '\0' || !strcmp(entry, "-")) { entry[0] = '\0'; entering = false; }
}

static double entry_f(void)
{
    return strtod(entry, NULL);
}

static int64_t entry_i(void)
{
    bool neg = entry[0] == '-';
    uint64_t u = strtoull(neg ? entry + 1 : entry, NULL, base);
    return wrap(neg ? -(int64_t)u : (int64_t)u);
}

static val_t entry_val(void)
{
    if(mode == CALC_MODE_PROG) return val_i(entry_i());
    return val_f(entry_f());
}

//************************************[ maths ]*********************************
static double to_rad(double a) { return degrees ? a * M_PI / 180.0 : a; }
static double from_rad(double a) { return degrees ? a * 180.0 / M_PI : a; }

/* A unary function on a double. False, with the error set, when it cannot. */
static bool unary_f(const char *k, double v, double *r)
{
    double o;
    if(!strcmp(k, "sin"))       o = sin(to_rad(v));
    else if(!strcmp(k, "cos"))  o = cos(to_rad(v));
    else if(!strcmp(k, "tan"))  o = tan(to_rad(v));
    else if(!strcmp(k, "asin")) { if(v < -1 || v > 1) { set_err("Out of range"); return false; } o = from_rad(asin(v)); }
    else if(!strcmp(k, "acos")) { if(v < -1 || v > 1) { set_err("Out of range"); return false; } o = from_rad(acos(v)); }
    else if(!strcmp(k, "atan")) o = from_rad(atan(v));
    else if(!strcmp(k, "ln"))   { if(v <= 0) { set_err("Log of non-positive"); return false; } o = log(v); }
    else if(!strcmp(k, "log"))  { if(v <= 0) { set_err("Log of non-positive"); return false; } o = log10(v); }
    else if(!strcmp(k, "e^x"))  o = exp(v);
    else if(!strcmp(k, "10^x")) o = pow(10.0, v);
    else if(!strcmp(k, "x^2"))  o = v * v;
    else if(!strcmp(k, "sqrt")) { if(v < 0) { set_err("Root of negative"); return false; } o = sqrt(v); }
    else if(!strcmp(k, "1/x"))  { if(v == 0) { set_err("Divide by zero"); return false; } o = 1.0 / v; }
    else if(!strcmp(k, "n!")) {
        if(v < 0 || v > 170 || v != floor(v)) { set_err("n! needs 0 to 170"); return false; }
        o = 1; for(int i = 2; i <= (int)v; i++) o *= i;
    }
    else if(!strcmp(k, "%"))    o = v / 100.0;
    else if(!strcmp(k, "+/-"))  o = -v;
    else return false;

    if(isnan(o) || isinf(o)) { set_err("Out of range"); return false; }
    *r = o;
    return true;
}

static bool is_unary_f(const char *k)
{
    static const char *names[] = { "sin", "cos", "tan", "asin", "acos", "atan", "ln", "log",
                                   "e^x", "10^x", "x^2", "sqrt", "1/x", "n!", "%", NULL };
    for(int i = 0; names[i]; i++) if(!strcmp(k, names[i])) return true;
    return false;
}

/* a op b, in whichever arithmetic the mode uses. */
static bool binary(const char *op, val_t a, val_t b, val_t *r)
{
    if(mode == CALC_MODE_PROG) {
        int64_t p = a.i, q = b.i, v;
        if(!strcmp(op, "+"))        v = p + q;
        else if(!strcmp(op, "-"))   v = p - q;
        else if(!strcmp(op, "*"))   v = p * q;
        else if(!strcmp(op, "/"))   { if(q == 0) { set_err("Divide by zero"); return false; } v = p / q; }
        else if(!strcmp(op, "mod")) { if(q == 0) { set_err("Divide by zero"); return false; } v = p % q; }
        else if(!strcmp(op, "and")) v = p & q;
        else if(!strcmp(op, "or"))  v = p | q;
        else if(!strcmp(op, "xor")) v = p ^ q;
        else if(!strcmp(op, "<<"))  v = (q < 0 || q >= 64) ? 0 : (int64_t)((uint64_t)p << q);
        else if(!strcmp(op, ">>"))  v = (q < 0 || q >= 64) ? (p < 0 ? -1 : 0) : (p >> q);
        else return false;
        *r = val_i(v);
        return true;
    }

    double p = a.f, q = b.f, v;
    if(!strcmp(op, "+"))        v = p + q;
    else if(!strcmp(op, "-"))   v = p - q;
    else if(!strcmp(op, "*"))   v = p * q;
    else if(!strcmp(op, "/"))   { if(q == 0) { set_err("Divide by zero"); return false; } v = p / q; }
    else if(!strcmp(op, "mod")) { if(q == 0) { set_err("Divide by zero"); return false; } v = fmod(p, q); }
    else if(!strcmp(op, "^"))   v = pow(p, q);
    else return false;

    if(isnan(v) || isinf(v)) { set_err("Out of range"); return false; }
    *r = val_f(v);
    return true;
}

/* Binding strength, C's order for the bitwise ones so that a programmer's
 * expectations hold: "a + b << 1" shifts the sum. */
static int prec(const char *op)
{
    if(!strcmp(op, "or"))  return 1;
    if(!strcmp(op, "xor")) return 2;
    if(!strcmp(op, "and")) return 3;
    if(!strcmp(op, "<<") || !strcmp(op, ">>")) return 4;
    if(!strcmp(op, "+")  || !strcmp(op, "-"))  return 5;
    if(!strcmp(op, "*")  || !strcmp(op, "/") || !strcmp(op, "mod")) return 6;
    if(!strcmp(op, "^"))   return 7;
    return 0;
}

static bool is_binary(const char *k)
{
    return prec(k) > 0;
}

//************************************[ algebraic ]*****************************
static void alg_reset(void)
{
    vsp = osp = 0;
    x = val_f(0);
    op_pending = false;
    fresh = false;
    x_shown = false;
    expr[0] = '\0';
    entering = false;
    entry[0] = '\0';
}

static void expr_add(const char *s)
{
    int l = strlen(expr), n = strlen(s);
    if(l + n < EXPR_MAX) memcpy(expr + l, s, n + 1);
}

/* Takes whatever is being typed as the current value. */
static void alg_commit(void)
{
    if(entering) { x = entry_val(); entering = false; }
}

/* The current value becomes the next operand on the stack. */
static void alg_operand(void)
{
    alg_commit();
    if(fresh) { expr[0] = '\0'; fresh = false; }
    if(vsp < STACK_MAX) vs[vsp++] = x;

    /* A value that came out of a bracket is already written as the bracket. */
    if(x_shown) { x_shown = false; return; }

    char buf[LINE_MAX];
    fmt_val(buf, sizeof(buf), x);
    expr_add(buf);
}

static bool alg_reduce_top(void)
{
    if(osp <= 0 || !strcmp(os[osp - 1], "(")) return false;
    if(vsp < 2) { osp--; return true; }   // an operator with nothing to work on

    val_t b = vs[--vsp], a = vs[--vsp], r;
    if(!binary(os[osp - 1], a, b, &r)) return false;

    vs[vsp++] = r;
    osp--;
    return true;
}

static void alg_key(const char *k)
{
    if(is_digit_key(k)) {
        if(!entering) entry_start();
        if(fresh) { fresh = false; expr[0] = '\0'; }
        entry_append(k[0]);
        op_pending = false;
        x_shown = false;
        return;
    }
    if(!strcmp(k, "."))   { if(!entering) entry_start(); entry_point(); x_shown = false; return; }
    if(!strcmp(k, "EEX")) { entry_eex(); x_shown = false; return; }

    if(!strcmp(k, "+/-")) {
        if(entering) entry_negate();
        else if(mode == CALC_MODE_PROG) x = val_i(-x.i);
        else x = val_f(-x.f);
        return;
    }
    if(!strcmp(k, "BS")) {
        if(entering) entry_backspace();
        else x = val_f(0);
        return;
    }
    if(!strcmp(k, "CLR")) { alg_reset(); return; }

    if(!strcmp(k, "pi")) { alg_commit(); x = val_f(M_PI); x_shown = false; return; }
    if(!strcmp(k, "e"))  { alg_commit(); x = val_f(M_E);  x_shown = false; return; }

    if(!strcmp(k, "not")) { alg_commit(); x = val_i(~x.i); return; }

    if(is_unary_f(k)) {
        alg_commit();
        double r;
        if(unary_f(k, x.f, &r)) x = val_f(r);
        return;
    }

    if(!strcmp(k, "y^x")) k = "^";

    if(is_binary(k)) {
        if(op_pending && !entering) {
            // Two operators in a row: the second replaces the first.
            snprintf(os[osp - 1], sizeof(os[0]), "%s", k);
            int l = strlen(expr);
            while(l > 0 && expr[l - 1] == ' ') l--;    // the space after it
            while(l > 0 && expr[l - 1] != ' ') l--;    // the old operator
            while(l > 0 && expr[l - 1] == ' ') l--;    // the space before it
            expr[l] = '\0';
            expr_add(" "); expr_add(k); expr_add(" ");
            return;
        }

        alg_operand();

        bool right = !strcmp(k, "^");
        while(osp > 0 && strcmp(os[osp - 1], "(") != 0 &&
              (right ? prec(os[osp - 1]) > prec(k) : prec(os[osp - 1]) >= prec(k))) {
            if(!alg_reduce_top()) { alg_reset(); return; }   // an error; the message is set
        }
        if(osp < STACK_MAX) snprintf(os[osp++], sizeof(os[0]), "%s", k);

        x = vs[vsp - 1];   // show the running total, as a desk calculator does
        expr_add(" "); expr_add(k); expr_add(" ");
        op_pending = true;
        return;
    }

    if(!strcmp(k, "(")) {
        // Only where an operand could go; "5 (" would leave 5 with nothing to do.
        if(entering || (!op_pending && vsp > 0)) return;
        if(fresh) { fresh = false; expr[0] = '\0'; }
        if(osp < STACK_MAX) snprintf(os[osp++], sizeof(os[0]), "(");
        expr_add("(");
        op_pending = false;
        return;
    }
    if(!strcmp(k, ")")) {
        bool open = false;
        for(int i = 0; i < osp; i++) if(!strcmp(os[i], "(")) open = true;
        if(!open) return;

        alg_operand();
        while(osp > 0 && strcmp(os[osp - 1], "(") != 0) {
            if(!alg_reduce_top()) { alg_reset(); return; }
        }
        osp--;                        // the "("
        x = vs[--vsp];                // the bracket's value is now the operand in hand
        expr_add(")");
        op_pending = false;
        x_shown = true;
        return;
    }

    if(!strcmp(k, "=")) {
        alg_operand();
        while(osp > 0) {
            if(!strcmp(os[osp - 1], "(")) { osp--; continue; }   // close what was left open
            if(!alg_reduce_top()) { alg_reset(); return; }
        }
        if(vsp > 0) x = vs[vsp - 1];
        vsp = 0;
        expr_add(" =");
        fresh = true;
        op_pending = false;
        return;
    }

    if(!strcmp(k, "DEG")) { degrees = !degrees; return; }
    if(!strcmp(k, "INV")) { inv = !inv; return; }
    if(!strcmp(k, "HEX") || !strcmp(k, "DEC") || !strcmp(k, "OCT") || !strcmp(k, "BIN")) {
        alg_commit();
        base = !strcmp(k, "HEX") ? 16 : !strcmp(k, "DEC") ? 10 : !strcmp(k, "OCT") ? 8 : 2;
        return;
    }
    if(!strcmp(k, "WID")) {
        alg_commit();
        width = width >= 64 ? 8 : width * 2;
        x = val_i(x.i);   // re-wrapped to the new width
        for(int i = 0; i < vsp; i++) vs[i] = val_i(vs[i].i);
        return;
    }
}

//************************************[ rpn ]***********************************
static void rpn_push(double v)
{
    rs[3] = rs[2]; rs[2] = rs[1]; rs[1] = rs[0]; rs[0] = v;
}

static void rpn_drop(void)
{
    rs[0] = rs[1]; rs[1] = rs[2]; rs[2] = rs[3];
}

static void rpn_commit(void)
{
    if(entering) { rs[0] = entry_f(); entering = false; }
}

static bool is_test(const char *k)
{
    return !strcmp(k, "x=0?") || !strcmp(k, "x<0?") || !strcmp(k, "x=y?") || !strcmp(k, "x<y?");
}

static bool rpn_test(const char *k)
{
    rpn_commit();
    if(!strcmp(k, "x=0?")) return rs[0] == 0;
    if(!strcmp(k, "x<0?")) return rs[0] < 0;
    if(!strcmp(k, "x=y?")) return rs[0] == rs[1];
    if(!strcmp(k, "x<y?")) return rs[0] < rs[1];
    return false;
}

static int rpn_find_label(char n)
{
    for(int i = 0; i < prog_len; i++) {
        if(prog[i][0] == 'L' && prog[i][3] == n && !strncmp(prog[i], "LBL", 3)) return i;
    }
    return -1;
}

static void prog_save(void)
{
    File f = SPIFFS.open(CALC_PRG_PATH, FILE_WRITE);
    if(!f) { Serial.println("[CALC] could not save the program"); return; }
    for(int i = 0; i < prog_len; i++) { f.print(prog[i]); f.print('\n'); }
    f.close();
    Serial.printf("[CALC] program saved, %d steps\n", prog_len);
}

static void prog_load(void)
{
    prog_len = 0;
    File f = SPIFFS.open(CALC_PRG_PATH, FILE_READ);
    if(!f) return;
    while(f.available() && prog_len < PROG_MAX) {
        String line = f.readStringUntil('\n');
        line.trim();
        if(line.length() == 0 || line.length() >= STEP_LEN) continue;
        snprintf(prog[prog_len++], STEP_LEN, "%s", line.c_str());
    }
    f.close();
    if(prog_len) Serial.printf("[CALC] program loaded, %d steps\n", prog_len);
}

/* Runs one complete token - a key, or a key with its digit attached. Used by a
 * finger, by the single-step key and by the program runner alike. */
static void rpn_do(const char *k)
{
    if(is_digit_key(k)) {
        if(!entering) { if(lift) rpn_push(0); entry_start(); }
        entry_append(k[0]);
        return;
    }
    if(!strcmp(k, "."))   { if(!entering) { if(lift) rpn_push(0); entry_start(); } entry_point(); return; }
    if(!strcmp(k, "EEX")) { if(!entering && lift) rpn_push(0); entry_eex(); return; }
    if(!strcmp(k, "+/-")) { if(entering) entry_negate(); else rs[0] = -rs[0]; return; }
    if(!strcmp(k, "BS"))  { if(entering) entry_backspace(); else { rs[0] = 0; lift = false; } return; }

    if(!strcmp(k, "ENT")) { rpn_commit(); rpn_push(rs[0]); lift = false; return; }
    if(!strcmp(k, "CLx")) { entering = false; rs[0] = 0; lift = false; return; }
    if(!strcmp(k, "CLR")) { entering = false; rs[0] = rs[1] = rs[2] = rs[3] = 0; lift = false; return; }

    if(!strcmp(k, "x<>y")) { rpn_commit(); double t = rs[0]; rs[0] = rs[1]; rs[1] = t; lift = true; return; }
    if(!strcmp(k, "Rv"))   { rpn_commit(); double t = rs[0]; rs[0] = rs[1]; rs[1] = rs[2]; rs[2] = rs[3]; rs[3] = t; lift = true; return; }
    if(!strcmp(k, "LSTx")) { rpn_commit(); if(lift) rpn_push(0); rs[0] = lastx; lift = true; return; }
    if(!strcmp(k, "pi"))   { rpn_commit(); if(lift) rpn_push(0); rs[0] = M_PI; lift = true; return; }
    if(!strcmp(k, "e"))    { rpn_commit(); if(lift) rpn_push(0); rs[0] = M_E;  lift = true; return; }

    if(!strncmp(k, "STO", 3) && k[3] >= '0' && k[3] <= '9') { rpn_commit(); regs[k[3] - '0'] = rs[0]; lift = true; return; }
    if(!strncmp(k, "RCL", 3) && k[3] >= '0' && k[3] <= '9') {
        rpn_commit(); if(lift) rpn_push(0); rs[0] = regs[k[3] - '0']; lift = true; return;
    }

    if(!strcmp(k, "y^x")) k = "^";
    if(is_binary(k)) {
        rpn_commit();
        val_t r;
        if(!binary(k, val_f(rs[1]), val_f(rs[0]), &r)) return;
        lastx = rs[0];
        rpn_drop();
        rs[0] = r.f;
        lift = true;
        return;
    }
    if(is_unary_f(k)) {
        rpn_commit();
        double r;
        if(!unary_f(k, rs[0], &r)) return;
        lastx = rs[0];
        rs[0] = r;
        lift = true;
        return;
    }

    if(!strcmp(k, "DEG")) { degrees = !degrees; return; }
    if(!strcmp(k, "INV")) { inv = !inv; return; }

    // Program control, met by a finger rather than by the runner.
    if(is_test(k)) { snprintf(test_note, sizeof(test_note), rpn_test(k) ? "YES" : "NO"); return; }
    if(!strncmp(k, "LBL", 3)) return;
    if(!strncmp(k, "GTO", 3)) {
        int at = rpn_find_label(k[3]);
        if(at < 0) { set_err("No such label"); return; }
        pc = at;
        return;
    }
}

static void rpn_run(void)
{
    rpn_commit();
    if(prog_len == 0) { set_err("No program"); return; }
    if(pc >= prog_len) pc = 0;

    running = true;
    int n = 0;
    while(running && pc < prog_len && n++ < RUN_MAX) {
        const char *t = prog[pc++];

        if(!strcmp(t, "R/S")) { running = false; break; }
        if(!strncmp(t, "GTO", 3)) {
            int at = rpn_find_label(t[3]);
            if(at < 0) { set_err("No such label"); break; }
            pc = at;
            continue;
        }
        if(is_test(t)) { if(!rpn_test(t)) pc++; continue; }   // false skips the next step

        rpn_do(t);
        if(err[0]) break;
    }
    if(n >= RUN_MAX) set_err("Runaway program");
    running = false;
    if(pc >= prog_len) pc = 0;
}

static void rpn_exec(const char *k)
{
    // Editing keys, never recorded.
    if(!strcmp(k, "PRGM")) {
        recording = !recording;
        if(!recording) prog_save();
        return;
    }
    if(!strcmp(k, "CLP")) { prog_len = 0; pc = 0; prog_save(); return; }
    if(!strcmp(k, "BST")) { if(prog_len > 0) prog_len--; if(pc > prog_len) pc = prog_len; return; }
    if(!strcmp(k, "SST")) {
        if(recording) return;
        if(prog_len == 0) { set_err("No program"); return; }
        if(pc >= prog_len) pc = 0;
        const char *t = prog[pc++];
        if(!strcmp(t, "R/S")) ;
        else if(!strncmp(t, "GTO", 3)) { int at = rpn_find_label(t[3]); if(at < 0) set_err("No such label"); else pc = at; }
        else if(is_test(t)) { if(!rpn_test(t)) pc++; }
        else rpn_do(t);
        if(pc >= prog_len) pc = 0;
        return;
    }

    if(recording) {
        if(prog_len >= PROG_MAX) { set_err("Program memory full"); return; }
        snprintf(prog[prog_len++], STEP_LEN, "%s", k);
        return;
    }

    if(!strcmp(k, "R/S")) { rpn_run(); return; }
    rpn_do(k);
}

/* A key from a finger: the ones that take a digit wait for it here, so that
 * "STO" then "3" becomes the single step "STO3". */
static void rpn_key(const char *k)
{
    if(pending[0]) {
        if(k[0] >= '0' && k[0] <= '9' && k[1] == '\0') {
            char tok[STEP_LEN];
            snprintf(tok, sizeof(tok), "%s%c", pending, k[0]);
            pending[0] = '\0';
            rpn_exec(tok);
            return;
        }
        pending[0] = '\0';   // anything else abandons it, and is then handled
    }
    if(!strcmp(k, "STO") || !strcmp(k, "RCL") || !strcmp(k, "GTO") || !strcmp(k, "LBL")) {
        snprintf(pending, sizeof(pending), "%s", k);
        return;
    }
    rpn_exec(k);
}

//************************************[ display ]*******************************
static void render(void)
{
    char a[LINE_MAX], b[LINE_MAX];

    out_status[0] = out_info[0] = out_top[0] = '\0';

    if(mode == CALC_MODE_RPN) {
        fmt_float(a, sizeof(a), rs[1]);
        snprintf(out_top, sizeof(out_top), "Y %s", a);

        snprintf(a, sizeof(a), "%.6g", rs[2]);
        snprintf(b, sizeof(b), "%.6g", rs[3]);
        snprintf(out_info, sizeof(out_info), "Z %s  T %s", a, b);

        int n = snprintf(out_status, sizeof(out_status), "%s%s", degrees ? "DEG" : "RAD", inv ? " INV" : "");
        if(pending[0])        snprintf(out_status + n, sizeof(out_status) - n, "  %s _", pending);
        else if(recording)    snprintf(out_status + n, sizeof(out_status) - n, "  PRGM %03d %s", prog_len,
                                       prog_len ? prog[prog_len - 1] : "");
        else if(prog_len)     snprintf(out_status + n, sizeof(out_status) - n, "  P%03d/%03d", pc, prog_len);
        if(test_note[0])      snprintf(out_status + n, sizeof(out_status) - n, "  %s", test_note);

        if(err[0])         snprintf(out_main, sizeof(out_main), "%s", err);
        else if(entering)  snprintf(out_main, sizeof(out_main), "%s", entry);
        else               fmt_float(out_main, sizeof(out_main), rs[0]);
        return;
    }

    // algebraic
    if(mode == CALC_MODE_SCI) {
        snprintf(out_status, sizeof(out_status), "%s%s", degrees ? "DEG" : "RAD", inv ? " INV" : "");
    } else if(mode == CALC_MODE_PROG) {
        snprintf(out_status, sizeof(out_status), "%s %d-bit",
                 base == 16 ? "HEX" : base == 10 ? "DEC" : base == 8 ? "OCT" : "BIN", width);

        val_t v = entering ? entry_val() : x;
        if(base == 10) { fmt_int(a, sizeof(a), v.i, 16); snprintf(out_info, sizeof(out_info), "HEX %s", a); }
        else           { fmt_int(a, sizeof(a), v.i, 10); snprintf(out_info, sizeof(out_info), "DEC %s", a); }
    }

    // The tail of the expression, since the latest part is the part that matters.
    int l = strlen(expr);
    const char *tail = l > LINE_MAX - 1 ? expr + (l - (LINE_MAX - 1)) : expr;
    snprintf(out_top, sizeof(out_top), "%s", tail);

    if(err[0])         snprintf(out_main, sizeof(out_main), "%s", err);
    else if(entering)  snprintf(out_main, sizeof(out_main), "%s", entry);
    else               fmt_val(out_main, sizeof(out_main), x);
}

//************************************[ prefs ]*********************************
static void prefs_save(void)
{
    Preferences p;
    if(!p.begin(CALC_PREFS, false)) return;
    p.putUChar("mode", mode);
    p.putBool("deg", degrees);
    p.putUChar("base", base);
    p.putUChar("width", width);
    p.end();
}

static void prefs_load(void)
{
    Preferences p;
    if(!p.begin(CALC_PREFS, true)) return;
    mode    = p.getUChar("mode", mode);
    degrees = p.getBool("deg", degrees);
    base    = p.getUChar("base", base);
    width   = p.getUChar("width", width);
    p.end();

    if(mode < 0 || mode >= CALC_MODE_MAX) mode = CALC_MODE_SIMPLE;
    if(base != 2 && base != 8 && base != 10 && base != 16) base = 10;
    if(width != 8 && width != 16 && width != 32 && width != 64) width = 32;
}

//************************************[ public API ]****************************
void calc_init(void)
{
    alg_reset();
    memset(rs, 0, sizeof(rs));
    memset(regs, 0, sizeof(regs));
    lastx = 0;
    lift = true;
    pending[0] = test_note[0] = err[0] = '\0';
    prefs_load();
    prog_load();
    render();
}

int calc_mode(void) { return mode; }
int calc_base(void) { return base; }

const char *calc_mode_name(int m)
{
    switch(m) {
        case CALC_MODE_SIMPLE: return "Basic";
        case CALC_MODE_SCI:    return "Scientific";
        case CALC_MODE_PROG:   return "Programmer";
        case CALC_MODE_RPN:    return "RPN";
        default:               return "?";
    }
}

const char *calc_mode_short(int m)
{
    switch(m) {
        case CALC_MODE_SIMPLE: return "BAS";
        case CALC_MODE_SCI:    return "SCI";
        case CALC_MODE_PROG:   return "PRG";
        case CALC_MODE_RPN:    return "RPN";
        default:               return "?";
    }
}

void calc_set_mode(int m)
{
    if(m < 0 || m >= CALC_MODE_MAX || m == mode) return;

    // Whatever is showing goes with you.
    double cur;
    if(mode == CALC_MODE_RPN) { rpn_commit(); cur = rs[0]; }
    else                      { alg_commit(); cur = mode == CALC_MODE_PROG ? (double)x.i : x.f; }

    err[0] = '\0';
    inv = false;
    pending[0] = test_note[0] = '\0';
    mode = m;

    alg_reset();
    x = val_f(cur);
    x.i = wrap(clamp_i(cur));
    rs[0] = cur;
    lift = true;

    prefs_save();
    render();
}

void calc_key(const char *k)
{
    if(k == NULL || k[0] == '\0') return;

    bool had_err = err[0] != '\0';
    err[0] = '\0';
    test_note[0] = '\0';

    // An error is cleared by the next key, which is then not also acted on -
    // otherwise a "CLR" meant to dismiss it would do more than that was asked.
    if(had_err && strcmp(k, "CLR") != 0) { render(); return; }

    if(mode == CALC_MODE_RPN) rpn_key(k);
    else                      alg_key(k);

    // A function key uses INV up; only INV itself leaves it on.
    if(strcmp(k, "INV") != 0 && !is_digit_key(k)) inv = false;

    if(!strcmp(k, "DEG") || !strcmp(k, "WID") ||
       !strcmp(k, "HEX") || !strcmp(k, "DEC") || !strcmp(k, "OCT") || !strcmp(k, "BIN")) prefs_save();

    render();
}

const char *calc_line_status(void) { return out_status; }
const char *calc_line_info(void)   { return out_info; }
const char *calc_line_top(void)    { return out_top; }
const char *calc_line_main(void)   { return out_main; }
bool        calc_error(void)       { return err[0] != '\0'; }
bool        calc_inv(void)         { return inv; }

int         calc_prog_len(void)        { return prog_len; }
const char *calc_prog_step(int i)      { return (i >= 0 && i < prog_len) ? prog[i] : ""; }
int         calc_prog_pc(void)         { return pc; }
bool        calc_prog_recording(void)  { return recording; }
