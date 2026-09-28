# The Glissando app

A desktop front end for Glissando (`docs/DESIGN.md`): a console with the
visi-scope waterfall, tuning and the mode's options, and a text chat window
whose messages go out as Glissando melodies. It lives in `app/`, and the C++
modem it uses lives in `modem/`, beside the NumPy prototype in `prototype/`
that the modem's tests are checked against.

## Where it came from

`app/` started as a copy of
[baitisj/freedv-gui](https://github.com/baitisj/freedv-gui), branch
`claude/glissando-frontend-xwberv` at commit fa6a931 (itself Jeff's
`text-messaging` fork of FreeDV 2.4.1), so it keeps FreeDV's licence:
`app/COPYING` (LGPL 2.1) covers everything under `app/`. The rest of this
repository, `modem/` included, is MIT (`LICENSE`).

It is the Glissando app, not a FreeDV release. Voice is outside its scope:
it has no voice controls, no FreeDV main window, no FreeDV Reporter and no
FreeDV Help menu, and text chat runs over Glissando. It does not carry RADE
either. The codec2 code is still built in underneath, since the audio
pipeline it came with is organised around it. The shared
[freedv-backend](https://github.com/tmiw/freedv-backend) library is no
longer fetched at configure time: `app/backend/` holds a copy of it with RADE,
RADE text, FARGAN and the Opus bandwidth expander taken out (see
`app/backend/README.md`). Settings a RADE build wrote are harmless: a saved
RADE mode falls back to 700D.

## Building and running (Linux)

Packages on Debian or Ubuntu:

    sudo apt install build-essential cmake git autoconf automake libtool \
        libwxgtk3.2-dev libpulse-dev libspeexdsp-dev libsndfile1-dev \
        libhamlib-dev libasound2-dev libao-dev libgsm1-dev libebur128-dev sox

Then, from the top of the repository:

    cmake -S app -B build -DUSE_NATIVE_AUDIO=1 -DUNITTEST=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    ./build/src/glissando

Without a build type the build is Debug, and an unoptimised receiver cannot
keep up with listening on every tempo at once: decodes fall further and
further behind the air.

The first configure fetches libsamplerate and RNNoise (from GitHub, and the
RNNoise model from media.xiph.org). `ctest --test-dir build -R
"glissando|text_messaging"` runs the modem and chat tests.

## Two floating windows

Both open at launch:

* **The console**, the application's window: the visi-scope waterfall,
  tuning, the mode's modulation options, the radio's dial and the setup
  dialogs, dressed as Chaotica's control room from the Captain Proton
  holonovel. Closing it quits.
* **The chat window** ("Glissando Chat"): heard stations, the transmission
  log and the transmitter, in the same dress. Closing it only hides it;
  `Transmission log` on the console brings it back.
* **The snooping window** ("Glissando Snooper"): every message the station
  hears, including directed messages between other stations, which the chat
  window leaves out. Messages for you are lit and marked FOR YOU. The
  pings, pongs and acknowledgements around them are listed in small print, and
  `Every frame` adds each fragment as it is decoded. An addressee that has
  never been heard transmitting shows as `#` and the CRC of its callsign,
  because frames carry only that. Closing it only hides it; `Snooper` on the
  console brings it back with everything heard meanwhile.

`--glissando` is still accepted, so older scripts keep working, but it no
longer changes anything.

## Controls

| Control | What it does |
| --- | --- |
| Visi-scope | Receive spectrum over the melody's part of the passband, white phosphor on black. The scale's notes are ruled across it with the receiver's +/-25 Hz search band shaded. Double click to put the lowest note where you clicked; mouse wheel nudges the tuning 1 Hz (shift: 0.1 Hz). |
| Decoded frames | Every frame the receiver decodes is written back on the visi-scope where it was heard, and scrolls down with it. Its held notes are lit over the phosphor (the three signature motifs brighter, and marked as bars down the left edge), and a caption beside it reads out what that frame added to the chat frame, which Glissando carries nine bytes at a time: first the header fields as each one becomes whole (MESSAGE, TO YOU, DE K6ABC, the message number, which part of how many), then the characters of the text. A frame that finishes a chat frame says RECEIVED. Frames addressed to other stations are written up too. |
| Scan rate | Waterfall rows per second, 0.5 to 20. |
| Time lens | On by default. The top of the visi-scope works like an elongated convex lens: the newest rows are drawn three pixels tall, and further down the time scale eases into a logarithmic one (`y = K asinh(age / tau)`), so older history moves down ever more slowly and eight times as much of it stays on screen as without the lens. Where several rows share a pixel the brightest wins, so an old, weak melody is squeezed but never averaged away. The rim marked TIME LENS x3 is where one row takes one pixel. The time scale on the left follows the lens. Off: one row per pixel, as before. |
| Duet voice | Widens the scope to show the duet gear's high voice (C6..E7). |
| All tempos | Decode every gear at once, so a station that shifts gear is still heard. Off: only the chosen gear (and the one automatic shifting picked). |
| Melody offset | Moves every note by up to +/-250 Hz, on transmit and receive: the audio equivalent of the tuning dial. |
| Radio dial | Shows the rig frequency, read from the radio once a second and again just before each keying (the radio is not read while it transmits). The radio is the source of truth: turning its dial or retuning it with `rigctl` shows here, and the app never puts it back. Only `Presets` (the frequency list, edited in Preferences, Options) and `Set` (a typed frequency) retune the radio; one picked while disengaged is sent when the radio connects. Starting and stopping leave the radio where it is. The US data segment check follows the shown frequency. |
| Tempo | Adagio (640 ms notes, 55 s frame), Andante, Allegro, Presto (80 ms, 7 s), Duet (two voices, two payloads per frame). |
| Auto shift | Picks the fastest gear the SNR and Doppler spread measured on the last frame heard support (`recommendGear`, the prototype's table with 2 dB margin). The tempo being sent lights up; the hand-picked one glows faintly beside it, and is used until something is heard, and again 15 minutes after the last frame. |
| Scale | Pentatonic (default, harmonious when stations overlap), whole tone, diminished, diabolus (tritones). The scale costs nothing in sensitivity (DESIGN.md 3.1a). It is the scale you send in: the receiver hears every scale and shows the one heard (DESIGN.md 3.1b). |
| Telemetry | SNR meter, Doppler spread, tempo of the last frame and how long ago, tempo we would send at, that tempo's frame length, and the scale the last frame was sung in. |
| Engage | Starts and stops audio. |
| Transmission log | Brings the chat window back if it was closed. |
| Snooper | Brings the snooping window back if it was closed. |
| Preferences | Drops down Options, Sound cards, Rig control (CAT and PTT), Audio filters and Easy setup. Sound cards, rig control and easy setup only change while disengaged. The app opens only the radio's two audio streams, input from the radio and output to it, so Sound cards and Easy setup ask for nothing else; leave the output as none to only listen. |

![The visi-scope at Presto, 10 rows a second, with a message from K6ABC and W1AW's acknowledgement written on it as each frame decoded (a simulated channel, drawn by the scope's own code)](images/visi-scope-captions.png)

![The same with the time lens on after a three minute exchange: the last seconds magnified under the glass at the top, then the history squeezed on a logarithmic scale down to four minutes ago, with the newest captions kept where older ones would overlap (simulated channel)](images/visi-scope-lens.png)

Glissando keeps its own settings and never reads or changes FreeDV's, so the
two can be installed side by side. On Linux the settings are in `~/.glissando.conf`
(or `~/.config/glissando/glissando.conf` with wxWidgets 3.3 and later), the chat
history in `~/.glissando/text_messaging.db` (`~/.local/share/glissando/` with
3.3), and the default reception log in `~/.local/share/glissando/`. On Windows
the settings are under `HKEY_CURRENT_USER\Software\Glissando`. Nothing is
imported from an existing FreeDV setup, so the first start runs Easy setup.
Glissando's own options are in the `[Glissando]` section.

## How chat rides on Glissando

A Glissando frame carries 77 bits. `modem/GlissandoLink.h` cuts each
chat burst (a 14 byte signalling frame or a 54 byte text frame) into 9 byte
segments with a 5 bit header (mode, index, last), dropping trailing zero
padding, so a short message costs fewer frames. The duet gear carries two
segments per frame. The receiver puts segments back together in order; a
missing segment loses the burst and the chat protocol retries it, as it does
a faded codec2 burst.

A Glissando burst takes tens of seconds, so the protocol's timers
(acknowledgement timeout, reservation for fragments still to come, reply
window) are no longer constants: `TextMessaging::AirTiming` holds them, the
codec2 defaults are unchanged, and `AirTiming::forFrameSeconds()` sizes them
to the tempo being sent. Carrier sense only sees a Glissando burst once its
first frame decodes, so the waits for the far end are sized to whole bursts.

Air time for a short message (up to 12 characters of text behind the 15 byte
header, three segments):
about 20 s at Presto, 41 s at Allegro, 2.8 minutes at Adagio. A full 54 byte
text fragment is six frames, about 5.5 minutes at Adagio.

The chat window's send button shows how long the message being typed will be
on the air at the tempo it would go out at now, in red once that is longer
than the transmit time-out (Preferences, Rig control; 180 s when the app's
timer is off, the usual rig setting). Nothing is shown for codec2 or Data2G.

A chat keying runs the same time-out timer as voice. When one would outlast
it, the transport sends it as several keyings, each at least 20 s short of the
limit (clear of the warning the main window gives 15 s before it) and cut only
between whole frames, and lets the radio up for 2 s in
between, which restarts both the app's timer and the rig's. The protocol
still sees one transmission. Every Glissando frame carries its own sync and
the far end rejoins segments by order, so the pauses cost nothing but the
2 s each; the far end's channel-busy hold (one and a half frames) covers
them. If the operator keys for voice during a pause, voice wins: the transport
waits up to 10 s for the radio to come free, then drops the rest of the burst.

## Chat through Data2G

Chat can also go out through [Data2G](https://github.com/arodland/Data2G), a
separate HF data modem program. The app does not start it or include any of
it: run `data2g-host` yourself (with its own sound card and rigctld PTT
settings), then tick **Send chat through Data2G** under Preferences, Modem,
Text Chat, and give its host and ports (KISS 8100, command 8300). The chat
window shows whether data2g-host is reachable. Turn the command port off if
VarAC or Pat also use the same data2g-host. docs/DATA2G.md has the details.

## Modem

`modem/` is a C++17 port of `prototype/glissando.py` and `fec.py`
with no wxWidgets or codec2 dependency: modulator, batch receiver (sync
search by dechirping, sample-accurate time refinement, BCJR over the note
trellis, K=7 r=1/2 Viterbi, CRC-14) and a streaming receiver that searches
each gear every quarter frame on a worker thread. It builds and tests on its
own:

    cmake -S modem -B build-modem -DUNITTEST=ON
    cmake --build build-modem && ctest --test-dir build-modem

The tests check modulation and FEC against vectors generated by the Python
prototype (`modem/test/make_vectors.py`) and decode across gears,
scales and tuning offsets in noise.

## Two station bench

The text chat loopback bench runs Glissando too:

    app/test/test_text_chat_loopback.sh up

Press Engage in both consoles and send from either chat window.

![Two stations on the loopback bench: B (right) hearing A's reply at Presto on the visi-scope, both chat windows below with the conversation both ways](images/glissando-bench.png)

## Packaging

`app/appimage/make-appimage.sh` already carries the console, since it is
part of the `glissando` binary. A Glissando-first AppImage needs a desktop
entry that runs `glissando`, an icon, and a CI job to build it.
