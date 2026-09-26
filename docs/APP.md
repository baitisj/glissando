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

It is the Glissando app, not a FreeDV release, and it does not carry RADE. The FreeDV voice modes left are the codec2 ones (700D, 700E and
1600), always shown in the main window's Mode box, and text chat runs over
either codec2's DATAC13/DATAC4 data modes or Glissando. The shared
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

    cmake -S app -B build -DUSE_NATIVE_AUDIO=1 -DUNITTEST=ON
    cmake --build build -j
    ./build/src/freedv --glissando

The first configure fetches libsamplerate and RNNoise (from GitHub, and the
RNNoise model from media.xiph.org). `ctest --test-dir build -R
"glissando|text_messaging"` runs the modem and chat tests.

## Two floating windows

* **The console** (`Tools -> Glissando Console...`, or start FreeDV with
  `--glissando`). The visi-scope waterfall, tuning, and the mode's
  modulation options, dressed as Chaotica's control room from the Captain
  Proton holonovel.
* **The chat window** (`Transmission log` on the console, or
  `Tools -> Text Chat...`). The existing text chat UI, unchanged, in its own
  window.

While the console is open, text chat goes out as Glissando instead of over
the codec2 DATAC13/DATAC4 modes. Closing the console puts chat back on
codec2. With `--glissando` the console stands in for FreeDV's main window,
which is hidden (the `FreeDV panel` button brings it back for audio and rig
setup), and closing the console quits.

## Controls

| Control | What it does |
| --- | --- |
| Visi-scope | Receive spectrum over the melody's part of the passband, white phosphor on black. The scale's notes are ruled across it with the receiver's +/-25 Hz search band shaded. Double click to put the lowest note where you clicked; mouse wheel nudges the tuning 1 Hz (shift: 0.1 Hz). |
| Scan rate | Waterfall rows per second, 0.5 to 20. |
| Duet voice | Widens the scope to show the duet gear's high voice (C6..E7). |
| All tempos | Decode every gear at once, so a station that shifts gear is still heard. Off: only the chosen gear (and the one automatic shifting picked). |
| Melody offset | Moves every note by up to +/-250 Hz, on transmit and receive: the audio equivalent of the tuning dial. |
| Radio dial | Shows and sets the rig frequency through FreeDV's own frequency box, so rig control and the US data segment check follow it. |
| Tempo | Adagio (640 ms notes, 55 s frame), Andante, Allegro, Presto (80 ms, 7 s), Duet (two voices, two payloads per frame). |
| Auto shift | Picks the fastest gear the SNR and Doppler spread measured on the last frame heard support (`recommendGear`, the prototype's table with 2 dB margin). The hand-picked tempo stays lit and is used until something is heard, and again 15 minutes after the last frame. |
| Scale | Pentatonic (default, harmonious when stations overlap), whole tone, diminished, diabolus (tritones). The scale costs nothing in sensitivity (DESIGN.md 3.1a). |
| Telemetry | SNR meter, Doppler spread, tempo of the last frame and how long ago, tempo we would send at, and that tempo's frame length. |
| Engage | Starts and stops audio, as the main window's Start button does. |

Settings live in the FreeDV config under `[Glissando]`.

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
text fragment is six frames. Watch the transmit time-out timer at slow tempos.

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

    FREEDV_EXTRA_ARGS=--glissando app/test/test_text_chat_loopback.sh up

Press Engage in both consoles, open both Transmission logs and send.

![Two stations on the loopback bench: A (left) hearing B's broadcast at Presto on the visi-scope, both chat windows below](images/glissando-bench.png)

## Packaging

`app/appimage/make-appimage.sh` already carries the console, since it is
part of the `freedv` binary. A Glissando-first AppImage needs a desktop
entry that runs `freedv --glissando`, an icon, and a CI job to build it.
