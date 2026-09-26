"""Render listening samples and a spectrogram.

    python3 render.py            # writes ../samples/*.wav and ../docs/*.png
"""
import os
import wave

import numpy as np

import channel as ch
import glissando as g

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLES = os.path.join(HERE, "..", "samples")
DOCS = os.path.join(HERE, "..", "docs")


def write_wav(path, x, peak=0.7):
    x = np.asarray(x, dtype=float)
    x = x / (np.max(np.abs(x)) + 1e-12) * peak
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(g.FS)
        w.writeframes((x * 32767).astype("<i2").tobytes())


def main():
    rng = np.random.default_rng(73)  # 73 = best regards
    os.makedirs(SAMPLES, exist_ok=True)
    pad = np.zeros(int(0.5 * g.FS))
    clips = {}
    for gi, gear in g.GEARS.items():
        payloads = [rng.integers(0, 2, 77) for _ in range(gear.voices)]
        x = np.concatenate([pad, g.transmit(payloads, gear), pad])
        clips[gi] = x
        name = f"g{gi}-{gear.tempo.lower().replace(' ', '-')}"
        write_wav(os.path.join(SAMPLES, name + ".wav"), x)
        # Only the gear's own decoder can hear it through noise; check that.
        assert g.receive(x, gear)[0][0] is not None
    # What it sounds like on the air: Allegro through a moderate HF path at -10 dB.
    x = ch.hf_channel(clips[3], "moderate", rng)
    y = ch.add_noise(x, -10, rng, 0.5)
    write_wav(os.path.join(SAMPLES, "g3-allegro-moderate-hf-minus10db.wav"), y)
    (p, rep), = g.receive(y, g.GEARS[3])
    print("on-air sample decoded:", p is not None, rep)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    fig, axes = plt.subplots(2, 1, figsize=(10, 6.5), constrained_layout=True)
    for ax, (x, title) in zip(axes, [(clips[3][:int(7 * g.FS)], "G3 Allegro, first 7 s (signature motif, then data)"),
                                     (clips[5][:int(4 * g.FS)], "G5 Presto duet, first 4 s")]):
        x = x + 1e-5 * rng.standard_normal(len(x))  # a noise floor keeps log(0) out of the plot
        ax.specgram(x, NFFT=512, Fs=g.FS, noverlap=448, cmap="magma", vmin=-120)
        ax.set_ylim(200, 2900)
        ax.set_ylabel("Hz")
        ax.set_title(title)
        for f in list(g.VOICE_LOW) + (list(g.VOICE_HIGH) if "duet" in title else []):
            ax.axhline(f, color="w", lw=0.3, alpha=0.35)
    axes[-1].set_xlabel("seconds")
    fig.savefig(os.path.join(DOCS, "spectrogram.png"), dpi=80)


if __name__ == "__main__":
    main()
