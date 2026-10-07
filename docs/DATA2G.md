# Data2G as a chat modem

Status: chat reaches a separately running `data2g-host` over TCP. Jeff
chose KISS plus the command port (2026-09-26), and the operator starts
`data2g-host`, never the app. On the advice of Data2G's author (relayed by
Jeff, 2026-10-07) chat now sends its frames straight to Data2G's **GLISS
broadcast group**, picks a Data2G mode from the Glissando tempo, and puts
messages for one station through a connected session. Tested against a
fake host; not yet against a real one.

[Data2G](https://github.com/arodland/Data2G) is an OFDM/CPM HF data modem
with its own rate shifting. This adds it to the Glissando app as another
way to carry chat, next to Glissando and the codec2 data modes.

## Ground rules

- **No Data2G code in this repository.** No vendoring, submodule,
  linking, or copied snippets. The app talks to a separately installed,
  separately started `data2g-host` over TCP, on the same machine or
  another one. What the app needs (KISS framing, the VARA-style command
  lines, the broadcast commands) is small public protocol, written here
  from Data2G's documentation (`docs/broadcast.md`) and the KISS and VARA
  descriptions.
- This also keeps the AppImage free of Python, PyTorch and PyAudio, which
  Data2G needs to run.
- Data2G owns the sound card and the PTT (through rigctld). The app does
  not key the radio for chat while Data2G carries it.

## The ports

| Port | What chat uses it for |
|---|---|
| 8100, KISS | Chat frames to and from the GLISS group, on the KISS port `BCAST OPEN` gives it, with ACKMODE so the host says when each burst has been transmitted. |
| 8300, commands | `MYCALL`, `LISTEN ON`, `CHAT ON`, `BCAST OPEN GLISS FROM call`, `BCAST MODE`, `MODES`, `CONNECT`, `DISCONNECT`, `ABORT`. Reports read: `PTT`, `BUSY`, `MODE`, `BCAST n HEARD/LOST/DROPPED`, `CONNECTED`, `DISCONNECTED`, `BUFFER`. |
| 8301, session data | The byte stream of a connected session (always the command port + 1). |

All three ports can be changed in Preferences; the session data port
follows the command port.

## Broadcasts: the GLISS group

When the command port connects, the app sends `MYCALL`, `CHAT ON` and
`LISTEN ON` (when sessions are on), then `BCAST OPEN GLISS FROM <call>`,
and reads the KISS port number from the `BCAST PORT n` reply. Then
`MODES` lists the host's modes.

Each chat frame is written to that KISS port as it is, with no AX.25
header and no tag: the group already keeps out anybody else's traffic,
since every Data2G burst for the group carries its name and only stations
that opened GLISS take it in. The frames of one keying go in together, so
Data2G sends them as one burst. Frames heard on the group port go straight
to the chat protocol. Data2G reports no SNR over KISS, so they carry no
signal report (the chat window shows a dash).

Each frame is written as a KISS ACKMODE frame with a tag. The host answers
with the tag once the burst carrying it has gone out, which is how the app
knows the keying is over and the acknowledgement timer can start. A keying
the host never sends (it holds broadcasts during a session, and while the
channel is busy) is given up after two minutes.

The command port is now required: without it there is no group. Data2G
serves one command client at a time, so a station that also runs VarAC or
Pat on the same `data2g-host` can't use both at once.

## The tempo buttons pick the Data2G mode

The tempo the console has set (or Auto's pick, or a message's own tempo
from the chat window's menu) chooses the Data2G mode the group sends in,
through `BCAST MODE <port> <mode>` before a keying when the group's mode
differs. Each tempo takes the first of its choices the host's `MODES`
list offers:

| Tempo | 2400 Hz host | 500 Hz host (`--kiss-bw 500`) | 10 % failure SNR (AWGN) |
|---|---|---|---|
| Adagio | `fsk16r25-r1/3` | the same | -12.3 dB, Data2G's most sensitive |
| Andante | `fsk8r50-r1/3` | the same | -10.1 dB |
| Allegro | `fsk32r62-r1/3` (2.3 kHz) | `n10-qpsk-r1/3` | -7.9 dB |
| Presto | `qpsk-r1/3` (1.2 kHz) | `n10-qpsk-r1/2` | -2.8 dB |
| Duet | `w48-16qam-r1/2` (2.4 kHz) | `n10-16qam-r1/2` | +7.6 dB |

Thresholds are Data2G's own, from its `mode_thresholds.json`. The old
broadcast default, `qpsk-r1/5`, sits at -4.9 dB, so Adagio is about 7 dB
more sensitive than what chat used before.

How long a burst takes, from the airtimes `MODES` gives (host at
`8ffe15c`):

| Tempo | One text frame | Two text frames | Ping | Ack timeout |
|---|---|---|---|---|
| Adagio | 26 s | 37 s | 16 s | 112 s |
| Andante | 18 s | 25 s | 11 s | 82 s |
| Allegro | 9 s | 13 s | 6 s | 52 s |
| Presto | 3 s | 4 s | 3 s | 32 s |
| Duet | 2 s | 2 s | 2 s | 25 s |

The chat protocol's timers follow the tempo's mode, and change when the
tempo does or when the host's own `MODES` list arrives. If none of a
tempo's choices is offered, Adagio and Andante take the host's first
(slowest) mode and the others its middle one.

## Messages to one station: sessions

A message to a station picked in the call roster, and its pings and
acknowledgements, can go through a connected Data2G session instead of
the group. Data2G then negotiates the session, rate-shifts it to the path,
and resends what is lost. **Connect a session for messages to one
station** in Preferences turns this on (on by default).

- The keying goes through a session only when every frame in it is for
  that one station. Broadcasts, and anything for more than one station, go
  to the group.
- The app sends `CONNECT <mycall> <theircall>`. Once `CONNECTED`, the
  chat frames are written to the session data port, each as `G`, a length
  byte and the frame, so a stream from a program that is not Glissando is
  told apart and the session dropped.
- The keying is over when PTT has gone on and off and `BUFFER 0` says the
  host has nothing left to send.
- The chat protocol still sends its own acknowledgement inside the
  session, so the chat window's delivery states work as they do on the
  group.
- The station that called closes the session once nothing has gone
  either way for 45 s, or straight away when a keying for someone else is
  waiting. Data2G sends no broadcasts during a session.
- With `LISTEN ON`, the host takes sessions other Glissando stations
  open to us, and frames arriving in them go to the chat protocol.
- If the station doesn't answer (`DISCONNECTED` while connecting, or no
  `CONNECTED` within 90 s, after which the app sends `ABORT`), the keying
  goes to the GLISS group instead, and that station gets group messages
  for the next ten minutes before a session is tried again.

## What was built

- `app/src/text_messaging/Data2GLink.{h,cpp}`: KISS framing with ports and
  ACKMODE, the command lines (including the broadcast reports and the
  `MODES` list), the tempo-to-mode table, burst lengths and timers, and the
  session stream framing. No sockets, so it is all unit tested.
- `app/src/text_messaging/Data2GTransport.{h,cpp}`: the
  `ITextMessagingTransport` over TCP, on its own thread, reconnecting
  every 2 s (backing off to 30 s) while data2g-host is not there. A
  callsign change reconnects the command port so the group reopens under
  the new call.
- Preferences, Modem tab, Text Chat: **Send chat through Data2G**, host
  (default `127.0.0.1`), KISS port (8100), command port (8300), and the
  sessions switch. Bandwidth (`--kiss-bw`) is set when starting
  data2g-host.
- With Data2G chosen, frames our own modem decodes are left out of the
  conversation (their replies would go out on the other modem) but still
  show in the snooping window.
- The chat window shows the Data2G host, whether it is reachable, the
  group's mode, and any session.

## Tests

- **Unit** (`fdv_text_messaging_data2g_test`, in CI with the other
  `text_messaging` tests): KISS ports and ACKMODE, command lines and the
  `MODES` list, the tempo-to-mode table for wide and 500 Hz hosts, the
  session stream, and chat protocols on two fake data2g-hosts on
  localhost: a broadcast on the group at Adagio's mode, Duet's mode, a
  directed message through a session with its acknowledgement, a station
  without sessions getting the group instead, a lost command port, and a
  callsign change reopening the group. Clean under ThreadSanitizer.
- **Bench** (manual, not yet run): two `data2g-host` instances on
  PulseAudio null sinks, each with a Glissando app attached; chat both
  ways at Adagio and Duet, a broadcast, a directed message through a
  session, and a ping. Data2G needs PyTorch, so this stays out of CI.

## Next

- Run the two-host bench and check the timers against real bursts.
- `BCAST MODE n AUTO` (Data2G's own rate shifting on the group) could
  stand in for Auto when the console is on Auto.
- Larger frames: a whole message would fit in one Data2G frame.
