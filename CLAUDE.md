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
and `docs/APP.md` for the app, building and packaging.

## Layout

| Path | What | Licence |
|---|---|---|
| `prototype/` | NumPy reference modem, simulator (`sim.py`), PulseAudio bench (`bench.py`), ham text table generator (`ham_table/`), map data generator (`globe/`) | MIT |
| `modem/` | C++17 modem: `GlissandoModem` (modulate and batch receive), `GlissandoReceiver` (streaming), `GlissandoFec`, `GlissandoChord` (opening chord and listener), `GlissandoCw` (CW tail), `GlissandoLink`. No wx or codec2 dependencies | MIT |
| `app/` | The desktop app. It started as a fork of freedv-gui and was never merged back | LGPL-2.1 (`app/COPYING`) |
| `app/src/text_messaging/` | Chat link layer: `FrameCodec` (bit-packed headers), `HamText` (Huffman text table, generated into `HamTextTable.h`), `TextMessagingProtocol` (queue, ACKs, retries, pings, locators) | |
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
finds nothing. CI runs only the modem job on each PR, and nothing on a
push to `main` (Jeff's choice, 2026-10-22). The full run is started by
hand (Actions > Build and test > Run workflow, or the GitHub
`actions_run_trigger` tool on the branch): the modem and app tests, the
release AppImage and the Windows zip, which it keeps as the run's
artifacts with their SHA-256 in the run summary. Threads
build the app and run its tests themselves before pushing; start a full
run when Jeff asks, and always on a release PR.

Other people build this with newer compilers. Include every standard header
you use, such as `<algorithm>` for `std::count` and `std::min`. The CI
compiler sometimes pulls headers in for free, and a newer one won't.

Two-station bench: `app/test/test_text_chat_loopback.sh up` starts two
stations (TEST1/P and TEST2/P) joined by PulseAudio null sinks, with a dummy
rig. Always run `... down` afterwards. Headless, it needs `pulseaudio`, Xvfb
and `PULSE_SERVER=unix:<path from pactl info>`, or Engage aborts.

## Releases

1. A PR bumps `PROJECT_VERSION` in `app/CMakeLists.txt`. It always gets a
   full CI run, started by hand on its branch: no `[skip ci]` on a release
   PR, even when earlier PRs skipped it at Jeff's request.
2. After Jeff merges it, build from the merge commit, or take both files
   from a full CI run on it (check each against the SHA-256 in its run
   summary):
   - The Linux AppImage: `app/appimage/make-appimage-jammy.sh`, which
     builds in an Ubuntu 22.04 Docker container (it passes a proxy and
     `SSL_CERT_FILE` through). Don't release one built by plain
     `make-appimage.sh` on 24.04: it needs glibc 2.38, and the AppImage
     catalog tests on 22.04.
   - The portable Windows zip: `app/windows/make-zip.sh`, which
     cross-compiles with MinGW-w64, gathers the DLLs, strips everything
     (or the zip is 120 MB instead of 10 MB) and writes the README.
3. Write `SHA256SUMS-<version>`, and smoke-test headless (AppImage
   under Xvfb, Windows build under Wine).
4. Draft release notes that say whether the new version can chat with the
   last one.
5. Jeff tags the merge commit and publishes. From 1.0 on, versions have no
   "beta" tag (`v1.0.0`, `Glissando-1.0.0-x86_64.AppImage`), and a release
   should not be marked pre-release: the AppImage catalog
   (`data/Glissando` in AppImage/appimage.github.io) tests the latest
   release.

There is no Mac build. The proposed route is an unsigned GitHub Actions job.
There is no ARM build yet either.

## The repository

Jeff's rules (2026-11-20):

- No localisms: no hostnames, absolute paths of anyone's machine, private
  links (Claude Docs, artifacts) or notes about one build sandbox. Write
  for someone who has only this repository. Paths in examples are
  relative to the checkout, or plainly made up (`/home/op/...`).
- No development-only documents: no handoffs, roadmaps, work logs or
  dated test reports. Docs describe what Glissando is and does, how to build, test
  and release it, and what goes on the air.
- No one-off test setups: tests and benches are ones anyone can run from
  the checkout (ctest, the two-station bench, `prototype/sim.py`). Setups
  tied to particular hardware or machines stay out.
- Every platform with a build has a release recipe and a job in the full
  CI run that builds it and keeps its files.

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
- **No Data2G here.** Since 2026-10-22, chat and files over
  [Data2G](https://github.com/arodland/Data2G) belong to Ragtime
  (`baitisj/ragtime`), Glissando's sibling. Glissando is the chirp mode only:
  don't add a Data2G path back. Old `/Data2G/...` keys in a config file are
  ignored.
- **Compatibility:** say in every release whether it chats with the last one.
  0.4 broke compatibility with 0.3 (Huffman text, packed headers). 0.6 broke
  Data2G compatibility with 0.5 (GLISS group), and the release after 0.9.8
  drops Data2G altogether.

## Further work

- **Not built yet:** `docs/DESIGN.md` section 10. The next big item is
  LDPC(174,91) in place of the convolutional code. The app's streaming
  receiver and repeat averaging already exist.
- **JTTY:** `docs/JTTY.md` compares the modes. List decoding, subtraction
  and partial copy came from JTTY in PR #109. Typed atoms, a tail-biting
  code, macros and a silence gate are not built yet. WSJT-X is GPL-3, so
  never copy its code.
- **Microcontroller transmitters:** see `docs/ESP.md`.
