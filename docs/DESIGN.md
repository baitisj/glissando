# Glissando: design

Status: draft 0.1, with a working NumPy prototype in [`prototype/`](../prototype).
Every number in this document comes from that prototype unless it is marked
as an estimate.

## 1. What we are building

A digital mode for HF amateur radio that

1. **sounds pretty**: a listener hears someone whistling a pentatonic tune,
   with glides between the notes, never a buzz or a siren;
2. **fits a standard SSB passband** (300 to 2700 Hz, one voice uses 330 to 880 Hz);
3. **decodes very weak signals**, in the same league as FT8, JT65 and WSPR,
   using the ideas that make LoRa's chirp spread spectrum work;
4. **shifts gears**: it measures the path (SNR and Doppler spread) and moves
   between five tempos, from 55 s messages that decode near -27 dB to 7 s
   duets that carry twice the data.

"Pretty" is an explicit requirement, and it costs something. Section 9 says
exactly what.

## 2. The idea that makes this possible

LoRa is chirp spread spectrum. A LoRa symbol is a linear chirp across the
channel, cyclically shifted by one of M = 2^SF amounts. The receiver
multiplies by a conjugate chirp ("dechirps"), which turns each possible
symbol into a pure tone at a different frequency, then takes an FFT and
picks the biggest bin. Vangelista (2017) calls this *frequency shift chirp
modulation* and shows it is an orthogonal M-ary modulation. Its error rate
in noise is the error rate of non-coherent M-FSK.

That observation is the key to Glissando. **LoRa's sensitivity does not come
from the chirp's shape. It comes from having many orthogonal symbols, each
integrated for a long time, protected by strong FEC.** The same is true of
FT8 (8-FSK), JT65 (65-FSK) and Q65. The chirp shape is a free choice as long
as the symbol set stays near-orthogonal, and what the chirp shape buys you
is how the signal occupies time and frequency: how it sounds, how it resists
narrowband interference, and how it behaves in frequency-selective fading.

So Glissando keeps LoRa's receiver structure (matched filtering against a
set of known chirps, dechirp-and-FFT for synchronisation) and chooses the
chirps for the ear.

## 3. The waveform

### 3.1 Notes

One voice uses eight notes of **A minor pentatonic**, equal temperament,
A4 = 440 Hz:

| index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| note | E4 | G4 | A4 | C5 | D5 | E5 | G5 | A5 |
| Hz | 329.63 | 392.00 | 440.00 | 523.25 | 587.33 | 659.26 | 783.99 | 880.00 |

Why pentatonic: it has no semitones and no tritone, so **every pair of
notes is consonant**. Any data sequence is a pleasant melody, and any two
notes sounding at once are a pleasant interval. That second property
matters on the air more than it sounds: multipath echoes, a second voice
(the duet gear), and another Glissando station in the same key all overlap
the signal in time, and with a pentatonic alphabet they overlap as harmony.

The thirds, fourths and fifths Jeff asked for are all here: E-G and A-C are
minor thirds, C-E is a major third, E-A a fourth, A-E a fifth.

### 3.1a Other scales: the devil's interval

The modem does not care which eight frequencies it glides between, only
that the receiver knows them, so the scale is a parameter (`SCALES` in
`glissando.py`, `--scale` on `sim.py` and `bench.py`). Three tritone-heavy
scales are included for contrast with the pentatonic default. All three are
symmetric under transposition by a tritone, so the duet's high voice is the
low voice a tritone plus an octave up and the two voices are the devil's
interval apart.

| scale | low voice | high voice | tritones |
|---|---|---|---|
| `wholetone` | E4 F#4 G#4 A#4 C5 D5 E5 F#5 | A#5 .. C7 | notes 3 steps apart; the Costas motif has two |
| `diminished` | E4 F4 G4 G#4 A#4 B4 C#5 D5 (half-whole octatonic) | A#5 .. G#6 | every note has its tritone in the scale |
| `diabolus` | E4 G#4 A#4 B4 D5 E5 F5 G#5 (E major + Bb major triads) | A#5 .. D7 | the two triads are a tritone apart |

