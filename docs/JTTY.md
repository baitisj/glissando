# Glissando and JTTY

JTTY is Joe Taylor's (K1JT) keyboard mode in WSJT-X 3.2. This note compares
the two modes, records what Glissando borrowed from JTTY's receiver, and
keeps JTTY's other ideas for later. It was written in October 2026, when
JTTY was a release candidate (3.2.0-rc1, 23 September 2026) and still
changing.

Per bit, the two modes are about equally efficient: JTTY's unofficial 50 %
point works out within about 1 dB of Glissando Presto's. JTTY is built for
speed, about 2.6 characters a second in 127 Hz. Glissando trades speed and
width for a melody and for much slower, deeper tempos: Adagio decodes about
10 dB below JTTY. Glissando has borrowed JTTY's three receiver ideas (list
decoding with a grammar check, signal subtraction, partial copy). Nothing
changed on the air.

## What JTTY is

JTTY is meant to feel like RTTY, contests included, but decode like a
weak-signal mode. The facts below come from the WSJT-X user guide, two press
write-ups and two independent implementations that match the WSJT-X encoder
bit for bit. No protocol document was online in October 2026; one
implementer says one ships with WSJT-X 3.2.

- **Waveform:** 4-GFSK, 31.25 baud, tones 31.25 Hz apart, BT 2.0, about
  127 Hz wide.
- **Frame:** 59 symbols in 1.888 s: 13 sync and 46 data.
- **Bits:** 34 information bits (a 32-bit source word, a reserved bit, an
  end-of-message flag), plus CRC-12, so 46 bits.
- **FEC:** rate 1/2 tail-biting convolutional code, constraint length 10
  (512 states), 92 coded bits, Gray-coded onto 46 four-tone symbols.
- **Source words:** five free-text characters from a 64-character alphabet,
  or a typed atom: a callsign action packed as FT8 packs callsigns (CQ, TU,
  AGN?, TU NOW), a contest exchange (serial, zone, state, grid and so on),
  or one of 18 control phrases. A dynamic program picks the fewest frames
  that spell the text exactly. "CQ K1ABC CQ" is one frame and "W9XYZ 599
  107" is two.
- **Transmission:** 1 to 16 frames, no UTC slots, keyed whenever you send.
  Frames one period apart on one frequency are joined, and a lost frame
  prints as `...`.
- **Decoder:** a wrap-around list Viterbi keeps 4 paths per state and hands
  the best 4 to the CRC; each must also parse under the source grammar.
  Decoded frames are re-synthesised and subtracted, and the search runs
  again.
- **Operating:** type and Send, or F1 to F8 macros with %M, %H, %E and %Q;
  N1MM Logger+ drives it. Suggested calling frequency 14.090 MHz.

## Side by side

Glissando's characters per second count a full six-segment text burst of
about 65 characters, header and callsign included.

| | JTTY | Glissando |
|---|---|---|
| Waveform | 4-GFSK, constant envelope | 8-note glide then hold, constant envelope, phase-continuous |
| Width | 127 Hz | 550 Hz (330 to 880 Hz); duet adds 1047 to 2637 Hz |
| Frame | 1.888 s, 34 bits | 6.9 s (Presto) to 55 s (Adagio), 77 bits |
| Sync share of frame | 13 of 59 symbols (22 %) | 21 of 86 symbols (24 %), a Costas motif |
| FEC | K=10 r=1/2 tail-biting convolutional, list Viterbi | K=7 r=1/2 convolutional with tail, BCJR over the note trellis, then list Viterbi (16 paths) when that fails; LDPC(174,91) planned |
| Check | CRC-12 plus grammar check | CRC-14; a runner-up must also agree with the soft bits and pass a grammar check |
| Speeds | one | four tempos plus duet, chosen to fit the path |
| Free text | 5 characters a frame, about 2.6 characters/s | Huffman, about 5 bits a character: about 1.6 characters/s Presto, 3.1 duet, 0.8 Allegro, 0.2 Adagio |
| 50 % decode, AWGN, 2500 Hz | about -16 dB (unofficial, see below) | -17.4 Presto, -20.4 Allegro, -26.5 Adagio, -14.1 duet |
| Timing | no slots, key any time | no slots, streaming receiver |
| Who sent it | not in free-text frames | the sender's callsign in every burst |
| Lost frame | 5 characters shown as `...`, the rest prints | the rest shows with `...` in the gap, and the retry fills it in |
| Link layer | none (operator and macros) | pings, ACKs, retries, reply tempo, carrier sense |
| Subtraction of decoded signals | yes | yes, up to 3 frames deep |
| Sound | four close tones | a tune in a chosen scale |

