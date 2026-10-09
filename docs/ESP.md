# Glissando from a microcontroller

Could a small microcontroller, such as an ESP8266, put Glissando on the air by
itself? This note collects what we know about that, for whoever writes the
firmware. Nobody has built it yet.

How sure each part is:

- **From the code:** facts about the signal, taken from `modem/`. These are
  exact.
- **Reasoned:** engineering arguments that have not been measured. They are
  marked *(inferred, untested)*.
- **From memory:** one outside reference, cnlohr's work, was recalled rather
  than looked up. It is marked *(from memory)*.

## The short answer

Yes, for the plain single-voice tempos (Adagio, Andante, Allegro, Presto).
One Glissando voice is a single tone with a constant envelope and a
continuous phase. On the air, after the SSB rig's mixing, it is just a
carrier gliding over a few hundred hertz. Anything that can make a clean
carrier and move its frequency smoothly can make it.

Three things don't fit a one-tone transmitter, because they sound two or
more notes at once:

- **Duet**, which sings two voices together;
- **the opening chord**, E4 and D5 together for 0.6 s;
- **the closing chord**, every note of the scale at once.

The **"CW, straight on E4+D5"** tail is also two notes. The **"CW,
glorified"** tail is one gliding note at a time, so it fits.

Two routes look workable:

1. **A 1-bit RF pin.** This is cnlohr's ESP8266 trick, the one Jeff used for a
   14 MHz CW broadcast. It is the cheapest route, but the spectrum is dirty.
2. **An ESP8266 stepping an Si5351 clock generator.** The Si5351 makes the
   carrier, and the MCU steps its frequency about every millisecond. It is
   cleaner, and it is the way FT8 and WSPR beacons work. This is the
   recommended route.

## What the transmitter has to make

Everything below comes from `modem/GlissandoModem.cpp`,
`modem/GlissandoInternal.h` and `modem/GlissandoChord.h`. The app makes the
same thing at 8 kHz audio and feeds it to an SSB rig on USB, so the RF
frequency is the dial frequency plus the audio frequency. A direct RF
transmitter makes that RF frequency itself.

### Notes

There are eight notes per voice. Each scale is in equal temperament, with
A4 = 440 Hz.