What they cost: nothing measurable. The receiver separates symbols by
their whole frequency trajectory over a symbol, and at G3 a semitone
(about 20 Hz at E4) is still three times the 6 Hz resolution of a 160 ms
symbol, so even the diminished scale's templates stay nearly orthogonal.
Measured 50 % / 90 % decode thresholds at G3 Allegro, 20 trials per point,
no false decodes in 2080 trials:

| scale | AWGN | CCIR poor |
|---|---|---|
| `pentatonic` | -20.4 / -18.5 | -17.7 / -15.6 |
| `wholetone` | -20.5 / -20.0 | -17.7 / -15.0 |
| `diminished` | -20.5 / -19.3 | -18.2 / -15.8 |
| `diabolus` | -20.2 / -19.1 | -17.6 / -15.3 |

The pentatonic scale stays the default for the reason in 3.1: it is the
one whose overlaps (echoes, the duet, a neighbour in the same key) are
harmony rather than dissonance. The others are there for the ear, and for
anyone who wants a mode that sounds like it is up to something.

### 3.2 A symbol is a glide, then a sustain

Symbol n carries one of the eight notes as its target. It starts on the
previous symbol's note, glides to the target, and sustains it:

- the glide takes the first 40 % of the symbol;
- the glide is linear in log-frequency (constant cents per second, which is
  how a voice or a slide whistle moves) with a raised-cosine ease-in and
  ease-out, so the frequency track has no corners;
- a repeated note is simply held for the whole symbol.

The phase is continuous across symbols and the amplitude never changes. The
whole transmission is one unbroken, constant-envelope melody. Constant
envelope means a transceiver can run it at full rated power with no
linearity worries, like FT8. Phase continuity and eased glides mean no
clicks: 99 % of the power of a solo gear sits between 326 and 884 Hz,
barely wider than the scale itself.

The glide fraction is a real knob. At 0 % the mode is plain 8-FSK tuned to
a scale; at 100 % every symbol is a pure chirp. More glide spreads each
symbol across more spectrum (a little frequency diversity, more "chirp"),
but makes neighbouring glides more alike: the worst-case correlation
between two glides that start on the same note is 0.10 at 640 ms symbols
and 0.21 at 80 ms. 40 % keeps the notes clearly audible as notes, which is
what makes the result sound like a tune rather than a swanee whistle.

### 3.3 The melody is a trellis code, for free

Because each glide starts where the last one ended, the waveform of symbol
n depends on symbols n-1 and n. The receiver correlates every symbol
against all 64 (from, to) glides and runs BCJR (forward-backward) over
an 8-state trellis whose state is the current note. The glide *into* the
next note is extra evidence about the current one, so the melody's
continuity is not a cost to decoding; it is a small amount of memory the
decoder exploits. (This is the same structure as continuous-phase
modulation; Anderson, Aulin and Sundberg 1986.)

### 3.4 The signature motif (sync)

Each transmission contains three copies of a seven-note **signature
motif**: FT8's 7x7 Costas array `3 1 4 0 6 5 2` played on the scale:
C5, G4, D5, E4, G5, E5, A4. A Costas array has an ideal
time-frequency ambiguity function (Costas 1984), which is why FT8 uses it,
and played on a pentatonic scale it happens to be a nice little tune.
Every Glissando transmission begins with it, like a call sign in music.

Frame layout (86 symbols):

```
 motif(7) | data(32) | motif(7) | data(33) | motif(7)
```

The receiver finds the motif by dechirping: it multiplies each expected
sync symbol by the conjugate of its known glide, which collapses a
correctly timed symbol into a tone at the frequency offset, and takes one
zero-padded FFT per symbol. Powers are added non-coherently across the 21
sync symbols. One FFT pass scores every frequency offset at once; the time
search steps T/8, then T/256, and a parabolic fit refines the offset. The
prototype searches +/-1.5 s and +/-25 Hz blind.

