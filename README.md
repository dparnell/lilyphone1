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
| Also fitted | SX1262 LoRa, u-blox GPS, BQ25896 charger, BQ27220 fuel gauge, LTR-553ALS (absent on this unit), BHI260AP (unused and unpowered) |

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
**Sharing your position is off by default and is a three-way choice**, on *This
node > Share location*: off, only when you press *Announce now*, or with every
advert. The middle setting is the point of having three — an advert floods the
mesh, gets relayed well past radio range and is readable by anyone running
MeshCore, so sharing because you meant to is a different thing from sharing
because a timer went off in your pocket.

The position comes from the phone's own GPS, or — when there is no fix — from
one set by hand on *This node > Set location*, in degrees, south and west
negative. A companion app can set it too, which is what putting the node on
the MeshCore map needs: a device that lives indoors may never see the sky, and a
fix it cannot get is not a location it can share. A live fix always wins, so a
phone that has moved says where it is rather than where it was put, and the
Share location row marks a given position *(set)*.

**With sharing on, the receiver stays running.** It is otherwise suspended
except while the GPS screen is showing, which is fine for a battery and useless
for a position that has to reach an advert - the coordinates would only ever
advance while somebody was looking at them.

Setting a position turns sharing on if it was off, because a position handed
over for adverts that never reaches one is no use to anybody. With neither a fix
nor a set position, nothing is shared whatever the setting says, the row shows
*(no fix yet)*, and announcing by hand says so too. Nodes you hear carry
their own position when they choose to share it, and that reaches a companion
app whether or not you share yours — so you can see others without being seen.

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

**An app whose module is switched off says so.** Its icon on the home screen is
struck through, and its own screen carries a line naming the module — the Mesh
screen, the GPS screen and the dialler, which are the three switches with an app
behind them. Messages and Hotspot share the modem's switch and are struck
through with the dialler.

**A module you switch off stays off across a restart.** The GPS, LoRa, modem and
sensor switches are remembered, and they are read before anything is given power
— so a module you turned off is not started at boot at all, rather than started
and then stopped once the settings screen appears. It saves the seconds of boot
that module's setup would have taken, and the boot screen marks it with a single
stroke to distinguish "you switched this off" from the cross a part that failed
to answer gets.

The mesh is the one that only half stops. Its node still comes up with the radio
switched off, because the contacts, the message log and every setting behind the
Mesh screen are worth reaching whether or not anything can be transmitted; what
waits is the radio, and it starts the moment the switch moves.

**The module power switches in Settings now take their readers with them.**
Cutting power to the GPS, the LoRa radio or the modem stops whatever was talking
to it, rather than leaving a task spending its timeouts on a module that is not
there. Switching one back on sets it up again from scratch, because a module
that has been switched off keeps nothing it was told — the modem forgets its
message format, the radio forgets its frequency.

**Switching the LoRa module off takes the link down with it.** An app connected
to a node whose radio has gone is connected to something that can no longer send
or hear anything. Powering the radio back on restores the link, without the
setting having changed in between — the same applies to the GPS icon on the home
screen, which disappears when that module is powered down rather than sitting
there searching for a receiver that is switched off.

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

**Hold the space key while the phone starts to keep the link off for that boot.**
The setting is not forgotten - turn it on again from the screen and it starts,
and the next ordinary boot brings it back as usual. This is the manual
counterpart to the crash latch below, for a link that is misbehaving badly
enough to matter without being bad enough to crash. The Companion app screen
says *held off at boot* so it is clear why nothing is running.

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

While an app is connected, the taskbar shows the link it came in over —
Bluetooth or WiFi — and the lock screen says *Companion app* where it would
otherwise count unread mesh messages. Those messages are being read on a phone,
so counting them here would be counting somebody else's post; unread texts still
raise the envelope either way, since those remain this device's own business.

