# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for a LilyGo T-Deck-Pro (ESP32-S3, 16MB flash, 8MB PSRAM) turned into a phone: e-paper display, physical keyboard, cellular modem, LoRa, GPS. PlatformIO + Arduino framework + LVGL 8.3.

## Commands

PlatformIO is not on `PATH` in this environment. Use the full path:

```bash
/c/Users/danie/.platformio/penv/Scripts/pio.exe run -e T-Deck-Pro          # build
/c/Users/danie/.platformio/penv/Scripts/pio.exe run -e T-Deck-Pro -t upload
/c/Users/danie/.platformio/penv/Scripts/pio.exe device monitor -b 115200
/c/Users/danie/.platformio/penv/Scripts/pio.exe run -t clean
```

`T-Deck-Pro` is the only environment and the default. The board definition lives in `boards/T-Deck-Pro.json` (it is not a stock PlatformIO board).

The build is noisy with pre-existing `DEFAULT_SDA`/`DEFAULT_SCL`/`TINY_GSM_MODEM_SIM7672` redefinition warnings from the vendored libraries — ignore them, and filter them out when checking your own changes:

```bash
pio.exe run -e T-Deck-Pro 2>&1 | grep -E "error|FAILED|SUCCESS|RAM:|Flash:"
```

To force a rebuild of just the project sources (libraries are slow to recompile): `rm -rf .pio/build/T-Deck-Pro/src`.

`test/` holds only PlatformIO's stock placeholder README — there is no test suite, and nothing is verifiable except by building and running on hardware. Say so rather than implying a change is tested.

## Dependencies

Everything is vendored under `lib/` (LVGL, GxEPD2, TinyGSM, RadioLib, SensorLib, XPowersLib, TinyGPSPlus, Adafruit TCA8418, BQ27220, ...). `lib_deps` in `platformio.ini` is commented out on purpose — edit the sources in `lib/` rather than reintroducing registry dependencies.

LVGL is configured by `include/lv_conf.h`, force-included via a build flag. Notable consequences: `LV_MEM_SIZE` is only 48KB, and margin style properties (`lv_obj_set_style_margin_*`) do **not** exist in this build — offset a child by positioning it, not by margins. Only Montserrat 14/18/26 are compiled in; the project's own `Font_Mono_Bold_*` fonts (`src/assets/`) cover ASCII 32..126 **only**, so any label showing an `LV_SYMBOL_*` glyph must be left on a Montserrat font.

**`LV_LABEL_LONG_DOT` needs a height, not just a width.** It wraps the text and only writes the ellipsis once the text overflows the label's *height*, so a label left at the default `LV_SIZE_CONTENT` height silently grows downward to fit the whole string and paints over whatever is below it. This has produced garbled screens twice. Any label holding text of unpredictable length — a contact name, a number, a message body — needs `lv_obj_set_size()` with an explicit height. The `Font_Mono_Bold_*` fonts are monospace with a `line_height` of 18 (21 for size 20), and advance widths of 134/144/154 in 1/16px for sizes 14/15/16 — i.e. 8.4/9.0/9.6px per character, which is what to divide by when working out how much text a given width holds.

Message bodies additionally carry the sender's own line breaks, so anything rendering one on a single line must flatten it first (`ui_message_snippet()` in `ui_phone1.cpp`).

## Architecture

### Display: everything is e-paper

`src/main.cpp` renders LVGL into a 1bpp buffer and pushes whole frames to a GxEPD2 panel. `disp_drv.full_refresh = 1`, so *any* invalidation repaints the entire panel — a once-per-second label update is a once-per-second full refresh. Periodic UI updates are deliberately slowed to a few seconds (see the in-call timer). `disp_full_refr()` / `ui_disp_full_refr()` request a full (rather than partial) waveform for the next flush and are called on screen transitions to clear ghosting.

### Screen manager

`src/ui/ui_scr_mrg.c` is a small stack-based screen manager. Screens register a `scr_lifecycle_t` (`create` / `entry` / `exit` / `destroy`) against an id from the `SCREEN*_ID` enum in `include/ui_phone1.h`, and are driven with `scr_mgr_switch` / `scr_mgr_push` / `scr_mgr_pop`.

Two rules follow from how it works, and violating either corrupts the stack or frees live objects:

- **Never call `scr_mgr_pop`/`push` from inside a screen's own `entry()` or `exit()`** — the manager is mid-walk of its stack. A screen whose contents went stale must rebuild in place (`lv_obj_clean` + repopulate). The list screens do this with revision counters: `ui_sms_revision` / `ui_contacts_revision` are bumped by whoever mutates the data, and each list compares them in `entry()`.
- Popping from a button's own event callback is fine and is the established pattern, but chaining several pops/pushes in one callback is not.