One lesson from the prototype: **a glide is a chirp, and chirps compress in
time**, exactly as they do in radar and in LoRa. A glide that sweeps several
hundred Hz has a timing resolution of a few milliseconds, whatever the
symbol length. At G1 a 1.25 ms timing error costs 1.5 dB, and the first
version of the receiver, which timed to T/64, lost about 2 dB at G1
against a receiver told the true timing. The receiver now refines timing to
the sample on the known sync glides, then refines the frequency offset on
the sustained part of every note in the frame (sustains have no
time-frequency coupling), then re-times. After that, blind sync decodes
36 of 40 frames that genie sync decodes at G1, -25 dB (and the receiver
tries the three best sync candidates, which recovers most of the rest).
The upside is that Glissando timing is precise enough to measure
multipath delay; see the roadmap.

### 3.5 FEC and framing

- Payload: 77 bits, the same size and so the same message set as FT8/FT4
  (call signs, grid, report, free text), plus a 14-bit CRC = 91 bits.
- Prototype code: K = 7, rate 1/2 convolutional (0o133/0o171), soft
  Viterbi, 194 bits + 1 pad = 195 bits = 65 notes of 3 bits (Gray-mapped
  along the scale), pseudo-random bit interleaver across the whole frame.
- Target code: FT8's LDPC(174,91). It fits the same 91 information bits
  and would shorten the data section to 58 notes; the gain over the
  convolutional code at this block length is an estimated 1.5 to 2 dB. It
  was left out of the prototype only to keep it small.

## 4. Gears

A gear is a tempo. Doubling the symbol length doubles the energy per
symbol (+3 dB) and halves the data rate, and a listener *hears* the gear:
the same kind of melody, slower or faster.

| Gear | Tempo | Symbol T | Transmission | Slot | Payload | Voices | Band |
|---|---|---|---|---|---|---|---|
| G1 | Adagio | 640 ms | 55.0 s | 60 s | 77 bits | 1 | 330-880 Hz |
| G2 | Andante | 320 ms | 27.5 s | 30 s | 77 bits | 1 | 330-880 Hz |
| G3 | Allegro | 160 ms | 13.8 s | 15 s | 77 bits | 1 | 330-880 Hz |
| G4 | Presto | 80 ms | 6.9 s | 7.5 s | 77 bits | 1 | 330-880 Hz |
| G5 | Presto duet | 80 ms | 6.9 s | 7.5 s | 2 x 77 bits | 2 | 330-880 + 1047-2637 Hz |

G5 adds a second, higher voice on C6 to E7 (the same key, an octave and a
bit above) carrying a second codeword. Each voice gets half the power, and
the sum of two tones is no longer constant-envelope: its peak-to-average
ratio is 3 dB, so at the same peak (PEP) power the duet's average power is
3 dB lower than a solo gear's. With the per-voice split, G5 needs about
6 dB more SNR per decoded message than G4 at equal PEP; it trades that for
twice the data in the same 6.9 s. It is for strong, clean paths and needs a
receive filter that reaches 2.7 kHz. (Thresholds in section 7 are quoted
at equal average power, the WSJT-X convention.)

## 5. Gear shifting

Gear shifting needs two measurements and one rule.

**SNR.** Measured on the 21 known sync symbols: signal energy per symbol
against the noise floor of the wrong-note correlators, converted to the
WSJT-X convention (2500 Hz noise bandwidth). It reads within about 1 dB
from -25 to -10 dB and compresses above that (the estimate is only used
near threshold).

**Doppler spread**, the other half of "path quality". HF fading turns a
long symbol into a smear: once the channel changes within a symbol, a
slower gear stops helping and starts hurting. After a good decode the
receiver knows every note and the waveform's running phase, so each symbol's
correlation is a sample of the channel at that note's pitch. Pairs of
symbols on the same note (same pitch, so multipath frequency selectivity
cancels) at lag d give the channel's time correlation, and a Gaussian
Doppler fit gives the spread. This is decision-directed channel sounding
with no overhead: the melody is the sounding signal. A single frame's
measurement is coarse; see section 7 for how well it tracks the CCIR
profiles. A gear cannot measure Doppler faster than about 1/(2T), so a
slow gear reports "at least this much" and the rule steps up.

