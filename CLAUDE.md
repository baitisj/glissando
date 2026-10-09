# Working on Glissando

Notes for a fresh Claude (or any newcomer) picking this project up. It sums up
what earlier sessions learned and what the owner, Jeff (AG7EW, GitHub
`baitisj`), has decided. Where these notes and the code disagree, the code
wins. Fix the note.

## What this is

Glissando is a weak-signal HF digital mode for keyboard chat that sounds like
a melody. Each symbol glides from one note of an eight-note scale to the
next, then holds. The data is the melody. The signal has a constant envelope
and a continuous phase, and it fits inside an SSB passband. The desktop app
is a 1950s "Bride of Chaotica" control-room console (the Captain Proton
episodes of *Star Trek: Voyager*).

Start with `README.md`. Then read `docs/DESIGN.md` for the reasoning,
`docs/HOW_IT_HEARS.md` for the receiver explained from first principles,
`docs/APP.md` for the app, building and packaging, and `docs/DATA2G.md` for
the Data2G transport.

## Layout

| Path | What | Licence |
|---|---|---|
| `prototype/` | NumPy reference modem, simulator (`sim.py`), PulseAudio bench (`bench.py`), ham text table generator (`ham_table/`), map data generator (`globe/`) | MIT |
| `modem/` | C++17 modem: `GlissandoModem` (modulate and batch receive), `GlissandoReceiver` (streaming), `GlissandoFec`, `GlissandoChord` (opening chord and listener), `GlissandoCw` (CW tail), `GlissandoLink`. No wx or codec2 dependencies | MIT |
| `app/` | The desktop app. It started as a fork of freedv-gui and was never merged back | LGPL-2.1 (`app/COPYING`) |
| `app/src/text_messaging/` | Chat link layer: `FrameCodec` (bit-packed headers), `HamText` (Huffman text table, generated into `HamTextTable.h`), `TextMessagingProtocol` (queue, ACKs, retries, pings, locators), `Data2G*` (the Data2G transport, file transfer) | |
| `app/src/gui/glissando/` | The console: `GlissandoConsole`, `GlissandoScope` (visi-scope waterfall and its art), `MapBall`/`Globe`, `ChaoticaTheme`/`ChaoticaControls` | |
| `app/src/pipeline/TextMessagingModem.cpp` | Where a keying is assembled: opening chord, frames, closing chord or CW tail | |
| `docs/` | Markdown docs. Figures are SVG in `docs/images/` with white backgrounds | |

## Build and test

Linux packages (Ubuntu 24.04) are in `.github/workflows/build.yml`. Also
install `libgtk-3-dev`, or the window-position restore quietly compiles out.

    # Modem and its tests
    cmake -S modem -B build-modem -DUNITTEST=ON && cmake --build build-modem -j$(nproc)
    ctest --test-dir build-modem --output-on-failure

    # App and its tests (what CI runs)
    cmake -S app -B build -DCMAKE_BUILD_TYPE=Release -DUSE_NATIVE_AUDIO=1 -DUNITTEST=ON
    cmake --build build -j$(nproc)
    ctest --test-dir build -R "glissando|text_messaging" --output-on-failure

Run only the `glissando|text_messaging` tests. Some of the inherited
FreeDV tests hang. Unit tests need `-DUNITTEST=ON`, and without it ctest
finds nothing. CI runs both jobs on every PR and on every push to `main`.

Other people build this with newer compilers. Include every standard header
you use, such as `<algorithm>` for `std::count` and `std::min`. The CI
compiler sometimes pulls headers in for free, and a newer one won't.

Two-station bench: `app/test/test_text_chat_loopback.sh up` starts two
stations (TEST1/P and TEST2/P) joined by PulseAudio null sinks, with a dummy
rig. Always run `... down` afterwards. Headless, it needs `pulseaudio`, Xvfb
and `PULSE_SERVER=unix:<path from pactl info>`, or Engage aborts.

## Cloud sandbox quirks

- These hosts are blocked: `media.xiph.org`, `codeload.github.com`, and
  GitHub archive and release downloads. `git clone` works, so point CMake
  `ExternalProject` URLs at local clones for a cloud-only build. Never commit
  that change.
- Tag pushes are refused, and a session can't create GitHub releases. Jeff
  tags and publishes in the GitHub UI.

## Releases

