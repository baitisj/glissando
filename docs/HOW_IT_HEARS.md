# How Glissando hears a chirp

Does the Glissando receiver detect chirps or continuous tones? This page
answers that from the C++ modem in [modem/](../modem/) and then walks
through the maths one idea at a time. You don't need a DSP background to
follow it.

> **Short answer: both, because it listens for the whole melody.**
>
> Every symbol glides for its first 40% and holds a note for the other 60%.
> The receiver compares the audio with a stencil of the complete shape,
> glide and hold together, so the glide's energy counts toward every
> decision. To find where a frame starts, it uses the glides the same way
> LoRa uses chirps. To decide which note was sent, it works more like an
> FT8 tone detector, because most of each symbol is a steady note and the
> note carries the data.

![The first four notes of the opening motif: C5 held, then glides C5 to G4, G4 to D5 and D5 to E4, each followed by a held note](images/glide-then-hold.svg)

*The first four notes of the Costas opening motif, drawn to scale on a
log-frequency axis. At Adagio each symbol lasts 640 ms, so each glide takes
256 ms and each hold 384 ms. The shape comes from `pitchTrack()` in
[GlissandoModem.cpp](../modem/GlissandoModem.cpp).*

Line references below are to [GlissandoDemod.cpp](../modem/GlissandoDemod.cpp)
as of September 2026. The function names will stay put longer than the
line numbers. Lesson 6 leaves the frame decoder for the listener that hears
the opening chord, in [GlissandoChord.cpp](../modem/GlissandoChord.cpp).

## Lesson 1: a matched filter is a stencil

Suppose you know exactly what a signal looks like, sample by sample. The
best test for "is it there?" is to multiply the received audio by the
expected waveform, sample by sample, and add up the products. That sum is
called a correlation, and a receiver built around it is a *matched filter*.

Here is why it works. Where the audio matches the stencil, every product
points the same way, so after L samples the signal has grown L times.
Noise points in random directions, so its sum wanders like a drunk walking
home and grows only as the square root of L. One Adagio symbol is 30,720
samples at 48 kHz, so the signal gains on the noise by a factor of about
175 in amplitude. That is the whole secret of weak-signal modes. The
waveform itself holds no magic; what matters is a long, patient comparison
against something you already know.

$$y = \sum_n r[n]\cdot\overline{t[n]} \qquad r = \text{received audio},\; t = \text{template}$$