**The rule** (`recommend_gear` in the prototype): choose the fastest gear
whose 90 % threshold on a moderate path, plus a 2 dB margin, is below the
measured SNR and whose symbol length times Doppler spread is at most 1.
If none fits, choose the slowest gear the Doppler spread allows. The
simulations (section 7) showed the Doppler limit matters less than
expected: on the CCIR "poor" path (1 Hz spread) the 640 ms gear still
decodes 2 dB deeper than the 320 ms gear, because 58 to 121 Hz note
spacing leaves a 1 Hz smear harmless. The limit is there for flutter and
auroral paths (10 Hz and up), where it pushes the mode to faster tempos.

**The protocol.** Stations exchange the measurement in the report they
already send. A proposed message field: the usual SNR report plus a 3-bit
"gear request" (G1 to G5, with room for more). Calls (CQ) go out in G3 by
default, or G1 when the band is dead. After each received report a station
moves to the requested gear at the start of its next slot; if a gear change
is followed by a missed decode it falls back one gear. Receivers run the
sync search for every gear in parallel on every slot, so no one needs to
be told which gear the other side is in: the tempo of the signature motif
announces it.

## 6. Receiver summary

```
audio -> analytic signal -> sync search (dechirp + FFT, all gears)
      -> for each candidate: 64-glide matched filter bank per symbol
      -> noise and signal level from sync symbols -> log I0 likelihoods
      -> BCJR over the note trellis -> bit LLRs -> deinterleave
      -> soft Viterbi (LDPC in the target design) -> CRC check
      -> on success: decision-directed channel sounding -> gear advice
```

## 7. Measured performance

Blind receive: random start time (0.3 to 1.2 s), random frequency offset
(+/-15 Hz), searched over 1.5 s and +/-25 Hz. 40 trials per point, 7000
trials in all (`prototype/sim.py`, raw data in
`prototype/sim_results.json`). SNR is average signal power over noise in
2500 Hz, the WSJT-X convention. Numbers are the SNR (dB) for 50 % and 90 %
of messages decoded with a correct CRC. Fading paths are the Watterson
two-path model: moderate = 1 ms delay, 0.5 Hz Doppler spread; poor = 2 ms,
1 Hz (ITU-R F.1487 mid-latitude profiles).

| Gear | Length | AWGN 50 % / 90 % | Moderate 50 % / 90 % | Poor 50 % / 90 % |
|---|---|---|---|---|
| G1 Adagio | 55.0 s | **-26.5** / -25.2 | -24.0 / -22.2 | -23.0 / -21.2 |
| G2 Andante | 27.5 s | -23.6 / -22.7 | -21.0 / -19.1 | -20.8 / -18.8 |
| G3 Allegro | 13.8 s | -20.4 / -18.6 | -18.4 / -15.5 | -17.6 / -15.5 |
| G4 Presto | 6.9 s | -17.4 / -16.2 | -15.2 / -13.0 | -14.2 / -12.1 |
| G5 Presto duet | 6.9 s, 2 msgs | -14.1 / -13.1 | -10.5 / -7.8 | -10.8 / -9.3 |

For comparison, the published 50 % sensitivities in the WSJT-X
documentation are about -21 dB for FT8 (12.6 s) and -25 dB for JT65
(about 47 s), and WSPR reaches about -31 dB in 110 s with a much smaller
payload. Glissando's G3 sits about 0.5 dB behind FT8 at similar length,
and G1 matches JT65 territory, with the prototype's convolutional code and
without FT8's a-priori decoding or averaging. Each doubling of tempo costs
2.9 to 3.1 dB in AWGN, as it should.

Fading costs 2 to 4 dB at the 90 % point. The duet loses most on fading
paths because each voice fades independently and both must decode.

**False decodes:** 1 in 7000 (G1, moderate path, -25 dB), a wrong message
that passed the 14-bit CRC after the receiver tried three sync candidates.
FT8 handles this with extra plausibility checks on decoded messages; the
real-time decoder should do the same.

**Path sounding** (median over 40 decodes at high SNR, Doppler spread in Hz):

| Gear | AWGN (0) | Moderate (0.5) | Poor (1.0) |
|---|---|---|---|
| G1 | 0.03 | 0.42 | 0.97 |
| G2 | 0.04 | 0.42 | 0.78 |
| G3 | 0.04 | 0.49 | 1.04 |
| G4 | 0.04 | 0.57 | 1.06 |

