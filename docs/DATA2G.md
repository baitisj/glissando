# Data2G as a chat modem: integration plan

Status: proposal, for Jeff to approve before any app code is written.

[Data2G](https://github.com/arodland/Data2G) is an OFDM/CPM HF data modem
with its own gear shifter. This plan adds it to the Glissando app as a
third way to carry chat, next to Glissando and the codec2 data modes.

## Ground rules

- **No Data2G code in this repository.** No vendoring, submodule,
  linking, or copied snippets. The app talks to a separately installed,
  separately started `data2g-host` over TCP, on the same machine or
  another one. Everything the app needs (KISS framing, AX.25 headers, the
  VARA command lines) is small, public protocol and is written here from
  the protocol descriptions.
- This also keeps the AppImage free of Python, PyTorch and PyAudio, which
  Data2G needs to run.
- Data2G's repository carries no licence file as of commit `da4cb0e`
  (2026-09-26), so copying from it would not be permitted anyway. Talking
  to it over a socket needs no licence.

## What Data2G offers a client

Read from Data2G at `da4cb0e`; its interfaces are still moving.

`data2g-host` runs two "personalities" on one radio, both on by default:

| Port | Protocol | What it is |
|---|---|---|
| 8100 | KISS over TCP | Datagrams. Queued frames go out combined into one burst. Every frame heard is sent to every connected client. No resends. |
| 8300 | VARA command lines (CR-terminated) | `MYCALL`, `LISTEN`, `CONNECT`, `DISCONNECT`, `BW500`/`BW2300`, `CHAT ON`, `CQFRAME`. Notifications: `PTT ON/OFF`, `BUSY ON/OFF`, `MODE submode`, `CONNECTED`, `DISCONNECTED`, `BUFFER n`, `IAMALIVE`. |
| 8301 | VARA data | The byte stream of one connected ARQ session. |

Things that shape the design:

- **Data2G owns the audio and PTT.** It opens the sound card itself
  (PyAudio) and keys the radio through rigctld.
- **KISS mode shifting follows AX.25.** Each KISS burst carries reports on
  the stations it has heard, and connected-mode AX.25 frames (I, S) to a
  station with a fresh report go in the mode that station asked for. UI
  frames and non-AX.25 frames always go in the robust broadcast mode
  (`qpsk-r1/5`, or `n10-qpsk-r1/5` with `--kiss-bw 500`).
- **The command port reports PTT and BUSY for KISS traffic too**
  (`Host.after_step` announces the engine's PTT and
  `Receiver.channel_busy` whatever sent the burst).
- **Only one command client at a time.** A new client replaces the old
  one, and a client that drops makes the host stop listening for ARQ
  connects. A station running VarAC or Pat against the same host would
  lose its command connection to us.

## Recommended interface: KISS for frames, the command port for status

The chat layer already has the right seam:
`TextMessaging::ITextMessagingTransport` (`transmit`, `isTransmitting`,
`isChannelBusy`, `poll`) plus `TextMessagingProtocol::onFrameReceived`.
Today `TextMessagingTransport` implements it over the in-process modem. A
new `Data2GTransport` implements the same interface over sockets, and the
protocol (fragmenting, acknowledgements, retries, pings, heard list) runs
unchanged.

**Frames (port 8100).** Each chat frame the protocol emits is wrapped in
an AX.25 UI frame and written as one KISS data frame:

- Source: our callsign. Destination: the addressee, or `GLISS` for a
  broadcast. PID `0xF0` (no layer 3).
- Info field: the chat frame exactly as `FrameCodec::encode` builds it
  today, with a leading tag byte so we can tell our frames from APRS or
  other KISS traffic the host passes up.
- A keying's fragments go in together, so Data2G combines them into one
  burst.
- Received KISS frames that are AX.25 UI with our tag go to
  `onFrameReceived`; anything else is dropped (and counted, for the log).
- Data2G reports no SNR over KISS, so received frames carry "unknown";
  the heard list shows a dash instead of a number.

Real AX.25 addressing is what lets Data2G see stations at all: its link
layer reads callsigns from the frame, and every burst we send carries
its reception reports.

**Status (port 8300, optional, on by default).** The transport also keeps
a command connection open, sends `MYCALL` and nothing else that changes
state, and reads:

- `PTT ON/OFF` into `isTransmitting()`, which is what starts the
  acknowledgement timer at the true end of our burst.
- `BUSY ON/OFF` into `isChannelBusy()`, which freezes the protocol while
  somebody else's burst is being received.
- `MODE submode` for the console's telemetry line.

With the command port turned off (for a station that also runs VarAC on
the host), the transport falls back to timing from `AirTiming`: it treats
itself as transmitting for an estimated burst length after each write,
and never reports the channel busy (Data2G still listens before talking
on its own side).

**Timers.** Data2G plans broadcast bursts in its longest size class
(12 s, `kisslink.BROADCAST_S`), and a burst grows past it when a frame
needs more room; its own turnaround estimate is 1.3 s. A `Data2GTransport` supplies its own
`AirTiming`, sized from measured burst lengths on the loopback bench
rather than guessed; Glissando's `forFrameSeconds` is the model.

**Frame size.** Phase 1 keeps the current 14 and 54 byte frames, so the
protocol and its tests need no change. Data2G would happily carry a whole
312-byte message in one frame; larger frames are a follow-up once the
basic path works.

### Why not the VARA ARQ session (8300/8301)?

It would give reliable delivery and full gear shifting, but:

- It is connection-oriented, one peer at a time: no broadcasts, and a
  CONNECT/DISCONNECT round per message.
- Its retries and acknowledgements duplicate the ones our protocol
  already does, so the chat window's delivery states would have to be
  rebuilt around session events.
- It fights over the single command connection with VarAC-style clients.

It remains a good later option for long messages or files to one
station; the transport interface leaves room for it.

### Known limitation: robust mode only, at first

Because chat frames go as UI frames, Data2G sends them all in its robust
broadcast mode. We expect that mode to be several times faster than
Glissando's quickest gear, but that is inferred, not measured; the bench
below measures it. Getting per-station mode
shifting would need either sending chat as AX.25 I frames outside a real
AX.25 connection (which misuses AX.25 on the air), or a Data2G option to
shift UI frames addressed to a station with a fresh report. The second
is a change for Data2G's author to consider; we would ask, not patch.

## App changes

All in `app/`, after the UI rework in the thread "Glissando front end,
Chaotica style" has landed:

1. `app/src/pipeline/Data2GTransport.{h,cpp}`: KISS encoder and decoder,
   AX.25 UI header, VARA command reader, reconnect with backoff, all on a
   socket thread that hands frames to the protocol.
2. The console's mode choice gets **Data2G (external)**. Selecting it
   stops the app keying the radio and sending audio. Receive audio can
   still feed the waterfall where PulseAudio lets both programs open the
   same capture device.
3. Preferences: Data2G host (default `127.0.0.1`), KISS port (8100),
   command port (8300) with an on/off switch, and a note that bandwidth
   (`--kiss-bw`) is set when starting `data2g-host`.
4. Chat window: a connection indicator (connected, reconnecting, not
   running) and the last `MODE` heard.
5. PTT: when Data2G is selected, the app's own rig control must not key
   the radio. Running rigctld and pointing both programs at it keeps the
   app's frequency display working without a serial-port fight.

## Tests

- **Unit** (in the existing `text_messaging` test binary): KISS escaping
  round trips, AX.25 header build and parse, foreign frames ignored, and
  the protocol driven through `Data2GTransport` against a fake in-process
  KISS and command server (including PTT/BUSY sequencing and the
  no-command-port fallback).
- **Bench** (manual, on Jeff's Linux box): two `data2g-host` instances on
  PulseAudio null sinks, as `prototype/bench.py --null-sink` already sets
  up, each with a Glissando app attached; chat both ways, a broadcast,
  and a ping. Data2G needs PyTorch, so this stays out of CI.

## Decisions for Jeff

1. KISS for frames plus the command port for status, as above
   (recommended), or the VARA ARQ session.
2. Whether the app should ever start `data2g-host` itself. The plan says
   no: the operator starts it, and the app connects wherever it runs.