A pushed screen's widgets are destroyed on pop, so a screen cannot return a value directly. The hand-off is a set of file-scope variables in `src/ui/ui_phone1.cpp` (`ui_active_number`, `ui_active_contact`, `ui_pick_mode`, `ui_pick_ready`, `ui_compose_prefill`): the pusher sets the subject, the pushed screen reads it.

### UI layering

- `src/ui/ui_phone1.cpp` — every screen, all `static`. Shared helpers near the top (`scr_back_btn_create`, `scr_row_create`, `scr_app_list_create`, `scr_action_bar_create`, `scr_field_create`) — reuse them rather than restyling widgets from scratch, since the mono theme needs explicit bg/border/shadow overrides everywhere.
- `src/ui/ui_phone1_port.cpp` — the only place the UI touches hardware. Screens call `ui_*` wrappers; those call the peripheral layer.
- `src/peripherals/` — one file per device.
- `src/apps/` — data models with no LVGL dependency.

`ui_phone1_entry()` at the bottom of `ui_phone1.cpp` creates the always-running timers and registers every screen. `phone_event_timer_cb` is the one that runs regardless of which screen is showing: it drains received SMS into the store, reconciles outgoing-send results, and raises the call screen on an incoming call.

### Modem ownership (important)

`src/peripherals/peri_modem.cpp` owns `SerialAT` exclusively, on its own FreeRTOS task. **Nothing else may read or write `SerialAT`** — an earlier passthrough task did, and it swallowed the `RING` and `+CMTI` notifications that make the device answerable. Route raw AT traffic through `modem_request_at()` (the Wifi AP screens do this).

The UI never blocks on the modem. Requests go down a queue, received messages come back up a second queue, and call/network state is a mutex-protected snapshot the UI polls. Two consequences to preserve when editing:

- The URC handler must not perform modem I/O — it can be running in the middle of another command. `+CMTI` therefore only records the SIM slot; the task reads the message later when it is between commands.
- `TinyGsm modem` (`src/main.cpp`) is only usable before `modem_service_init()` starts the task, during `A7682E_init()`.

### Storage

`src/apps/phone_store.cpp` keeps contacts and a bounded message log in PSRAM, mirrored to SPIFFS as TSV (`/contacts.tsv`, `/messages.tsv`); each mutation rewrites the file. It is **not** thread-safe — only the LVGL task may call into it, which is why received messages arrive via a queue rather than being written by the modem task.

Numbers are compared by `phone_number_match()` (last 7 digits, digits only), so `+61412345678` / `0412345678` / `412345678` are one contact. Conversations are derived from the log on demand, not stored, so any thread index is only valid until the log changes.

### Input

Two LVGL input devices are registered in `lvgl_init()`: the CST touch panel as a pointer, and the TCA8418 keyboard as a keypad bound to a default group. LVGL auto-adds textareas and buttons to that group, so a screen with a text field should `lv_group_focus_obj()` the field it wants typing to land in.

`src/peripherals/peri_keypad.cpp` holds four keymaps (base / shift / sym / alt) selected by modifier state; digits live on the **sym** layer, which is why the dialer and contact editor provide on-screen numeric button matrices.

Horizontal swipes are detected separately by `indev_get_gesture_dir` polling the pointer indev; only the home screen uses it, to page between menu screens. The menu holds 9 buttons per page with hard-coded `pos_x`/`pos_y` per entry, and a second page appears once `menu_btn_list` exceeds 9 items.

### Clock

There is no RTC, and two things can say what time it is: a GPS fix and the cellular network (NITZ, read back with `AT+CCLK?`). Both go through `src/apps/system_clock.cpp`, which arbitrates by source — GPS outranks the network, each may refresh itself — and owns the time zone.

`time(NULL)` returns near-zero until one of them lands; timestamps are stored as 0 and rendered as `--:--` in that state.

**Do not use `mktime()` to turn a UTC time into an epoch here.** It reads its input as *local* time, so it is wrong by the zone offset as soon as a zone is in force — which it now is, since the network reports one. Use `system_clock_epoch_from_utc()`, which does the conversion arithmetically. The `day` it takes may sit one outside the month, which is what a caller subtracting a UTC offset gets for free when it steps over midnight.

Zones come from `timezone_db` (461 IANA names with full POSIX DST rules, in `include/timezone_names.h`). That header is large and defines its data `static`, so exactly one translation unit includes it — go through `timezone_db.h`. A zone chosen by hand persists in NVS and suppresses the network's offset, since a fixed offset cannot express daylight saving.