## Sensitivity per bit

Once you correct for speed, the two modes sit within about 1 dB of each
other and of FT8. A fast mode always needs more SNR than a slow one, so the
SNR figure alone is unfair. The fair yardstick is energy per bit over noise
density, Eb/N0. It is the SNR in 2500 Hz plus a correction for how many bits
a second the mode carries:

    Eb/N0 = SNR(2500 Hz) + 10 log10(2500 Hz / Rb)

Think of it as asking how much energy the transmitter spent on each bit,
whatever the tempo. A lower Eb/N0 at the 50 % point means the coding and
receiver waste less.

| Mode | Bits per frame | Frame | Rate Rb (bit/s) | 50 % SNR (dB) | Eb/N0 (dB) |
|---|---|---|---|---|---|
| FT8 | 77 | 12.64 s | 6.1 | -21 | 5.1 |
| JTTY | 34 | 1.888 s | 18.0 | about -16 | about 5.4 |
| Glissando Allegro | 77 | 13.8 s | 5.6 | -20.4 | 6.1 |
| Glissando Presto | 77 | 6.9 s | 11.2 | -17.4 | 6.1 |

The JTTY row rests on one measurement: an independent iPhone implementation
decoded 20 of 20 frames at -14 dB and 3 of 20 at -17 dB, so the 50 % point
is near -16 dB. That is a third-party decoder on 20 trials, and K1JT's own
may do better. WSJT-X published no threshold as of October 2026.

Two lessons follow. First, Glissando's melody costs about 1 dB per bit
against these 4-tone and 8-tone FSK modes, which matches the finding that
Allegro sits about 0.5 to 1 dB behind FT8. Second, JTTY's short 34-bit frame
does not cost it much, because it uses a long-memory convolutional code with
list decoding, which works well on short blocks where LDPC codes are weak.

## What Glissando borrowed

All three of JTTY's receiver ideas are built, in PR #109 (merged 20 October
2026, after the 0.9.8 release). Only the receiver changed, so a station with
them still chats with 0.9.8 both ways. The numbers are from simulation; none
of it had been tried on the air when this was written. Lessons 10 to 12 of
[HOW_IT_HEARS.md](HOW_IT_HEARS.md) explain how each one works.

| JTTY idea | What Glissando does | Measured in simulation |
|---|---|---|
| List decoding with a grammar check | A frame that never decoded gets a list Viterbi with the 16 best paths into each state (`decodeFrameList()`). A runner-up is taken when its CRC-14 passes, at most 7.5 % of the soft bits' weight disagrees with it (`disagreement()`), and it parses as a chat segment (`couldBeChatSegment()`). | 28 % to 49 % more frames decoded at each tempo's 50 % point; fading gains the most |
| Subtract what you decoded | Each decoded frame is timed to the sample, re-synthesised with a gain per note, and taken out of the audio (`refineFrame()`, `subtractFrame()`). The same stretch is searched again, up to 3 frames deep. | Two Presto stations 3 dB apart: the weaker decodes 50 times in 60, up from 8 |
| Show partial copy | A burst that lost segments shows with `...` in the gaps, once its first two segments (the header) arrived (`FrameCodec::decodePartial()`). A message to you still asks for the fragment, and the retry fills the line in where it stands (`PartialCopies`). | Covered by unit tests |