The SNR estimate reads within about 1 dB of the truth from threshold up to
about -10 dB and reads low above that. That is fine for gear decisions,
which happen near threshold.

## 8. Multiple stations

A solo gear occupies 550 Hz, so the SSB passband holds two voice ranges,
not the dozens of 50 Hz slots FT8 packs in. Glissando shares a frequency
the way FT8 does, by even and odd time slots, and adds **keys** as
channels. Pentatonic scales a semitone apart share no notes (A minor
pentatonic and B-flat minor pentatonic are disjoint), so they are
orthogonal to the matched filters: a station in B-flat is just noise to a
receiver listening in A. Keys a fifth apart share four notes and collide.
The price is musical: two simultaneous stations a semitone apart clash.
The default is A for everyone and time sharing; keys are for busy bands.

## 9. What "pretty" costs, and what it buys

| Choice | Cost | Buys |
|---|---|---|
| 8-note alphabet (scale-limited) | ~1 dB versus 16-ary, ~2 to 3 dB versus Q65's 65-ary at equal energy (textbook non-coherent M-FSK curves; estimate) | every symbol is a scale note; any sequence is consonant |
| 550 Hz per voice | roughly 10x fewer stations per passband than FT8 | notes spaced 58 to 121 Hz apart shrug off Doppler and drift; frequency diversity against selective fading |
| musical tuning is absolute | a dial error of Hz shifts every note by the same Hz, which bends the intervals (20 Hz is ~100 cents at E4 but ~40 cents at A5). Needs a calibrated rig or an AFC that also re-tunes playback | sounds in tune |
| glides (40 %) | neighbouring glides correlate up to 0.21 at 80 ms | audible glissandi, continuous phase, compact spectrum |
| constant envelope | none versus FT8 | full-power operation |

The honest headline: at a similar transmission length G3 lands about
0.5 dB behind FT8 and G1 matches JT65, while sounding like music. The LDPC
code should more than close that gap (an estimate, not yet measured).

## 10. Roadmap

1. Swap in LDPC(174,91) and a real FT8-compatible 77-bit message packer.
2. Real-time implementation (C or Rust library, sound card I/O, CAT/PTT),
   decoding every gear every slot, several signals per slot.
3. Averaging of repeated transmissions (as Q65 does) for a "Largo" gear
   below G1 without longer symbols, which HF Doppler would punish.
4. Over-the-air tests on 40 m and 20 m against the simulator's predictions.
5. AFC that also corrects the audible pitch, so a mistuned rig still sounds
   in tune.
6. Explore a glide-fraction and scale per gear (e.g. major pentatonic for
   the duet), and a longer-message / file-transfer gear with ARQ.

## References

- L. Vangelista, "Frequency Shift Chirp Modulation: The LoRa Modulation,"
  IEEE Signal Processing Letters, 24(12), 2017.
- M. Chiani and A. Elzanaty, "On the LoRa Modulation for IoT: Waveform
  Properties and Spectral Analysis," IEEE Internet of Things Journal, 2019.
- J. Tapparel et al., "An Open-Source LoRa Physical Layer Prototype on GNU
  Radio," IEEE SPAWC, 2020.
- S. Franke, B. Somerville and J. Taylor, "The FT4 and FT8 Communication
  Protocols," QEX, July/August 2020.
- WSJT-X User Guide, sections on JT65, WSPR and Q65 (Q65 uses Nico
  Palermo's q-ary repeat-accumulate codes).
- J. P. Costas, "A study of a class of detection waveforms having nearly
  ideal range-Doppler ambiguity properties," Proc. IEEE, 72(8), 1984.
- C. C. Watterson, J. R. Juroshek and W. D. Bensema, "Experimental
  confirmation of an HF channel model," IEEE Trans. Communication
  Technology, 18(6), 1970.
- ITU-R Recommendation F.1487, "Testing of HF modems with bandwidths of up
  to about 12 kHz using ionospheric channel simulators."
- J. B. Anderson, T. Aulin and C.-E. Sundberg, *Digital Phase Modulation*,
  Plenum, 1986.
- J. G. Proakis and M. Salehi, *Digital Communications*, 5th ed., chapter
  on non-coherent orthogonal signalling.
