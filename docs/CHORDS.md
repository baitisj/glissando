# The opening and closing chord

Every Glissando keying opens with a chord and closes with one. The idea is
Tyler Carr's (PR #22).

**The opening chord** is E4 and D5 together, a minor seventh, for 0.6 s at
every tempo. Those two notes are in all four scales, so every Glissando
station opens the same way, and listening stations use it to know the
channel is taken (see "The opening chord as carrier sense" below).

**The closing chord** is every note of the scale at once (all sixteen in
the Presto duet), for one bar of four beats at the tempo being sent, so the
ear hears the tempo from it:

| Tempo | Closing chord | One-frame message, with both chords |
|---|---|---|
| Adagio | 2.56 s | 55.0 s + 3.2 s |
| Andante | 1.28 s | 27.5 s + 1.9 s |
| Allegro | 0.64 s | 13.8 s + 1.2 s |
| Presto (and duet) | 0.32 s | 6.9 s + 0.9 s |

The chords go at the ends of a keying, not around each frame. Both are on
by default, and set in Preferences, Options, Modem, under Text Chat: a
switch for the opening chord, and a Tail choice of Off, Chord or CW for the
end. With CW, the station's call is sung in Morse at most once every ten
minutes and the closing chord ends the keyings in between (see
[CW_TAIL.md](CW_TAIL.md)). The chat's timers, the Send button's on-air time
and the split before the 180 s time-out all count the chords and the tail.

## The opening chord as carrier sense

A station knows somebody else is sending only once it decodes a frame of
theirs: about 10 s in at Presto, and about 70 s in at Adagio. Until then it
can key over them. The chord listener (`modem/GlissandoChord.h`) closes
most of that gap. Every tenth of a second it takes the last 0.6 s of audio
and looks for E4 and D5 sounding together, across the whole ±25 Hz tuning
range. When it hears them, the chat treats the channel as busy. It stays
busy for as long as the notes of the scale being sung (at the chord's
tuning) keep sounding, and for 2.5 s after they stop, which bridges the 2 s
pause a long keying takes before the transmit time-out.

Which chord to send was measured, not guessed: 0.6 s chords of each shape
buried in noise, with the threshold set for about one false chord an hour
on noise alone. SNR is a frame's power in 2500 Hz, as for decoding.

| Opening chord | Heard half the time at |
|---|---|
| Every note of the scale (the old chord) | −8 dB |
| Three notes: E4, D5 and a note only that scale has | −11.5 dB |
| A fifth, or a tritone in the tritone scales | −14 dB |
| **E4 and D5 (as built)** | **−14 dB** |

Fewer notes win because a transmitter's power is capped at its peak and
the notes of a chord share it, so each of two notes is much louder than
each of eight. E4 and D5 match the best two-note chord and have two more
things going for them. There is only one chord to look for, so noise gets
fewer chances to fake it (one false chord in four hours of noise). And the
chord doesn't have to say which scale follows: the listener works that out
from the melody.

As built, measured on the chord and a whole frame, with a random tuning
offset (`modem/test/GlissandoChordTest.cpp` checks these):

| SNR | Presto: chord heard | Presto: busy through the frame | Adagio: chord heard | Adagio: busy through the frame |
|---|---|---|---|---|
| −16 dB | 17 % | 11 % | 20 % | 31 % |
| −14 dB | 46 % | 38 % | 45 % | 72 % |
| −12 dB | 93 % | 93 % | 95 % | 99 % |
| −10 dB | 100 % | 100 % | 100 % | 97 % |

The channel is released about 2.5 s after the closing chord ends (the
table was measured with a 1.5 s hold, which changes neither column).

What it can't do:

- **Weak stations.** A frame decodes down to −17.4 dB at Presto and
  −26.5 dB at Adagio. Below about −14 dB the chord goes unheard, and the
  channel shows busy only once a frame decodes, as before.
- **Older builds.** Stations on the 0.1 beta don't open with E4 and D5. A
  strong Presto melody sings both notes within 0.6 s often enough to be
  heard anyway; slower tempos from older builds are only sensed once a
  frame decodes.
- **A carrier on one of the notes** is not a chord: the listener wants both
  notes at similar strength. It could still hold the channel a little
  longer after a real chord, capped like any busy spell.

## Does the chord help the decoder?

No: the decoder ignores both chords. That was measured, not assumed, with
the first chord (every note, at both ends) and the C++ modem on Tyler's branch
(300 frames per point, AWGN, SNR in 2500 Hz measured on the frame alone,
listening for every scale):

| | Without chord | With 1 s chord before and after |
|---|---|---|
| Presto, −18.4 dB | 32 / 300 | 25 / 300 |
| Presto, −17.4 dB | 174 / 300 | 171 / 300 |
| Presto, −16.4 dB | 288 / 300 | 289 / 300 |
| Allegro, −21.4 dB | 36 / 300 | 36 / 300 |
| Allegro, −20.4 dB | 177 / 300 | 184 / 300 |
| Allegro, −19.4 dB | 289 / 300 | 288 / 300 |

The two columns are the same within the noise of the test. The chord also
never fooled the receiver: a chord on its own, clean or at 0 dB, gave no
decode in 500 searches.

Here is why, in three steps.

**1. The receiver never listens for the chord.** It finds a frame by its
21 gliding motif notes (see [HOW_IT_HEARS.md](HOW_IT_HEARS.md)). Energy
that isn't shaped like the motif adds nothing to that search.

**2. If we did listen for it, there would be too little of it.** A
transmitter is limited by its peak power. One note uses the whole peak all
the time. Eight notes at once must share the peak, and even with their
phases chosen to spread the peaks out, the chord's power comes out 7.4 dB
below a frame's. Each note is 16 dB down. At each tempo's decode threshold,
the motif carries 19 dB of signal energy against the noise (Es/N0). A
one-second three-note chord carries 12 dB at Presto, 9 dB at Allegro,
6 dB at Andante and 3 dB at Adagio. A best-case detector, told exactly
when the chord starts, found it 24 % of the time at the Presto threshold,
5 % at Allegro and 0 % at Adagio (with one false alarm per thousand
tries). The all-notes chord is weaker still per note.

**3. A steady chord looks like a carrier.** The glides are there so that
a steady tone on frequency does *not* look like Glissando. Using steady
tones as a signal would give that away.

In Captain Proton terms, the fanfare is for the audience. Chaotica's
radar tracks the rocket, not the brass section.

## What the chord does tell you

- **That the channel is taken**, from the opening chord: see above.
- **The scale**, for your ear, from the closing chord. The receiver
  already finds the scale for free (it hears all four at once).
- **The tempo**, from the closing chord's length. A receiver could only
  measure that at good signal levels, and it searches every tempo anyway.
- **Start and end of a keying**, for the ear and on the scope: the frames
  begin as the opening chord ends, and the closing chord says "over".

## What would make it earn its keep

The chord could only help decoding by carrying the energy of a sync
signal. That means shaping it like one: gliding notes the receiver
already knows, which is exactly what the motif is. A fourth motif, played
as the "chord", would add sync energy but would sound like the frame, not
like a chord. The chord as built is for the ear, which is a fair thing to
spend 9 % on in a mode whose point is to sound good.

## Tyler's fourths and fifths (transposition)

Tyler's branch can also send a frame shifted up to the fourth or fifth
degree of the scale, and the receiver then searches three shifts of every
scale. Measured on the same bench: sensitivity and false decodes were
unchanged (3 vs 4 false decodes in 20,000 noise searches). The search work
roughly doubled: listening to all five tempos and all four scales took
16 % of a core on main and 33 % with the three shifts.