Messages that arrive while no app is connected are held — sixteen of them — and
handed over when one connects, so a conversation is not lost because the phone
was in somebody's pocket. Contacts are not persisted: they are rebuilt from the
adverts nodes send anyway, so an app reconnecting after a restart re-adds
whatever this node has not heard from yet.

The node reports protocol version 7 and answers anything newer with "unsupported
command" rather than going quiet. Everything a conversation needs is there —
contacts, messages, channels, radio settings, device time, adverts, advert
paths, flood scope, signing data with the node's key, and custom variables —
while statistics and telemetry are not.

The only custom variable this node exposes is `loc_share`, the position sharing
policy, which is the one setting worth changing from an app that is not already
a command of its own. An unknown name is refused rather than quietly accepted,
so an app told a setting was stored can believe it.

**Starting up.** Bringing this board up takes several seconds, most of it the
modem, and the panel used to show one static word for the whole wait — which
looks the same as a phone that has hung. The boot screen now draws a grid of the
systems being started, fills each icon in as it comes up, and names the one
being waited on underneath. A system that never answers is left as an empty
frame; one that failed is struck through. So a glance at the end of a boot says
what is and is not working, which used to mean reading the serial log.

It costs a little boot time — each repaint is an e-paper refresh — so repaints
are rate limited: a run of fast steps collapses into one, while a slow step gets
its own, which is where the information is actually wanted. The old splash was a
full-screen refresh of its own, so this is close to free.

**Ear detect.** A capacitive panel cannot tell a cheek from a fingertip, so a
call held to your ear is a call being hung up and dialled into by the side of
your head. With *Ear Detect* on, the proximity sensor suppresses the touch panel
while the phone is against a face during a call.

**The BHI260AP motion hub is not used, and is no longer powered.** It offers an
accelerometer, a gyroscope and gesture detection, and nothing in this firmware
ever read any of it - so its driver was removed and the 1.8V rail it sits on now
starts switched off, rather than supplying a chip that does nothing for the life
of the battery. The rail is raised only long enough for the light sensor to be
looked for, and dropped again when none answers. It is not a compass: the hub
was asked directly and has no magnetometer, so a heading is only available from
GPS course over ground, and only while moving.

The settings switch that used to read *Power Gyro* now reads *Power Sensors*,
which is what it always actually did - it switches that 1.8V rail, and the light
and proximity sensor is the only thing left on it.

**On the board this was written against the setting reads "No sensor".** The
LTR-553ALS does not answer on the I2C bus at all - not cold, and not with its
1.8V supply forced up - so there is nothing to read a face with. The feature is
kept because it costs nothing when the sensor is absent and works the moment one
answers, but on this hardware it does nothing, and the setting says so rather
than offering a switch that cannot do anything.

It is off by default, and deliberately narrow. It runs only while a call is
connected, so a sensor stuck reporting "near" cannot lock the phone up. It
suppresses touch only — the keyboard still works, so there is always a way to
end the call. And its threshold is relative to a baseline taken when the call
connects, because cover glass and ambient infrared both offset the reading and
neither is the same on two devices or in two rooms.

The sensor is watched whenever the setting is on, not only during a call, and
what it reads is logged once a second — so it can be tested by turning the
setting on and waving a hand at the phone, rather than by ringing somebody.
Acting on the reading is still confined to a connected call.

**Modem LED.** The modem module has its own network status LED that blinks
whenever the phone is on the network. *Settings > Modem LED* asks the module to
stop, over AT, and asks again each time it rejoins - the setting is volatile on
some SIMCom modules and kept on others. Whether this particular module honours
the command is not something its datasheet settles, so the modem's answer is
logged either way; look for `AT+CNETLIGHT` in the serial output.

The module's second LED is its power indicator and is not under software
control at all.

**GPS.** A live readout of position, speed, satellites and time, with a reset
button in the corner. The reset is a warm start: the ephemeris goes, the almanac
and the last known position stay. Bad ephemeris is what a stuck receiver usually
has, and discarding it is enough to unstick one — while keeping the almanac is
what lets the next fix take about a minute instead of the quarter of an hour it
takes to collect a new one from the satellites.

