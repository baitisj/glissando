# Data2G as a chat modem

Status: chat reaches a separately running `data2g-host` over TCP. Jeff
chose KISS plus the command port (2026-09-26), and the operator starts
`data2g-host`, never the app. On the advice of Data2G's author (relayed by
Jeff, 2026-10-07) chat now sends its frames straight to Data2G's **GLISS
broadcast group**, picks a Data2G mode from the Glissando tempo, and puts
messages and files for one station through a connected session. Tested
against a fake host; not yet against a real one.

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
channel is busy) is given up after two minutes, not counting time a
session holds the group back.

On the group too, data2g-host takes its own turns: it waits for a clear
channel and for any session to end. So the app keeps none of its own
pauses there (Jeff, 2026-10-08): no turnaround after hearing a frame, no
reply windows, no channel-busy hold and no retry backoff, and the chat
window shows no countdown on a queued message. Only "Woah!" holds it.
The app still hands over one keying at a time, the next as soon as the
host reports the last one sent, so whatever waits behind it stays in the
app's queue, where it can be cancelled. A keying the host has been given
but not yet sent (waiting for its mode, a clear channel or the end of a
session) can be cancelled too, and is dropped when sending is inhibited;
one that has started goes out. A directed message on the group still
waits for the far end's chat acknowledgement, since the group has no
acknowledgement of its own.

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

A message or ping to a station picked in the call roster goes through a
connected Data2G session instead of the group. Data2G negotiates the
session, picks its own speed for the path, resends what is lost and
acknowledges what arrives. **Connect a session for messages to one
station** in Preferences turns this on (on by default).

In a session the modem does the work our own chat protocol does on the
group, so the protocol stands aside (Jeff, 2026-10-08):

- **The modem's acknowledgements settle each message.** `BUFFER n` is the
  count of our bytes the far end's modem has not yet acknowledged. Since
  Data2G's PR #51 it is exact for a client that sent `CHAT ON` (which the
  app does). The app writes everything waiting for the station in one go
  and notes where each message ends in the byte stream. As the count falls
  past a message's end, that message shows OK. A ping shows "PING
  delivered" instead of a pong.
- **The modem's timeouts end them.** The app adds no acknowledgement timer
  and no retries of its own. If the session is lost (`DISCONNECTED`)
  before a message is acknowledged, it shows NO ACK; Data2G has already
  retried for longer than we would. A `BUFFER 0` that comes after
  `DISCONNECTED` is the host listening again, not a delivery.
- **No acknowledgements of our own.** Frames that arrive through a session
  are not answered with a chat ACK, pong or report of missing fragments,
  so the Auto acknowledge button makes no difference there. It still
  matters for the group.
- **No pauses.** A message for a session goes to the transport as soon as
  it is queued. The app's turnarounds, reply windows, channel-busy holds
  and retry backoffs don't apply. Frames arriving through a session hold
  nothing up either. Only "Woah!" holds them, as it holds everything.
- **No tempo.** The session picks its own speed, so the chat window shows
  no tempo on such a message and offers no "Change Tempo to...". The
  console's tempo still picks the GLISS group's mode.

- **Cancelling.** A message the app has not yet written into the session
  can be cancelled, and is dropped when sending is inhibited (outside the
  US data segments). One already written is the modem's to finish: it
  ends OK or NO ACK as the modem decides. Deselecting the station (below)
  ends the session itself.