1. A PR bumps `PROJECT_VERSION` in `app/CMakeLists.txt`. It always runs full
   CI: no `[skip ci]` on a release PR, even when earlier PRs skipped it at
   Jeff's request.
2. After Jeff merges it, build from the merge commit:
   - The Linux AppImage: `app/appimage/make-appimage.sh`, which builds a
     Release tree in `app/build-appimage`.
   - The portable Windows zip, cross-compiled with MinGW-w64 as in
     `docs/APP.md`. Strip the exe and DLLs with
     `x86_64-w64-mingw32-strip`, or the zip is 120 MB instead of 10 MB.
3. Write `SHA256SUMS-<version>-beta`, and smoke-test headless (AppImage
   under Xvfb, Windows build under Wine).
4. Draft release notes that say whether the new version can chat with the
   last one.
5. Jeff tags `v<version>-beta` on the merge commit and publishes.

There is no Mac build. The proposed route is an unsigned GitHub Actions job.
There is no ARM build yet either.

## How Jeff works

- He merges PRs himself, sometimes within minutes. Check a PR's live state
  before pushing more to it. A push after a merge strands the work.
- Lead with the answer, in plain words. He likes professor-style
  explanations of the maths (`docs/HOW_IT_HEARS.md` is the model).
- "Don't compile yet": hold builds and CI until he says the batch is done.
  "No CI" means `[skip ci]` commits, but never on a release PR.
- PR descriptions open with a "Before:" paragraph and an "After:"
  paragraph, then "How".
- Don't comment on or push to other contributors' PRs unless Jeff asks.

## Design rules Jeff has set

- **Pretty matters.** Pentatonic is the default because it is harmonious.
  The tritone scales (whole tone, diminished, diabolus) are the villain's
  sound. A sensitivity trade-off for beauty is acceptable. 1950s radio-culture
  names are welcome.
- **Weak-signal first.** It fits SSB bandwidth, is text only (no voice, no
  RADE), and shifts tempo to match the path. Keep the codec2 DATAC13/DATAC4
  chat fallback.
- **Every frame carries the sender's callsign,** so a listener who fades in
  mid-stream knows who it is. No per-QSO session tags in place of callsigns.
  Be wary of context-model (arithmetic) text coding, because a partial copy
  decodes less. Using known zero padding as known bits is welcome.
- **Never send an all-zero frame.** It passes the CRC.
- **The radio is the source of truth for frequency.** The app retunes only
  when the operator picks Presets or Set. Presets sit in the HF data segments
  just below FT8.
- **The console is the only main window.** COMMS and Snooper float, and
  their console buttons light while they are open.
- **Long keyings split** under the 180 s transmit time-out.
- **Data2G** (https://github.com/arodland/Data2G) is an external modem
  reached over TCP:
  - Never pull its code in: no vendoring, submodule, linking or copying.
  - Never start `data2g-host`; the operator runs it.
  - With Data2G off, the chirp modem's behaviour and performance must not
    change at all. Jeff called this "very important". Transport hooks
    default to no-ops for that reason.
- **Compatibility:** say in every release whether it chats with the last one.
  0.4 broke compatibility with 0.3 (Huffman text, packed headers). Over Data2G,
  0.6 broke compatibility with 0.5 (GLISS group).

## Working on Jeff's radio PC

Some work runs on Jeff's own Linux machine, which is connected to his radio
(an IC-7100 through Hamlib). Its rules:

- Never launch Glissando with his configuration, and never click its GUI,
  without his OK. It keys the radio, and the screen is shared.
- The system `rigctld` (localhost:4532, shared with fldigi) is his. Never
  kill or restart it, and send only read-only frequency and mode queries.
  The test bench uses its own dummy rig.
- Tests never use the radio's USB audio codec.
- Build PR branches in a separate worktree, so his own checkout and Release
  build stay untouched.
- Read `RADIO_SETUP.md` in his checkout before starting.

## Open threads

- **VFO jump on launch:** a launch may retune the radio to the last saved
  frequency. It is unconfirmed. The suspects are the Engage path
  (`onRadioConnected_` in `ongui.cpp`) and fldigi.
- **Roadmap:** `docs/DESIGN.md` section 10. The next big item is
  LDPC(174,91) in place of the convolutional code. The app's streaming
  receiver and repeat averaging already exist.
- **Data2G:** sessions and file transfer have been tested only against fake
  hosts. The real check is two `data2g-host`s.
- **Microcontroller transmitters:** see `docs/ESP.md`.