The home screen's status bar carries the receiver's state: nothing when it is
not being read, a plain satellite when it is searching, and the satellite with
the number of satellites in use once it has a fix. Not being read is its own
state worth showing — the receiver is only powered up while the GPS screen is
open or while the mesh wants a position, so a missing icon means nobody asked,
not that nothing was found.

**Storage.** *Settings > Storage* browses both filesystems. They are nothing
alike: internal flash is soldered on, holds the contacts, the messages and the
mesh identity, and cannot be read anywhere else; the SD card can be taken out
and put in a computer. Folders open, files show their size, and tapping one
shows the start of it — enough to confirm a file holds what it should. It is
read only on purpose: a browser that can delete is one wrong tap from losing the
contacts, and a phone has no undo.

*Export to card* writes the contacts and the whole message log to
`/lilyphone` on the SD card as CSV, under a name stamped with the time so an
export never overwrites an earlier one. CSV rather than the TSV kept internally,
because the point is that a spreadsheet opens it — message bodies keep the
sender's line breaks, safely inside the quotes. Mesh messages are not included;
they only ever exist in memory.

**Calculator.** Four calculators behind one *Calc* icon, cycled with the
button at the top right; the one you were last using comes back next time.

- *Basic* is the four operations and percent.
- *Scientific* adds precedence and brackets - `2 + 3 * 4` is 14 - along with
  trig in degrees or radians, logs, powers, roots and factorials. *INV* swaps
  the keys to their inverses for one press.
- *Programmer* works on whole numbers in hex, decimal, octal or binary, with
  and/or/xor/not, shifts and modulo, at a word width of 8, 16, 32 or 64 bits
  that *WID* cycles. The line above the result shows the same value in decimal
  when you are in another base, or in hex when you are in decimal, and the
  digit keys the current base cannot use are greyed.
- *RPN* is a four-level stack - X, Y, Z and T are all on screen - with LASTx,
  ten registers, and a program memory in the HP style. *PRGM* starts and stops
  recording, and every key pressed in between is a step; *R/S* runs it, *SST*
  runs one step, *LBL* and *GTO* make it loop, and *x=0?*, *x<0?*, *x=y?* and
  *x<y?* skip the next step when they are false. *LIST* shows the program with
  the step about to run marked, and it is kept on the flash, so it survives a
  restart. Two hundred steps is the limit.

The physical keyboard works as well as the on-screen keys: digits and
operators from the symbol layer, Enter for `=` or *ENTER*, Backspace, Esc for
clear, `x` as multiply, `e` for an exponent (or the hex digit, in programmer
mode) and space for *ENTER* in RPN. The value in hand is carried across when
you change mode, so a hex number can be read back in decimal by switching to
Basic and a result from Scientific can be pushed onto the RPN stack.

**Notifications.** Vibrate on an incoming call, on an incoming text, or neither;
optionally a tone as well. All configurable and remembered across reboots.

Also inherited from the vendor firmware, and left working: a GPS readout. The
vendor's other bring-up screens were removed - the battery-detail pages and
peripheral self test, whose one useful figure is on the status bar and whose
verdicts the boot screen now gives as they happen, and a WiFi scan-and-configure
app that did nothing this phone needed: the ESP32's own WiFi serves the mesh
companion link and the hotspot, and both are set up from their own screens.

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
tools/                the emoji font and boot icon generators
```

`CLAUDE.md` documents the architecture and the traps in more detail — the
e-paper refresh model, the rule that one task owns the modem, and the LVGL
behaviours this display is unusually sensitive to.

## Status

A personal project, developed against real hardware but without automated tests;
`test/` holds only PlatformIO's placeholder. The cellular paths in particular
depend on what a given A7682E firmware and carrier will accept, and the serial
log is deliberately talkative about which AT commands were refused.
