"""Audio prototype: a Glissando reply transposed by pentatonic scale degree.

The original and reply carry the same known FEC frame, but the reply's whole
8-note alphabet is moved upward by a scale-degree offset. This models two
stations taking turns (and also writes an overlap clip to audition the chord).
It does not modify the modem or define an on-air offset negotiation.

    python3 chord_reply.py --degree 2 --out /path/to/output
"""
from __future__ import annotations

import argparse
import os
import wave

import numpy as np

import fec
import glissando as g

PENTATONIC_STEPS = np.array([0, 3, 5, 8, 10])


def degree_shifted_notes(degrees: int) -> np.ndarray:
    """Frequency alphabet shifted by `degrees` steps in the repeating scale."""
    if degrees < 0:
        raise ValueError("degree offset must be nonnegative")
    indices = np.arange(g.N_NOTES) + degrees
    shifted_semitones = (indices // len(PENTATONIC_STEPS)) * 12 + PENTATONIC_STEPS[indices % len(PENTATONIC_STEPS)]
    base_semitones = (np.arange(g.N_NOTES) // len(PENTATONIC_STEPS)) * 12 + PENTATONIC_STEPS[np.arange(g.N_NOTES) % len(PENTATONIC_STEPS)]
    delta = shifted_semitones - base_semitones
    base = g.SCALES["pentatonic"][0]
    return base * 2.0 ** (delta / 12.0)


def render_frame(note_ids: list[int], frequencies: np.ndarray, gear: g.Gear) -> np.ndarray:
    audio = g.modulate_voice(note_ids, frequencies, gear)
    ramp_samples = int(0.01 * g.FS)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(ramp_samples) / ramp_samples)
    audio[:ramp_samples] *= ramp
    audio[-ramp_samples:] *= ramp[::-1]
    return audio


def occupied_band(audio: np.ndarray, sample_rate: int = g.FS, energy_fraction: float = 0.995) -> tuple[float, float]:
    """Central band containing the requested fraction of one-sided FFT energy."""
    spectrum = np.fft.rfft(audio * np.hanning(len(audio)))
    energy = np.abs(spectrum) ** 2
    total = float(np.sum(energy))
    cumulative = np.cumsum(energy) / max(total, 1e-30)
    tail = (1.0 - energy_fraction) / 2.0
    lo = int(np.searchsorted(cumulative, tail))
    hi = int(np.searchsorted(cumulative, 1.0 - tail))
    freqs = np.fft.rfftfreq(len(audio), 1.0 / sample_rate)
    return float(freqs[lo]), float(freqs[min(hi, len(freqs) - 1)])


def sync_score(audio: np.ndarray, degree: int, gear: g.Gear) -> float:
    """Known-time Costas-motif score for a candidate scale-degree offset."""
    voice = g.Voice(degree_shifted_notes(degree), gear)
    z = g.analytic(audio[:g.N_SYMBOLS * gear.L]).reshape(g.N_SYMBOLS, gear.L)
    templates = g._sync_templates(voice)
    sync_samples = z[np.asarray(g.SYNC_POS)]
    correlations = np.einsum("kl,kl->k", sync_samples, np.conj(templates))
    return float(np.sum(np.abs(correlations) ** 2))


def write_wav(path: str, audio: np.ndarray):
    pcm = np.asarray(np.round(np.clip(audio * 0.95, -1.0, 1.0) * 32767), dtype="<i2")
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(g.FS)
        f.writeframes(pcm.tobytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--degree", type=int, default=2, help="pentatonic scale steps to raise the reply (default: 2)")
    parser.add_argument("--gear", type=int, choices=range(1, 6), default=3)
    parser.add_argument("--out", required=True, help="directory for call/reply and overlapped audition WAVs")
    args = parser.parse_args()

    gear = g.GEARS[args.gear]
    rng = np.random.default_rng(0xD0AC)
    payload = rng.integers(0, 2, fec.PAYLOAD_BITS, dtype=np.uint8)
    note_ids = g.frame_notes(fec.encode_frame(payload))
    base_notes = g.SCALES["pentatonic"][0]
    reply_notes = degree_shifted_notes(args.degree)
    call = render_frame(note_ids, base_notes, gear)
    reply = render_frame(note_ids, reply_notes, gear)
    gap = np.zeros(int(0.35 * g.FS))
    call_reply = np.concatenate([call, gap, reply])
    # Equal peak mix for a simultaneous chord audition. This is a listening
    # sample, not a claim that two uncoordinated RF transmitters should key up.
    overlap = 0.5 * (call + reply)

    candidate_degrees = range(5)
    scores = {d: sync_score(reply, d, gear) for d in candidate_degrees}
    detected_degree = max(scores, key=scores.get)
    z = g.analytic(reply)
    reply_voice = g.Voice(reply_notes, gear)
    llr, _ = g.demod_voice(z, gear, reply_voice, g.SyncResult(0, 0.0, 1.0))
    decoded, crc_ok = fec.decode_frame(llr)
    payload_ok = bool(crc_ok and np.array_equal(decoded, payload))

    os.makedirs(args.out, exist_ok=True)
    call_path = os.path.join(args.out, f"glissando-call-reply-degree-{args.degree}-g{args.gear}.wav")
    chord_path = os.path.join(args.out, f"glissando-overlap-degree-{args.degree}-g{args.gear}.wav")
    write_wav(call_path, call_reply)
    write_wav(chord_path, overlap)

    print(f"Scale-degree offset: +{args.degree}")
    print("Base voice: ", ", ".join(f"{f:.1f}" for f in base_notes), "Hz")
    print("Reply voice:", ", ".join(f"{f:.1f}" for f in reply_notes), "Hz")
    print(f"Fundamental pitch range: {min(base_notes):.1f}–{max(reply_notes):.1f} Hz")
    print(f"Known-time sync candidate scores: {scores}; selected degree +{detected_degree}")
    print(f"Reply decoded with selected offset template: {payload_ok}")
    for name, audio in (("call/reply", call_reply), ("overlap", overlap)):
        lo, hi = occupied_band(audio)
        print(f"{name} 99.5% FFT-energy band: {lo:.1f}–{hi:.1f} Hz; below 4 kHz: {hi < 4000}")
    print(f"Wrote {call_path}")
    print(f"Wrote {chord_path}")


if __name__ == "__main__":
    main()
