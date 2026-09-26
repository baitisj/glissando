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

| Gear | Tempo | Symbol | Transmission | Needs (AWGN, 2500 Hz) |
|------|-------|--------|--------------|-----------------------|
| G1 | Adagio | 640 ms | 55 s | see [design](docs/DESIGN.md#measured-performance) |
| G2 | Andante | 320 ms | 27.5 s | |
| G3 | Allegro | 160 ms | 13.8 s | |
| G4 | Presto | 80 ms | 6.9 s | |
| G5 | Presto duet | 80 ms x 2 voices | 6.9 s (2 messages) | |

- Design and trade-offs: [docs/DESIGN.md](docs/DESIGN.md)
- Listen: [samples/](samples/) (8 kHz WAV; `g3-allegro-moderate-hf-minus10db.wav`
  is what it sounds like through a fading HF path at -10 dB SNR)
- Prototype: [prototype/](prototype/) (Python 3 + NumPy)

```sh
cd prototype
python3 -m pip install numpy matplotlib
python3 render.py                    # listening samples + spectrogram
python3 sim.py --gears 3 --trials 20 # decode-probability sweep
```

![Spectrogram](docs/spectrogram.png)
