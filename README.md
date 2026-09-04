# lilyphone1

Firmware that turns a [LilyGo T-Deck-Pro](https://www.lilygo.cc/) into a usable
phone: a dialler that knows who is calling, a contact book, and a threaded SMS
app, on a 3.1" monochrome e-paper screen with a physical keyboard.

It began as the vendor's demo firmware — a grid of hardware test screens — and
has grown the parts that make it answer, ring, remember who people are, and
stay out of the way when it is in a pocket.

## Hardware

| | |
|---|---|
| Board | LilyGo T-Deck-Pro (`boards/T-Deck-Pro.json`) |
| SoC | ESP32-S3, 16MB flash, 8MB PSRAM |
| Display | 3.1" 240×320 e-paper, 1 bit per pixel (GDEQ031T10) |
| Input | CST328 touch panel, TCA8418 physical keyboard |
| Cellular | A7682E modem — voice, SMS, network time |
| Also fitted | SX1262 LoRa, u-blox GPS, BQ25896 charger, BQ27220 fuel gauge, LTR-553ALS, BHI260AP |

## What it does

**Phone.** A dialler that names the number as you type it, an in-call screen
with caller ID, and incoming calls that raise themselves over whatever is on
screen — including a locked one. Call state is confirmed with the modem rather
than inferred, so the screen does not sit on "Calling…" after a call has gone
away.

**Contacts.** Names and numbers, stored on the device and sorted as you add
them. Numbers are matched on their last seven digits, so `+61412345678`,
`0412345678` and `412345678` are recognised as the same person however the
network happens to present them.

**Messages.** Conversations grouped by contact, with delivery state, unread
markers, and a composer. Incoming messages appear on screen as they arrive
rather than when you next navigate. Individual messages or whole threads can be
deleted.

- Messages that arrive in UCS2 — anything with an emoji or a curly quote in it —
  are decoded rather than shown as hex.
- Emoji render, in monochrome, from a font generated for this display.
- **Reactions** from other phones are recognised and pinned to the message they
  refer to, the way a modern phone shows them, instead of appearing as a
  sentence quoting it. You can send the same six back.

**Lock screen.** Swipe down from the top of the home screen for quick settings
and a lock button, or let it lock itself after a configurable idle period. The
lock screen shows the time, the date, the battery and who has messaged you.
Swipe up to unlock. A call still comes through.

**Clock.** Set from the cellular network (NITZ) or from a GPS fix, whichever
arrives first, with the local time zone taken from the network or chosen by hand
from a searchable list of 461.

**Hotspot.** The phone can become a WiFi access point that relays a UDP tunnel
out over mobile data, so a laptop with no other connection can reach the
internet through a WireGuard peer. Configure the endpoint, port, APN and access
point details on the Hotspot screen, join the network, and point the client's
tunnel at the phone on the same port.

Two things to know before relying on it:

- **It is a relay to one configured endpoint, not a router.** The Arduino core's
  lwIP is built with `CONFIG_LWIP_IP_FORWARD` off, so a packet addressed to any
  other host is discarded before this firmware could see it. A tunnel has
  exactly one endpoint, so this is enough for WireGuard - but it will not serve
  as a general gateway.
- **It is slow.** Every datagram crosses to the modem over a 115200 baud serial
  link wrapped in AT commands, which puts the ceiling somewhere near 50 kbit/s
  with tens of milliseconds of latency per packet. It is a usable control
  channel, not a usable internet connection. Set a small MTU on the tunnel;
  anything over 1472 bytes is dropped rather than fragmented.

**Notifications.** Vibrate on an incoming call, on an incoming text, or neither;
optionally a tone as well. All configurable and remembered across reboots.

Also inherited from the vendor firmware, and left working: LoRa send and
receive, a GPS readout, a WiFi access-point configuration screen, battery and
charger detail, and a peripheral self test.

## Building

Needs [PlatformIO](https://platformio.org/). The board definition is in-tree and
all libraries are vendored under `lib/`, so a checkout builds without fetching
anything.

```bash
pio run                      # build
pio run -t upload            # flash
pio device monitor -b 115200 # serial log
```

`T-Deck-Pro` is the only environment and the default. The build prints a number
of macro-redefinition warnings from the vendored libraries; they are pre-existing
and harmless.

### Regenerating the emoji font

`src/assets/Font_Emoji_16.c` and `Font_Emoji_28.c` are generated, not written by
hand. To rebuild them:

```bash
pip install pillow fonttools
curl -sSLo NotoEmoji.ttf \
  "https://github.com/google/fonts/raw/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
python tools/gen_emoji_font.py NotoEmoji.ttf 13 src/assets/Font_Emoji_16.c Font_Emoji_16
python tools/gen_emoji_font.py NotoEmoji.ttf 22 src/assets/Font_Emoji_28.c Font_Emoji_28 \
  1F44D,1F44E,2764,1F602,203C,2753
```

The glyphs derive from [Noto Emoji](https://github.com/google/fonts/tree/main/ofl/notoemoji),
under the SIL Open Font License 1.1.

## Layout

```
src/main.cpp          board bring-up, the LVGL display and input drivers
src/ui/               every screen (ui_phone1.cpp), the hardware wrappers they
                      call (ui_phone1_port.cpp), and the screen stack
src/peripherals/      one file per device; peri_modem.cpp owns the modem
src/apps/             contacts and message storage, the clock, the zone table
src/assets/           fonts and icons, some generated
tools/                the emoji font generator
```

`CLAUDE.md` documents the architecture and the traps in more detail — the
e-paper refresh model, the rule that one task owns the modem, and the LVGL
behaviours this display is unusually sensitive to.

## Status

A personal project, developed against real hardware but without automated tests;
`test/` holds only PlatformIO's placeholder. The cellular paths in particular
depend on what a given A7682E firmware and carrier will accept, and the serial
log is deliberately talkative about which AT commands were refused.
