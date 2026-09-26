# Glissando

A weak-signal amateur radio digital mode that sounds like someone whistling
a pentatonic tune.

Every symbol is a glide (a chirp) from one note of the A minor pentatonic
scale to the next, followed by a short sustain. The data is the melody. The
signal is constant-envelope, phase-continuous and fits inside an ordinary
SSB passband. Like LoRa, it reaches below the noise by using a large set of
orthogonal chirps, long integration and strong FEC; unlike LoRa, the chirps
are chosen so that any sequence of them, and any two of them at once, is
consonant. It shifts gears (tempo) to match the HF path.

| Gear | Tempo | Symbol | Transmission | 50 % decode, AWGN / CCIR poor (dB, 2500 Hz) |
|------|-------|--------|--------------|-----------------------|
| G1 | Adagio | 640 ms | 55 s | -26.5 / -23.0 |
| G2 | Andante | 320 ms | 27.5 s | -23.6 / -20.8 |
| G3 | Allegro | 160 ms | 13.8 s | -20.4 / -17.6 |
| G4 | Presto | 80 ms | 6.9 s | -17.4 / -14.2 |
| G5 | Presto duet | 80 ms x 2 voices | 6.9 s (2 messages) | -14.1 / -10.8 |

- Design and trade-offs: [docs/DESIGN.md](docs/DESIGN.md)
- Listen: [samples/](samples/) (8 kHz WAV; `g3-allegro-moderate-hf-minus10db.wav`
  is what it sounds like through a fading HF path at -10 dB SNR)
- The devil's interval: `--scale wholetone|diminished|diabolus` swaps the
  pentatonic alphabet for a tritone-built one (`samples/*-wholetone.wav` etc.;
  DESIGN.md 3.1a has the notes and what they cost)
- Prototype: [prototype/](prototype/) (Python 3 + NumPy)

```sh
cd prototype
python3 -m pip install numpy matplotlib
python3 render.py                    # listening samples + spectrogram
python3 sim.py --gears 3 --trials 20 # decode-probability sweep
python3 bench.py --backend pulse --null-sink --gears 4 --snr -16 -14 -12
                                     # same sweep through PulseAudio (Linux):
                                     # a null-sink loopback, or --sink/--source
                                     # for a rig's sound card
python3 -m pytest                    # tests (the PulseAudio test skips without a server)
```

![Spectrogram](docs/spectrogram.png)
