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
| Cellular | A7682E modem — voice, SMS, network time. Its WiFi is receive-only (used for positioning), so it cannot act as an access point |
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

**Mesh.** The phone is a [MeshCore](https://docs.meshcore.io/) node: LoRa chat
with no network, no SIM and no subscription, out to whatever is in radio range.
It carries its own Ed25519 identity, announces itself, and lists the nodes it
hears with their signal and hop count.

That list is also the list of people to talk to — there is nobody to add,
because nodes announce themselves and this one keeps whoever it hears. Tapping
one opens a conversation; above them sits the public channel, which every
MeshCore node on the frequency can read.

A private message is encrypted to the far node's key and acknowledged end to
end, so the conversation says **delivered** only once that node actually
answered — more than a text message ever tells you. It goes down a known route
where one exists and is flooded across the mesh where it does not, and gives up
after a timeout scaled to the airtime and hop count, marking itself *not
delivered*. A channel message is a broadcast that nobody acknowledges, so the
most it can report is that it went out. One message is in flight at a time,
because an acknowledgement names a packet rather than a message.

Messages live in memory only, not on flash: a mesh conversation is a
conversation in the moment, and writing every message through the filesystem
would put SPIFFS in the path of the radio. An unread mesh message raises the
same envelope on the taskbar and the lock screen as an unread text.

Every node on a mesh has to agree on four radio settings exactly — frequency,
bandwidth, spreading factor and coding rate — and getting any one wrong means
hearing nothing at all, which looks the same as a broken radio. The Mesh radio
screen offers presets and lets all four be set by hand:

| Preset | Frequency | Bandwidth | SF | CR |
|---|---|---|---|---|
| Victoria AU | 916.575 MHz | 62.5 kHz | 7 | 8 |
| Aus / NZ | 915.800 MHz | 250 kHz | 10 | 5 |
| EU / UK | 869.525 MHz | 250 kHz | 10 | 5 |
| US / Canada | 910.525 MHz | 250 kHz | 10 | 5 |

Editing any value switches to a custom preset seeded from what was showing.
Changing settings retunes the radio and re-announces the node, since nobody on
the new settings has heard it. The node listens and speaks for itself but does
not relay for others, since a phone in a pocket makes a poor repeater and
forwarding costs battery.

This replaced the vendor's LoRa demo screens: MeshCore expects to own the
SX1262, and two drivers cannot share one radio.

**Companion app.** A MeshCore companion app — the official phone app, or
anything else speaking the same protocol — can drive this node over either
Bluetooth or WiFi. It gets the same node the screen does: the same identity, the
same contacts, the same conversations, so a message sent from the app and one
typed here go out over the same key, and a message that arrives is delivered to
both. This is unlike a stock companion radio, where the app *is* the entire user
interface.

*This node > Companion app* picks the link:

- **Bluetooth** advertises as `MeshCore-<node name>` and pairs with a six-digit
  code shown on that screen. The code is generated once and kept, so a paired
  phone stays paired across reboots.
- **WiFi** puts the device up as an access point and listens for the app on TCP
  port 5000, showing the address to connect to. It cannot run while the UDP
  hotspot is on — both want the one WiFi radio in access-point mode — and the
  screen says so rather than failing quietly.

One link at a time, and turning Bluetooth off only stops it advertising: the
Bluetooth stack keeps the memory it claimed until the next restart.

**The link costs some drawing speed, and only while it is on.** Both radio
stacks want more fast internal memory than is left once the display has taken a
full screen buffer out of it — that buffer is a byte per pixel, the largest
single claim on the device. So when the link is set to come up, the drawing
buffer goes to the slower PSRAM instead and the radio gets the internal memory.
Turn the link off and the buffer goes back where it was on the next restart.
This is why the link is a setting rather than something always on.

It also means turning the link on cannot take effect immediately: the buffer was
placed at startup and the memory is already spent. The setting is saved and the
screen says *restart the phone to start the link*, which is exactly what to do.

The link is remembered and comes back on its own at boot, which is when there is
the most memory free for it. That also means a link which cannot start would
otherwise make the phone unusable — it would fail, restart, and fail again with
nobody able to reach the setting that turns it off. So the attempt is written
down before it is made and rubbed out once the link has been up for twenty
seconds. Finding it still written at boot means the last attempt did not
survive, and the link is left off with *last attempt crashed — turn it on again
to retry* on the Companion app screen. Starting a radio is also refused outright,
with the number of kilobytes free, when there is plainly not enough memory left
for it.

Messages that arrive while no app is connected are held — sixteen of them — and
handed over when one connects, so a conversation is not lost because the phone
was in somebody's pocket. Contacts are not persisted: they are rebuilt from the
adverts nodes send anyway, so an app reconnecting after a restart re-adds
whatever this node has not heard from yet.

The node reports protocol version 7 and answers anything newer with "unsupported
command" rather than going quiet. Everything a conversation needs is there —
contacts, messages, channels, radio settings, device time, adverts — while
custom variables, statistics, telemetry, signing and flood scoping are not.

**Notifications.** Vibrate on an incoming call, on an incoming text, or neither;
optionally a tone as well. All configurable and remembered across reboots.

Also inherited from the vendor firmware, and left working: a GPS readout, a WiFi access-point configuration screen, battery and
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
src/apps/             contacts and message storage, the clock, the zone table,
                      the MeshCore node and its companion protocol, the hotspot
                      relay
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