An older data2g-host (before PR #51) reports only 1 for "something is
unacknowledged". The app tells the two apart from the first `BUFFER` after
it writes, which on a new host counts at least what was written. With an
older host, messages written together are settled together, at
`BUFFER 0`. A host on another machine may read a large write in pieces and
answer each with a `BUFFER`; nothing is settled by count until one has
counted the whole write. The chat window shows how many bytes are waiting
only when the host counts them.

How the session itself runs:

- Only messages, pings and files (below) go through a session.
  Broadcasts, and the protocol's own replies to traffic heard on the
  group, go to the group.
- The app sends `CONNECT <mycall> <theircall>`. Once `CONNECTED`, chat
  frames are written to the session data port, each as `G`, a length byte
  and the frame, so a stream from a program that is not Glissando is told
  apart and the session dropped.
- A session the app opened is closed once nothing of ours is waiting on
  it and nothing has gone either way for 45 s, or straight away when
  something for another station, or for the group, is waiting. Data2G
  sends no broadcasts during a session. A session the far end opened is
  left to it, unless something else has waited 45 s.
- Deselecting the station in the call roster (clicking it again, picking
  another station, or removing it) ends the session with it at once and
  aborts every message and ping still outstanding for it, so the group is
  free for broadcasts straight away. With nothing of ours unacknowledged
  the app sends `DISCONNECT`, which tells the far end; otherwise `ABORT`,
  since `DISCONNECT` would wait for the acknowledgements, and the far end
  then finds the session gone when it stops hearing us. Over Glissando's
  own modem deselecting changes nothing, as before.
- With `LISTEN ON`, the host takes sessions other Glissando stations open
  to us, and frames arriving in them go to the chat protocol.
- If the station doesn't answer (`DISCONNECTED` while connecting; Data2G
  calls five times), the message goes back to the queue and on to the
  GLISS group, with our own acknowledgement and retries. That station
  then gets group messages for the next ten minutes before a session is
  tried again. The app gives up on a host that never answers a `CONNECT`
  after 180 s, and on a message that has waited ten minutes for its
  session (the channel busy with sessions of other stations).

## Files through a session

A file can go to one station through a connected session, the way VarAC
sends one. Only there: never on the GLISS group, and never over
Glissando's own modem. A file to everybody on the group is below.