We work with complex (I/Q) audio, so "multiply by the expected waveform"
means multiply by its complex conjugate. The conjugate spins the template's
phase backwards, so a matching signal ends up standing still, pointing one
way, and adds up. In our code that inner loop is `dotConj()`
([L71](../modem/GlissandoDemod.cpp#L71)).

## Lesson 2: in plain noise, the shape does not matter

This part surprises most people. Against white noise, how well you detect
a known waveform depends on only two things: the energy in the symbol
(power times duration) and how different the candidate symbols are from
one another. A steady tone, a chirp and a bird call of the same energy are
equally easy to detect.

So LoRa's famous sensitivity does not come from chirping. It comes from
very long symbols with a large time-bandwidth product, detected by a
matched filter. FT8 reaches similar depths with plain tones for the same
reason. That is why our simulated thresholds track FT8 and JT65 even though
most of each symbol is a held note: Allegro sits about 0.5 dB behind FT8
and Adagio matches JT65 ([DESIGN.md](DESIGN.md)).

What chirps really buy is protection against things that are not white
noise: a carrier parked on your frequency, echoes you want to separate in
time, and a frequency offset you don't know yet. Lesson 3 is about the last
one.

## Lesson 3: dechirping, the LoRa trick

A matched filter needs the template to line up in both time and frequency.
We don't know when a frame starts or how far off tune the other station is,
so a naive receiver would try every combination, one correlation each.
Dechirping gets all the frequency guesses for the price of one.

Multiply the received glide by the conjugate of the glide you expect. If the
timing is right, the sweep cancels and what is left is a steady tone sitting
at the tuning error. One FFT then measures every possible tuning error at
once, because an FFT is a bank of tone detectors.

![Received glide times the conjugate of the expected glide gives a steady tone at the tuning error](images/dechirp.svg)

*Dechirping a C5 to G4 glide. The sweep cancels and only the offset
survives. The sync search does this for all 21 motif notes of a frame.*

That is exactly our sync search. Each of the 21 known motif notes is
multiplied by the conjugate of its known glide, the result goes through one
FFT, and the powers are added across all 21 (`SyncSearch`,
[L321–L392](../modem/GlissandoDemod.cpp#L321-L392)). The search slides
along in steps of an eighth of a symbol, keeps the best peaks, then
re-searches finely around each one (`syncSearch()`,
[L408–L516](../modem/GlissandoDemod.cpp#L408-L516)). With automatic scale
detection it does this once per scale.

LoRa uses the same trick for its data. Every LoRa symbol is the same chirp
started at a different point in its sweep; dechirp it and the data appears
as a tone whose frequency is the symbol value. We don't do that for data,
because our data symbols are different notes rather than shifted copies of
one chirp.

## Lesson 4: why chirps are fussy about timing

Here is the catch in Lesson 3. A chirp that arrives a little late looks
almost exactly like a chirp that is a little off frequency, because at any
instant its pitch is where the on-time chirp's pitch was a moment earlier.
The sweep couples time and frequency. Radar makes use of this; for us it
means a glide gives a sharp timing fix but can mistake a timing error for a
tuning error.

The receiver handles this by splitting the job (`refineSync()`,
[L560–L630](../modem/GlissandoDemod.cpp#L560-L630)):

- **Time** is refined to the exact sample on the sync glides, where the
  sweep makes the correlation peak narrow.
- **Frequency** is refined on the held part of every note in the frame,
  where a steady tone has no coupling between time and frequency.

The glide and the hold each do the half they are good at. The prototype
showed why sample-accurate timing matters: at Adagio, a timing error of
1.25 ms already cost 1.5 dB.

## Lesson 5: deciding the note with 64 stencils and a trellis

Once the frame is found, each symbol is compared with every possible glide:
from any of 8 notes to any of 8 notes, 64 stencils in all (`correlate()`,
[L641–L671](../modem/GlissandoDemod.cpp#L641-L671)). Each stencil covers the
full symbol, glide plus hold, so it collects all of the symbol's energy. The
code saves work because every stencil ending on the same note shares the
same hold, but mathematically each one is a full-length matched filter.

Two refinements matter:

- **Non-coherent detection.** We never know the absolute phase of the other
  station's audio, so the receiver keeps only the power of each
  correlation, |y|², and turns it into a likelihood with the Bessel
  function formula for a known shape with unknown phase
  ([L788–L794](../modem/GlissandoDemod.cpp#L788-L794)).
- **The trellis.** A glide depends on the note before it, so symbols are not
  independent. The receiver runs the BCJR algorithm over the note sequence,
  which weighs every possible path through the melody at once
  ([L796–L833](../modem/GlissandoDemod.cpp#L796-L833)). This is how the
  glide's 40% of the energy gets used for data: a stencil only matches fully
  when both its start note and its end note are right, and the trellis
  supplies the start note from its neighbour.

About 1 in 8 data symbols repeats the previous note. When it does, the
symbol is a held tone for its whole length and the stencil is a plain tone
detector. The other 7 in 8 carry a glide.

## Lesson 6: hearing the opening chord, two lamps and an "and"

Lessons 1 to 5 decode a frame. Before any of that, the chat needs a much
quicker answer to a much simpler question: is somebody keying right now?
That is carrier sense, and it is what keeps two stations from talking over
each other. Every transmission opens with two notes played together for
0.6 s, E4 (329.63 Hz) and D5 (587.33 Hz), and the chord listener's only job
is to notice that pair ([CHORDS.md](CHORDS.md)). It is sometimes called the
fifths detector, but the interval is a minor seventh: E4 and D5 are the one
pair of notes all four scales share, so a single test covers every scale.

### Eleven lamps and a stencil

Jeff pictured it this way. Lay out a row of lamps, each one lit by how much
sound there is at its pitch. Cut a cardboard stencil with two holes the
chord's distance apart, slide it along the row, and at each position ask
whether light shows through both holes.

That picture is very nearly the code. The lamps are the bins of a Fourier
transform (the same tone-detector bank as in Lesson 3). Every tenth of a
second the listener takes the last 0.6 s of audio, exactly one chord long,
and runs an 8192-point FFT on it at 8 kHz, so each lamp is about 0.98 Hz
wide. The two holes are 257.7 Hz apart, about 264 lamps. Sliding the
stencil is the tuning search: the other station may be off tune by up to
25 Hz either way, so the listener reads the pair at 51 positions one lamp
apart. Like the dechirp in Lesson 3, one FFT serves all 51; sliding the
stencil costs two lookups per position, not another transform.

Each lamp is measured in units of the noise. The listener takes the median
lamp between 200 and 3000 Hz, which a handful of loud signals can't move,
and scales it to the average brightness of a noise-only lamp (for noise, the
median is ln 2 of the mean). After that, a lamp reading 8 is eight times
as bright as noise usually makes it, whatever the audio level.

### Where adding the two holes breaks down

The obvious test adds the light from both holes and compares the total with
a threshold. Jeff spotted the flaw: what if one lamp is blazing and the
other is dark? A carrier on 587 Hz, or one FT8 tone, fills a single hole so
brightly that the sum clears any sensible threshold. Adding is an "or": it
says yes if either note is loud enough.

![Three rows of twelve lamps under a stencil with holes at lamps 4 and 7. A real chord lights both and both tests hear it. One very bright lamp at 7 makes the sum say heard, wrongly, while the dimmer-lamp test stays quiet. Noise alone keeps both quiet.](images/chord-stencil.svg)

*The stencil's holes over lamps 4 and 7 (drawn three lamps apart; on the
air they are 264 apart). The two tests are set to the same false-alarm rate
on noise. Only the dimmer-lamp test rejects the single bright lamp.*

What we want is an "and": E4 is lit *and* D5 is lit. The listener gets one
by judging the pair on its dimmer lamp. At each stencil position it takes
the smaller of the two readings and asks whether that is over 8
(`CHORD_THRESHOLD`). A single blazing lamp can't pass, because the other
hole is still showing only noise.

### What the "and" costs, and why it is cheap

You might expect two separate yes-or-no tests to throw away sensitivity
compared with pooling the light. A little, but not much. On noise, a lamp's
reading follows an exponential law: the chance it reaches 8 is e⁻⁸, about
1 in 3000. Both lamps of one position must do so at once, so a false chord
at that position has chance e⁻⁸ × e⁻⁸ = e⁻¹⁶, about 1 in 9 million. The
summing test gives the same false-alarm rate with a threshold of 19 on the
total. With both tests set to that rate, a simulation of a chord in noise
gives:

| | Sum of both lamps over 19 | Dimmer lamp over 8 |
|---|---|---|
| Chord heard half the time | each note 9.1 dB over a noise lamp | each note 9.8 dB over a noise lamp |
| One lamp at 30 dB, noise in the other hole | heard every time | heard 0.04 % of the time |

So the "and" costs about 0.7 dB on a real chord and turns the blazing-lamp
case from always wrong to almost never. Two things make it cheap. The
transmitter sends both notes at the same level, so on a clean path the
lamps really are equal and nothing is lost by looking at the dimmer one.
And the threshold can be low, 8 rather than something like 16, because two
coincidences have to happen together before noise can fake a chord.

With 51 positions and ten windows a second, the listener makes about 510
of these tests every second. At 1 in 9 million each, noise alone makes a
false chord roughly once every five hours, before the checks below remove
most of those too.

### Four more questions before a chord counts

A busy band isn't white noise. FT8 tones, RTTY, carriers and voices can
light both holes at once by accident, and early versions of the listener
called that a chord ten times a second. So a position that passes the
dimmer-lamp test must also pass four checks (`ChordListener::analyse()`,
[GlissandoChord.cpp L224–L241](../modem/GlissandoChord.cpp#L224-L241)):

- **Nearly even.** The brighter lamp may be at most 10 times the dimmer
  (`MAX_NOTE_RATIO`). A strong carrier with a noise spike that happens to
  reach 8 in the other hole fails here. Ten times leaves room for fading
  to treat the two notes differently.
- **Just switched on.** The window that ended where this one starts, all
  of it from before a chord could have begun, must have shown both lamps at
  under half the dimmer reading now (`ONSET_RATIO`). A station keys from
  silence; a signal already sitting on E4 is not a new chord.
- **A pinpoint, not a lit wall.** Each lamp must be at least 4 times the
  average of the lamps 4 to 10 Hz either side of it (`PURITY`, `purity()`).
  A held note is a sharp line a couple of lamps wide. Voices and the
  wandering tones of FT8 smear their light across the neighbours.
- **Lit the whole time.** Each note must sound in both halves of the
  0.6 s window (`STEADY_SHARE`, `steady()`). Two short signals that each
  touch a hole for part of the window are not a chord.

The position that passes everything with the brightest dimmer lamp wins,
and its tuning becomes the station's tuning for what follows.

### After the chord: a stencil with eight holes

Once a chord is heard, the channel stays busy for as long as the melody
continues. Now the stencil has eight holes, one for each note of a scale, at
the tuning the chord gave. A melody plays one note at a time, so the
listener adds the eight lamps (this time the sum is right, since only one
note sounds at once) and does that for all four scales. The scale that has
been brightest since the chord is the one being sung. Its total must reach
26, about 1.7 times what noise alone makes, and also 1.5 times what that
same stencil usually showed during the 15 s before the chord, so a signal
that was already sitting on those notes doesn't count as melody.

The channel is released 2.5 s after the last bright reading
(`HOLD_SECONDS`), which spans the gaps between notes. On its own a chord
holds the channel for at most 75 s, enough for the first frame of an Adagio
melody to decode. Every frame that does decode starts those 75 s again
(`heardFrame()`), so a long keying is followed to its end, while a chord
the band made up by accident lets go after 75 s.

Measured over 20 minutes of a busy band, with a dozen FT8 signals and a
voice, these checks took the channel from busy all of the time to busy 6%
of the time, never for more than 7.9 s at once ([CHORDS.md](CHORDS.md)).
Below about −14 dB the chord is too faint for the lamps to stand out at
all, and carrier sense falls back on decoded frames.

## What glide-then-hold gains and gives up

| | Pure chirp (LoRa) | Glissando glide + hold |
|---|---|---|
| Sensitivity in white noise | Set by symbol energy | Same rule; thresholds near FT8 and JT65 |
| Finding the frame | Dechirp and FFT | Same trick on the 21 motif glides |
| Carrier or whistle on a note | Spread over the band, barely hurts | Lands on a held note and can steal it |
| Tuning error vs timing | Coupled; needs extra care | Holds measure tuning cleanly |
| Bandwidth | Whole sweep, every symbol | 330 to 880 Hz, fits SSB easily |
| How it sounds | Buzzing sirens | A melody |

The one real thing we give up is resistance to narrowband interference. A
pure chirp spends only a moment at any one frequency, so a carrier at 440 Hz
barely touches it. Our notes rest 60% of the time in a slot about 3 Hz wide
at Adagio, so a carrier sitting on one of our notes will hurt the symbols
that land there. The error-correcting code covers some of that. This is
inferred from the design, not measured; a bench run with a carrier on A4
would put a number on it.

## The prototype does the same

The C++ modem is a port of [prototype/glissando.py](../prototype/glissando.py)
and uses the same maths: dechirped sync search, sample-accurate timing on
glides, frequency on holds, 64 full-symbol stencils and BCJR. The port adds
speed-ups that don't change the result (summing the dechirped audio in
blocks before a shorter FFT, sharing hold correlations) and floors that stop
silence and hum from decoding (see the notes at the top of
[GlissandoDemod.cpp](../modem/GlissandoDemod.cpp)).