Every extra path the decoder tries is another 1 in 16,384 chance for noise
to pass the CRC. A plain ceiling of 40 wrong bits would have let in about 5
false decodes an hour, so a runner-up is judged by the weight of the
evidence against it instead. That lets in about one false decode every 2
hours, before the grammar check turns away two in three of those. On 80
minutes of simulated busy traffic the same audio gave 106 right decodes
instead of 98, with the same 3 false decodes as before, and the receiver
took about 4 % more CPU.

## Ideas kept for later

These are proposals, and none is coded. Typed atoms and a tail-biting code
would break compatibility with 0.9.8; macros and a silence gate would not.

1. **Typed atoms for reports and exchanges.** JTTY packs "599 107", grids
   and callsign actions as fields and picks the fewest frames with a dynamic
   program. Glissando's 42 phrases are single symbols, but numbers still
   cost about 5 bits a digit. Adding a report, serial and grid atom, chosen
   per segment so each segment still stands alone, would shorten the common
   short exchange. This breaks compatibility, so it belongs with the next
   phrase-table change.
2. **Tail-biting code.** JTTY's code has no tail. Glissando spends 6 tail
   bits, 12 coded bits or 4 notes a frame, on terminating the code. A
   tail-biting version would save about 5 % of airtime. It breaks
   compatibility, and the LDPC change on the roadmap removes the tail
   anyway, so it is only worth doing if LDPC slips.
3. **Macros and a silence gate.** F1 to F8 with %M and %H would make CQ and
   reply calls one key. One implementer notes JTTY's rolling rescan keeps a
   CPU core busy and proposes skipping silent audio; the same gate would
   help the uConsole. For scale: listening to every tempo and scale,
   Glissando's receiver used about a third of one CPU core on plain noise
   (measured 20 October 2026).

## What not to copy

- **Frames without the sender's callsign.** A JTTY free-text frame carries
  five characters and nothing else, so someone fading in mid-message cannot
  tell who is sending. Glissando's rule that every frame carries the
  callsign stays.
- **One speed and a narrow tone set.** 127 Hz and four close tones are what
  make JTTY fast and dense, but they would turn the melody into a buzz.
  Glissando's tempos are what reach -26 dB, which JTTY does not try to do.
- **No link layer.** JTTY leaves retries to the operator, which suits
  contests. Glissando's ACKs and retries suit chat at the margin.
- **WSJT-X code.** WSJT-X is GPL-3, and the app is LGPL-2.1 with an MIT
  modem, so its code can't be pulled in. PR #109 was written fresh from the
  published descriptions, and any later idea must be too.

## Sources

JTTY was a release candidate when these were read, and its details may
change.

- [WSJT-X User Guide](https://wsjtx.github.io/wsjtx/guide-full.html):
  operation, macros, 30 and 60 WPM throughput
- [morria/jtty](https://github.com/morria/jtty), an independent iOS
  implementation: frame, FEC, decoder and the -14 / -17 dB trials
- [sdroxide PR #597](https://github.com/dividebysandwich/sdroxide/pull/597):
  tones from 1500 Hz, 13-symbol sync, CPU note
- [cq.sk](https://cq.sk/en/jtty-new-digital-mode-combines-rtty-dynamics-with-the-operation-of-weak-signals/):
  4-GFSK, 31.25 baud, K=10, CRC-12
- [DXZone](https://www.dxzone.com/jtty-digital-mode-wsjt-x/): 3.2.0-rc1
  date, 127 Hz, frequencies
- [WSJT-X Improved mailing list](https://sourceforge.net/p/wsjt-x-improved/mailman/message/59403759/):
  still in development, by K1JT
- [marxy's blog](https://blog.marxy.org/2026/10/jtty-new-mode-for-keyboard-short.html):
  on-air report, 714 km at 1 W