| Scale | Voice 0 (the only voice outside Duet) | Voice 1 (Duet's high voice) |
|---|---|---|
| Pentatonic (default) | 329.63, 392.00, 440.00, 523.25, 587.33, 659.26, 783.99, 880.00 Hz | 1046.50, 1174.66, 1318.51, 1567.98, 1760.00, 2093.00, 2349.32, 2637.02 Hz |
| Whole tone | MIDI 64 66 68 70 72 74 76 78 | MIDI 82 84 86 88 90 92 94 96 |
| Diminished | MIDI 64 65 67 68 70 71 73 74 | MIDI 82 83 85 86 88 89 91 92 |
| Diabolus | MIDI 64 68 70 71 74 76 77 80 | MIDI 82 86 88 89 92 94 95 98 |

A MIDI note *m* is 440 × 2^((*m* − 69)/12) Hz. The receiver's TUNING
offset adds the same number of hertz to every note.

### Tempos

| Gear | Tempo | Symbol length *T* | Frame (86 symbols) |
|---|---|---|---|
| 1 | Adagio | 640 ms | 55.0 s |
| 2 | Andante | 320 ms | 27.5 s |
| 3 | Allegro | 160 ms | 13.8 s |
| 4 | Presto | 80 ms | 6.9 s |
| 5 | Duet | 80 ms, two voices | 6.9 s |

### One symbol: a glide, then a hold

Symbol *k* moves from the previous symbol's note *f_a* to its own note
*f_b*. For time *t* in [0, *T*):

    x    = min(t / (0.4 T), 1)          # the glide takes the first 40 %
    ease = 0.5 - 0.5 cos(pi x)          # raised-cosine ease, no corners
    f(t) = f_a * (f_b / f_a) ^ ease     # linear in log frequency

After 40 % of the symbol, the frequency holds on *f_b*. The phase is the
running integral of *f*, so it never jumps inside a frame. Symbol 0 has no
note before it, so it just holds its own note.

### One frame

A frame has 86 symbols:

- the signature motif, `3 1 4 0 6 5 2` (FT8's 7×7 Costas array), at symbols
  0-6, 39-45 and 79-85;
- 65 data symbols between the motifs, 3 bits each, mapped to notes through
  the Gray code `0 1 3 2 6 7 5 4`.

Each 77-bit payload gets a CRC-14 (FT8's polynomial) and a K=7, rate-1/2
convolutional code. The result is padded to 195 bits and interleaved.
`Glissando::payloadMelody(payload)` returns the 86 note indices for a
payload, so a firmware writer never needs to port the FEC.

Every frame starts from phase 0, and its amplitude fades in and out over
10 ms with a raised cosine. The frames of one keying follow each other with
no gap.

### One keying

1. **Opening chord:** E4 + D5 (329.63 + 587.33 Hz) together for 0.6 s. Every
   scale has both notes. Listeners treat the chord as "channel taken", but
   the decoder doesn't need it.
2. **The frames,** back to back.
3. **The tail:** the closing chord (every note of the scale, one bar = four
   symbols long), or a CW tail, or nothing. The CW tail is "Gliss de
   <MYCALL>" at 20 WPM by default. The glorified tail is one note at a time
   with 5 ms fades. The straight tail keys E4 and D5 together.

The chords and the tail are all optional in the app's Preferences, so a
station that sends frames only is still a valid Glissando station.

### Where the payloads come from

Chat text, callsigns, ACKs and pings are packed into 77-bit payloads by the
app's link layer. That code is `app/src/text_messaging/`: `FrameCodec` for
the header and `HamText` for the Huffman text table. There are two ways to
handle this on a microcontroller:

- **A beacon** can have its frames worked out ahead of time on a PC and
  store only the note sequences: 86 notes of 3 bits each, about 33 bytes per
  frame. There is no tool for this yet. It would be a few lines around
  `FrameCodec` and `payloadMelody()`.
- **A two-way station** needs the receiver too. The decoder costs 0.9 % to
  5.5 % of one 2.1 GHz Xeon core, depending on how many tempos and scales it
  listens for. That is very likely beyond an ESP8266 at 80 or 160 MHz with no
  floating-point unit *(inferred, untested)*.

## Route 1: a 1-bit RF pin

cnlohr made an ESP8266 broadcast NTSC video by streaming a bit pattern out of
its I2S peripheral at about 80 Mbit/s and using the square wave, or its
harmonics, as the RF carrier. Jeff built a 14 MHz CW transmitter on the same
idea. cnlohr has also made LoRa chirps straight from MCU pins (his "lolra"
project) *(from memory)*. A Glissando voice is a slower and gentler chirp
than LoRa's, so the idea carries over. What follows is reasoned, not built:

- **The bit stream has to be computed live.** A fixed carrier can loop one
  short I2S buffer, which is how the CW transmitter works. A glide changes
  frequency all the time, so the bits have to come from something like a
  phase accumulator (an NCO), filled into the DMA buffers as they drain. At
  80 Mbit/s that is 2.5 million 32-bit words a second. A 160 MHz core gets
  only about 64 cycles per word, which is too few to step an NCO bit by bit.
  It would need precomputed words for each frequency step, or a lower bit
  rate that uses a harmonic as the carrier *(inferred, untested)*.
- **The phase is quantised.** Each edge has to land on the 12.5 ns bit clock.
  One cycle at 14 MHz lasts about 71 ns, so one bit is about 60° of phase.
  The error repeats in patterns that move as the frequency glides, which
  makes spurs near the carrier *(inferred, untested)*.
- **A square wave has harmonics.** A 14 MHz square wave has strong odd
  harmonics, at 42 MHz, 70 MHz and so on. The pin needs a serious band-pass
  filter before an antenna to meet spurious-emission limits (in the US,
  §97.307) *(inferred, untested)*.
- **No amplitude control.** A pin is either on or off, so it can't do the
  10 ms fades at the frame edges. Those edges will click unless the PA's bias
  or supply is ramped. Keeping the phase continuous across frame boundaries
  would hide most of it. The receiver finds frames by their motifs, not by
  the fades, so it should not mind *(inferred, untested)*.

## Route 2: ESP8266 plus Si5351 (recommended)

The Si5351 makes a clean square-wave carrier from a fractional PLL and
multisynth divider. The MCU only has to work out the next frequency every
millisecond or so and write a few registers over I2C. That is how FT8 and
WSPR beacons send their tones.

- **Glide smoothness:** the shortest glide is Presto's, 40 % of 80 ms =
  32 ms. With 1 ms steps that is 32 frequency steps per glide. The widest
  pentatonic leap, E4 to A5, is 550 Hz, so the biggest single step is a few
  tens of hertz. The staircase adds small sidebands at multiples of the step
  rate *(inferred, untested)*.
- **I2C budget:** a frequency change is about eight register bytes. At
  400 kHz I2C that is a fraction of a millisecond, so 1 kHz updates fit
  *(inferred, untested)*.
- **Phase:** changing the multisynth's fractional value without resetting the
  PLL moves the frequency without a phase jump. That is what WSPR beacons
  rely on *(inferred from how those beacons work, untested here)*.
- **Two notes at once:** an Si5351 has three outputs. Two of them, summed
  through a combiner, could make the opening chord or Duet's second voice.
  Each output still needs its own filtering, and the sum is no longer
  constant-envelope, so the PA has to be linear *(inferred, untested)*.
- **Harmonics:** the outputs are square waves too, so they still need a
  low-pass filter for the band.

## Frequency accuracy: calibrate it like a beacon

The receiver searches ±25 Hz around the tuned notes
(`MAX_OFFSET_HZ` in `modem/GlissandoReceiver.cpp`). A transmitter more than
about 25 Hz off, counting the receiving station's own error too, won't be
heard unless the listener turns the TUNING knob to compensate.

An ESP8266's 26 MHz crystal, or an Si5351's 25/27 MHz crystal, is typically
tens of ppm off. At 14 MHz, 10 ppm is 140 Hz. So the oscillator has to be
calibrated against a known reference: WWV, a GPS-disciplined source, or a
calibrated receiver. The correction goes into the firmware, as every WSPR
beacon builder does. The crystal also drifts with temperature during long
Adagio frames. The receiver tracks some Doppler spread, but the drift should
stay small compared with that *(inferred, untested)*.

## A sensible first build

1. On a PC, use the modem library to turn a fixed beacon message into note
   sequences: 86 notes per frame, plus the CW ID text.
2. On the ESP8266, drive an Si5351 at (dial + note) Hz with the glide
   formula above, updating every 1 ms. Send Presto or Allegro frames back to
   back, with the chords and tail off or the glorified CW tail on.
3. Calibrate the crystal, filter the output for the band, and check it on a
   second receiver running the Glissando app. The visi-scope and the decoder
   will show whether the melody and the timing are right.