**Sending.** Right-click a station in the call roster and pick **Send
File...** (dark, with a line under it saying why, unless Data2G is the
chat modem with sessions on and data2g-host's ports are up). The file
picked is queued for the station, which is then selected, as for a
message. A file over 100 kB asks first, and so does selecting the station
when a file is still going to or from the one selected before, as letting
go of that one cancels it. The session opens as for a message, and the
offer is the first thing written into it. One file goes to a station at
a time; another waits its turn. The chat shows a line for it that
follows it: `FRED.TXT, 7,000 bytes: offered`, then `sending 3,200 of
7,000` (bytes the far end's modem has acknowledged), then `delivered`,
`declined`, `cancelled`, `cancelled by W1AW`, `failed`, `expired`,
`failed: their Glissando can't take files` or `failed on W1AW's side`.
Right-click the line for **Cancel Transfer**.

**Receiving.** A box over the chat asks `W1AW offers FRED.TXT (7,000
bytes). Save it?` with **Save as...** and **Decline**, and the console's
COMMS button flashes as for a message. Save as... opens the system's save
dialog in the received files folder with the offered name filled in;
choosing a place accepts the file, and backing out declines it. The file
is written to `<name>.part` (`<name>.2.part` and so on if a file of that
name is there already) and renamed once all of it has come; the chat line
counts it in, and has the same Cancel Transfer. A name another file is
being saved under is refused, and the offer stays open. Nothing is
opened or run after saving.

Preferences, Modem tab, under the Data2G settings: **Save received files
in** (the folder the save dialog starts in; saving somewhere else makes
that folder the one used next time) and **Accept files without asking
from**, a list of callsigns whose files go straight into that folder,
still through a `.part` file, under the offered name (numbered, `FRED
(2).TXT`, when one is there already; with no number free up to 999 the
operator is asked instead). A file that turns up under that name while
the transfer runs is not replaced either: the file takes the next free
number when it is renamed. Only a name chosen in the save dialog, which
asks, replaces a file. The chat line says it was saved without
asking.

How it goes in the session's byte stream, beside the chat frames (`G`, a
length byte, the frame): `F`, the record's type, a 2-byte big-endian
length, then the body, whose first byte is the sender's number for the
transfer.

| Record | From | Body after the number |
|---|---|---|
| 1 Offer | sender | size (4 bytes, big-endian), the file's name (UTF-8, up to 200 bytes) |
| 2 Accept | receiver | |
| 3 Decline | receiver | |
| 4 Data | sender | the next piece of the file, in order, up to 4,096 bytes |
| 5 Saved | receiver | (the file is on disk under its name: this, not the modem's acknowledgement, is delivery) |
| 6 Cancel | either | a reason: 1 stopped, 2 couldn't read, 3 expired from the sender; the same with the top bit set (0x81 to 0x83) from the receiver |

Each station numbers its own transfers; the reason's top bit says whose
transfer a Cancel ends. Records of a type it doesn't know are ignored, so
later versions can add some.

Pacing. The next piece is written only once data2g-host has read what
went before and no more than a quarter of a piece is still
unacknowledged, so the modem always has the rest of a piece to send, and
a message typed during a file, or a Cancel, goes in behind at most that
much. The app keeps the least data2g-host can have read and the least
the far end can have acknowledged from the run of `BUFFER` counts (a
count that rose by n means at least n more were read; acknowledgements
only lower it), so a message written between pieces is still settled at
its own end. When the host reads a write in the same step as it takes an
acknowledgement bigger than it, its one answer falls instead of rising;
then everything is settled at `BUFFER 0` (data2g-host answers every write
with a count, never 0, and never below the size of the write), and the
next piece waits for that. A count below the size of the last write may
be from before the host read it, reporting only acknowledgements of what
went before, so a `BUFFER 0` after it settles only what was read. With an older host that only says 1 for "some", pieces go
one at a time, each once everything before it is acknowledged.

When things go wrong:

- An offer waiting for its answer counts as traffic: the 45 s idle close
  doesn't fire, on either side. An offer unanswered for 4 minutes expires
  on both sides (each side's clock, and a Cancel saying so).
- The session lost mid-file: `failed` on both sides (why is in the
  status line), and the receiver deletes its `.part`. No resume in this
  version.
- The sender cancels: no more pieces, and a Cancel; the receiver deletes
  what it has and drops pieces that still come after it. Cancelled once
  every piece is written, the receiver may have saved it already; then
  it ignores the Cancel, and its Saved, crossing it, shows `delivered`.
- The receiver cancels: a Cancel back; the sender stops and shows
  `cancelled by W1AW`.
- Deselecting the station ends the session at once, as before, and its
  files are cancelled on our side; the far end sees the session gone.
- A station running an older Glissando takes the `F` for a stream that
  isn't chat and closes the session. A session the far end ends while our
  offer is unanswered so shows `failed: their Glissando can't take
  files`. One that ends on this side (data2g-host's command or data port
  lost, a write to it failing) shows `failed`.
- The receiver can't write the file: a Cancel with that reason, and the
  sender shows `failed on W1AW's side`.
- An odd name offered (`../../x`, control characters, nothing, `CON`):
  only its last path part is kept, without control characters, path
  separators, the characters Windows refuses, or leading and trailing
  dots and spaces; a Windows device name, or nothing left, becomes
  `received-file` (with any extension it had); so do `COM¹` to `COM³`
  and `LPT¹` to `LPT³`, which Windows reserves too.
- A station that lately didn't take a session for a message is still
  called for a file, which has no group to fall back on. A file that
  can't get its session (no answer, data2g-host busy, or ten minutes of
  waiting) fails.
- Sending is off while the station is receive only (outside the US data
  segments).

Known limit: locators. A station learns that another takes locator
frames from its pings and acknowledgements. In a session there are no
chat acknowledgements, so two stations that only exchange session
messages learn it only from a ping.

## Files to the GLISS group

A file can also go to everybody on the GLISS group at once. Nobody
confirms it: the sender streams it, then opens short windows in which
stations still missing pieces ask for them, and resends what was asked,
until the windows go quiet or its time is up. Only with Data2G as the
chat modem, never over Glissando's own modem, and never from a station
that is receive only.

**Sending.** Right-click in the chat and pick **Send File to Group...**
(dark, with a line saying why, as Send File... is). The file
can be 64 KiB at most. Before it goes a box gives the estimate at the
tempo the console has set now: `FRED.TXT, 7,000 bytes, 32 pieces at
Presto: about 2 min on the air, then repairs until 14:32 at the latest.
Stations will not confirm receipt.` Above 15 minutes of air the box warns
that this holds the group for a long time, names the faster tempos with
their times, and defaults to No. Above an hour it refuses, naming the
faster tempos. One file goes to the group at a time.

The chat line follows it: `announced`, `sending 12 of 32`, `repairs
open, round 1 (14 s left)`, `round 2: resending 5 pieces, W1AW first in
line (+1 others)`, `idle; the next window in 40 s`, and at the end
`ended: no more requests`, `ended: repair time over`, `ended: stopped
serving repairs`, `cancelled` or `failed`. Under it, who asked and when,
and the reminder that silence says nothing. Right-click it for **Stop
Serving Repairs** (the stream, if still going, finishes, then an End) and
**Cancel Transfer** (an End that tells listeners to delete what they
have).

**Receiving.** A file heard on the group gets a line on the left, `W1AW
is sending FRED.TXT, 7,000 bytes: have 12 of 32`, with **Receive...** and
**Ignore** as links on it (and in its right-click menu). Pieces are kept
in memory whatever the operator says, so a file heard whole can be saved
at once; Receive... opens the save dialog in the received files folder,
and the missing pieces are then asked for in the sender's windows. The
line counts them in (`asking in slot 3 (in 12 s)`, `W1AW first in line;
4 of your pieces coming`) and says `saved to ...` once the file's hash
matches. Ignore stops asking and drops the pieces. Pieces of an
unfinished file are kept for a day (up to 4 files and 4 MB), so the same
file sent again by the same station, which has the same id, finishes it.

Preferences, Modem tab: **Receive group files automatically** (off by
default) saves every group file into the received files folder without
asking, numbering a name already there, but only while the chat window's
**Auto acknowledge** is lit too. Nothing is opened or run after saving.

**Tempo.** The piece size is fixed when the file is announced, from the
tempo then (106 bytes at Adagio, 220 at Presto, 228 at Duet). Every
keying after that goes at the tempo the console has set at that moment,
so turning the console mid-transfer changes the pace, not the pieces. A
Window says which tempo to ask in.

### On the air

Each frame goes on the group port as chat frames do. Its first byte's
top nibble is 0, which the chat frame codec never uses, so chat never
takes a file frame and a file frame never reaches the chat. All numbers
are big-endian; a call is a length byte and the call; the id is 3 bytes.

| Frame | Layout after the type byte |
|---|---|
| `01` Announce | id, version (1), size (3), piece size P (1), pieces N (2), the file's SHA-256 first 8 bytes, tempo (0 Adagio .. 4 Duet), first-pass airtime in s (2), call, name (length byte, UTF-8, up to 64) |
| `02` Data | id, piece number (2), the piece (P bytes, the last one shorter) |
| `03` Window | id, round (0: the end of the stream), size (3), P, N (2), hash (8), slots M, slot length in 0.5 s, the sender's time left in 10 s (2), a call given slot 0 (or empty), then the tempo to ask in (an addition: a listener that doesn't know it uses the Announce's) |
| `04` Request | id, round (`FF`: outside any window), flags (1 needs the Announce, 2 more missing than listed, 4 the hash failed: all of it again), total missing (2), call, the pieces |
| `05` Grant | id, round, flags (1 an Announce follows), the call first in line, count, the pieces that follow, ascending |
| `06` End | id, reason (0 quiet, 1 time over, 2 stopped, 3 cancelled: delete it, 4 failed), rounds |

A list of pieces is an encoding byte, then either a bitmap (0: the first
piece in 2 bytes, then a bit per piece from it, most significant first)
or ranges (1: 2 bytes of first piece and a byte of count - 1 each),
whichever is shorter; when not all fit the frame, the lowest are listed.
The id is the first 3 bytes of SHA-256 over the sender's call, the file's
SHA-256 and its size (4 bytes); SHA-256 is Glissando's own
(`Sha256.{h,cpp}`, checked against the standard vectors).

A piece is a Data frame of whole codewords less its 6-byte header: three
codewords in Data2G's FSK (CPM) modes, which send one piece per keying,
and as many as fit 255 bytes in its OFDM modes, which send up to 8 per
keying within 12 s. An Announce goes before the stream and again every
20 pieces.

**Windows.** After the stream, round 0's window has 4 slots; later ones
`ceil(1.5 x stations that asked) + 1`, kept to 3..8, plus 2 for each
station data2g-host reports lost on the group, 12 at most. A slot is a
one-codeword Request's airtime plus 4 s (20 s at Adagio, 6 s at Duet). A
listener asks in a random slot, up to 2 s into it, and leaves out pieces
others asked for in the same window. The sender then picks who is first
in line, in turn (last round's goes to the back, and a station named 3
times without getting anything is passed over), sends a Grant and resends
the union of what was asked, up to 600 s of air and 255 pieces, that
station's first. After one quiet window the next comes at once, after a
second one after a window's length, and after a third the End.

**Time.** The sender serves repairs for the first pass's airtime, at
least 10 minutes and at most 2 hours, from the end of the stream; time a
session holds the group is not counted. A listener that hears nothing for
`max(60 s, 3 x (a piece's keying + 5 s))` asks outside any window (round
`FF`), after a random 0 to 30 s; it gives up when the file goes quiet for
long, or 2 minutes after the sender's time left runs out.

**Taking turns.** File keyings are handed to data2g-host one at a time,
each once the host acknowledges the last (ACKMODE), and a chat keying
always goes first. After each of our file keyings the app leaves the
channel for 4 s (2 s more after any busy report), so others can get in;
chat keeps no such pause. Woah! holds file keyings too. A file keying the
host doesn't send within 2 minutes (not counting a session) is tried once
more; lost twice, the transfer fails.

**When things go wrong.** The hash doesn't match: the listener asks for
all of it again once, then the line says `failed verification: nothing
saved`. The sender cancels: listeners delete what they have
(`cancelled by W1AW`). The station turns receive only: our transfer
ends without an End (`failed`), and listeners time out; listening goes on,
but no Requests go. Chat moved off Data2G: our transfer fails. The
command port lost mid-keying: the keying counts as lost, as above.

## What was built

- `app/src/text_messaging/Data2GLink.{h,cpp}`: KISS framing with ports and
  ACKMODE, the command lines (including the broadcast reports and the
  `MODES` list), the tempo-to-mode table, burst lengths and timers, and the
  session stream framing. No sockets, so it is all unit tested.
- `app/src/text_messaging/Data2GFileTransfer.{h,cpp}`: files through a
  session: offer, answer, pieces, Saved, cancel and expiry for each
  station, safe names, and the `.part` file. No sockets, so it is unit
  tested on its own.
- `app/src/text_messaging/Data2GBroadcast.{h,cpp}`: files to the GLISS
  group: the frames, piece sizes and estimates, and the engine for both
  ends (stream, windows, turns, resends, spool, deadlines), driven by the
  transport with the time passed in. No sockets and no clock of its own,
  so it is unit tested with a simulated one.
- `app/src/text_messaging/Sha256.{h,cpp}`: SHA-256, written for
  Glissando.
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
  group's mode, and any session; and files, as above.

## Tests

- **Unit** (`fdv_text_messaging_data2g_test`, in CI with the other
  `text_messaging` tests): KISS ports and ACKMODE, command lines and the
  `MODES` list, the tempo-to-mode table for wide and 500 Hz hosts, the
  session stream, and chat protocols on two fake data2g-hosts on
  localhost: a broadcast on the group at Adagio's mode, Duet's mode, a
  directed message through a session settled by the modem with no chat
  acknowledgement, three messages each settled as its bytes are
  acknowledged, an older host settling them together, a lost session
  failing a message, a ping through a session, deselecting the station
  ending its session (ABORT with messages outstanding, DISCONNECT without), a station without sessions
  getting the group instead, a lost command port, and a callsign change
  reopening the group; the `F` records (every type, odd lengths, split
  across reads, interleaved with chat frames); and files through a
  session: a 7,000-byte file end to end, declined, cancelled by either
  side, a lost session, an offer expiring, an odd name saved safely
  without asking, a message overtaking a large file, a message whose read
  is answered together with an acknowledgement, counts sent before the host
  reads a piece settling nothing of it, an offer crossing the far end's
  DISCONNECT, deselecting the station mid-file, a far end running an older
  Glissando, a lost command port failing an offer, two files for one
  station going in turn, a file through an older host, and an offer
  expiring on one side's clock. Clean under ThreadSanitizer.
- **Unit** (`fdv_text_messaging_data2g_file_test`): the file engine on
  its own, two engines passing records by hand: safe names, a file end
  to end, an empty file, declined, cancelled each side with late pieces
  dropped, a Cancel crossing the receiver's Saved, expiry on each side's
  clock, a lost session, an older Glissando (and a session ended on this
  side, which is not that), one file at a time, auto-accept numbering a
  clash, a file made meanwhile under an auto-accepted name kept, a
  hostile name, direction controls in a name, a part file of the
  operator's left alone, two files refused one name, auto-accept never
  overwriting, a receiver that can't write, more data than offered, and
  releasing the station.
  `TextMessagingProtocolTest` covers the protocol's side against a fake
  link, and its older tests, unchanged, show nothing changes for our own
  modem.
- **Unit** (`fdv_text_messaging_data2g_broadcast_test`): the group file
  frames both ways (and that chat never takes one), piece lists, piece
  sizes and estimates, and engines on a simulated group with a simulated
  clock: a clean channel, two lossy listeners, a late joiner, the same file
  sent again, cancel, the deadline, the console's tempo changing
  mid-transfer, receiving only when told and ignoring, a hash failure
  (once restarted, twice failed), lost windows answered by a late
  request, turns going round, stopping serving, and the limits. In
  `fdv_text_messaging_data2g_test`, the same through transports and fake
  data2g-hosts sharing one group: one sender and two lossy listeners, a
  late joiner, the same file again, cancel, the deadline, a chat message
  going ahead of the file, and no group without the command port. Clean
  under ThreadSanitizer.
- **Unit** (`fdv_text_messaging_sha256_test`): SHA-256 against the FIPS
  180-4 and NIST vectors, whole and in pieces.
- **Bench** (manual, not yet run): two `data2g-host` instances on
  PulseAudio null sinks, each with a Glissando app attached; chat both
  ways at Adagio and Duet, a broadcast, a directed message through a
  session, and a ping. Data2G needs PyTorch, so this stays out of CI.

## Next

- Run the two-host bench and check the timers against real bursts.
- `BCAST MODE n AUTO` (Data2G's own rate shifting on the group) could
  stand in for Auto when the console is on Auto.
- Larger frames: a whole message would fit in one Data2G frame.
- Group files: keep the pieces of an unfinished file on disk, so they
  outlive a restart; show file frames in the snooping window; check the
  window and slot timings on the bench.
