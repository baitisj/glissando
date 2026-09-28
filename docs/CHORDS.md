# The opening and closing chord

Every Glissando keying opens with a chord and closes with one: all eight
notes of the scale at once (all sixteen in the Presto duet), for one bar of
four beats at the tempo being sent. The idea is Tyler Carr's (PR #22).
Tyler's version played a one-second triad; this one plays every note and
takes its length from the tempo, so the ear hears the tempo from the chord
alone:

| Tempo | Chord | One-frame message, with both chords |
|---|---|---|
| Adagio | 2.56 s | 55.0 s + 5.1 s |
| Andante | 1.28 s | 27.5 s + 2.6 s |
| Allegro | 0.64 s | 13.8 s + 1.3 s |
| Presto (and duet) | 0.32 s | 6.9 s + 0.6 s |

That is 9 % more air time on a one-frame message at every tempo, and less
on longer ones: the chords go at the ends of a keying, not around each
frame. It is on by default. The switch is in Preferences, under text chat:
"Open and close each Glissando transmission with a chord". The chat's
timers, the Send button's on-air time and the split before the 180 s
time-out all count the chords.

## Does the chord help the receiver?

No. That was measured, not assumed, with the C++ modem on Tyler's branch
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

- **The scale**, for your ear. The receiver already finds the scale for
  free (it hears all four at once), so the modem gains nothing here.
- **The tempo**, from the chord's length. A receiver could only measure
  that at good signal levels, and it searches every tempo anyway.
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
