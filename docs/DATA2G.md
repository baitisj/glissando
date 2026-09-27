# Data2G as a chat modem

Status: Jeff chose KISS plus the command port (2026-09-26), and the
operator starts `data2g-host`, never the app. Phase 1 below is built;
it has been tested against a fake host, not yet against a real one.

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

## Interface: KISS for frames, the command port for status

The chat layer already has the right seam:
`TextMessaging::ITextMessagingTransport` (`transmit`, `isTransmitting`,
`isChannelBusy`, `poll`) plus `TextMessagingProtocol::onFrameReceived`.
Today `TextMessagingTransport` implements it over the in-process modem. A
new `Data2GTransport` implements the same interface over sockets, and the
protocol (fragmenting, acknowledgements, retries, pings, heard list) runs
unchanged.

**Frames (port 8100).** Each chat frame the protocol emits is wrapped in
an AX.25 UI frame and written as one KISS data frame:

- Source: the callsign the chat frame carries (six characters and an
  SSID, as far as AX.25 allows). Destination: always `GLISS`. Chat frames
  address stations by a callsign CRC inside the frame, and the transport
  only sees encoded frames, so it has no addressee to put here; Data2G
  sends UI frames in its broadcast mode whoever they are for anyway.
  PID `0xF0` (no layer 3).
- Info field: the tag `GLS` and a version byte (1), then the chat frame
  exactly as `FrameCodec::encode` builds it, so our frames are told apart
  from APRS or other KISS traffic the host passes up.
- A keying's fragments go in together, so Data2G combines them into one
  burst.
- Received KISS frames that are AX.25 UI with our tag go to
  `onFrameReceived`; anything else is dropped.
- Data2G reports no SNR over KISS, so received frames carry NaN and the
  chat window shows a dash. History keeps it as 0, which it leaves blank.

Real AX.25 source callsigns are what let Data2G see stations at all: its
link layer reads them from the frame, and every burst we send carries
its reception reports.

**Status (port 8300, optional, on by default).** The transport also keeps
a command connection open, sends nothing on it, and reads:

- `PTT ON/OFF` into `isTransmitting()`, which is what starts the
  acknowledgement timer at the true end of our burst.
- `BUSY ON/OFF` into `isChannelBusy()`, which freezes the protocol while
  somebody else's burst is being received.
- `MODE submode` for the chat window's Data2G line.

A keying is "transmitting" from the KISS write until PTT goes on and off
again. If data2g-host never keys for it (it holds KISS traffic during an
ARQ session), the keying is given up after 60 s. PTT for anybody else's
traffic on the host also counts, so chat does not queue behind it.

With the command port turned off (for a station that also runs VarAC on
the host), the transport treats itself as transmitting for 14 s after
each write, and never reports the channel busy (Data2G still listens before talking
on its own side).

**Timers.** Data2G plans broadcast bursts in its longest size class
(12 s, `kisslink.BROADCAST_S`), and a burst grows past it when a frame
needs more room; its own turnaround estimate is 1.3 s.
`Data2G::airTiming()` treats a whole keying as one 12 s burst decoded
within 4 s (`AirTiming::forFrameSeconds`), giving an acknowledgement
timeout of about 45 s. These are estimates to tune on a real two-host
bench; with the command port on, PTT and BUSY keep the protocol from
leaning on them in the usual case.

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

## What was built

- `app/src/text_messaging/Data2GLink.{h,cpp}`: KISS framing, AX.25 UI
  frames, the chat payload tag, command-line parsing and the timers. No
  sockets, so it is all unit tested.
- `app/src/text_messaging/Data2GTransport.{h,cpp}`: the
  `ITextMessagingTransport` over TCP, on its own thread, reconnecting
  every 2 s (backing off to 30 s) while data2g-host is not there.
- Preferences, Modem tab, Text Chat: **Send chat through Data2G**, host
  (default `127.0.0.1`), KISS port (8100), and the command port (8300)
  with a switch to leave it alone. Bandwidth (`--kiss-bw`) is set when
  starting data2g-host.
- With Data2G chosen, chat's transport, timers and received frames all
  come from data2g-host: the app does not key the radio for chat, and
  frames its own modem decodes are left out of the conversation (their
  replies would go out on the other modem). Audio can still run for the
  waterfall where PulseAudio lets both programs open the capture device.
- The chat window shows a line naming the Data2G host, whether it is
  reachable, and the submode it last sent in.
- PTT: run rigctld and point both data2g-host and the app at it, so the
  app keeps the frequency display without a serial-port fight.

## Tests

- **Unit** (`fdv_text_messaging_data2g_test`, in CI with the other
  `text_messaging` tests): KISS escaping, AX.25 addresses and UI frames,
  foreign frames ignored, command lines, and two chat protocols
  exchanging a message and its acknowledgement through a fake pair of
  data2g-hosts on localhost (PTT, BUSY and MODE included), plus the
  no-command-port estimate, a keying that never airs, and a lost command
  connection. Clean under ThreadSanitizer.
- **Bench** (manual, on Jeff's Linux box, not yet run): two `data2g-host`
  instances on PulseAudio null sinks, each with a Glissando app attached;
  chat both ways, a broadcast, and a ping. Data2G needs PyTorch, so this
  stays out of CI.

## Next

- Run the two-host bench and tune `Data2G::airTiming()` from measured
  burst lengths.
- Larger frames: a whole 312-byte message fits one Data2G frame.
- Per-station gear shifting needs a Data2G option to shift UI frames
  (see above); to ask Data2G's author about.
