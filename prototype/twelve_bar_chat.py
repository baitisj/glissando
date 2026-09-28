"""Four-party, 12-bar blues pitch-map audition for Glissando.

Each party has an independent 77-bit FEC payload and a different role in the
current dominant chord. The note alphabet changes at each bar according to a
quick-change A blues progression. The 86 frame symbols are placed into 12 bars
of four-quarter-note length at 100 BPM; 7/8-symbol bars use 343/300 ms symbols
to land their chord change exactly on the barline.

This is a musical/waveform prototype, not a current-modem-compatible frame:
the receiver would need the same progression, bar timing, and chord-role map.

    python3 twelve_bar_chat.py --out /path/to/output
"""
from __future__ import annotations

import argparse
import os
import wave

import numpy as np

import fec
import glissando as g
import chord_reply as cr

SAMPLE_RATE = g.FS
BAR_SECONDS = 2.4  # four beats at 100 BPM
CHORD_STEPS = np.array([0, 2, 4, 7, 10])  # dominant pentatonic: 1, 2, 3, 5, b7
CHORDS = ["A7", "D7", "A7", "A7", "D7", "D7", "A7", "A7", "E7", "D7", "A7", "E7"]
CHORD_ROOT_MIDI = {"A7": 57, "D7": 62, "E7": 64}  # A3, D4, E4
BAR_SYMBOLS = [7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 7, 8]  # sum is the existing 86-symbol frame
PARTICIPANTS = [
    ("A", "CQ CHAT A", 0, "root"),
    ("B", "HEARD YOU", 2, "third"),
    ("C", "GOOD DX C", 3, "fifth"),
    ("D", "73 TO ALL", 4, "flat seventh"),
]


def _payload(text: str) -> np.ndarray:
    raw = text.encode("ascii")[:9].ljust(9, b" ")
    bits = np.zeros(fec.PAYLOAD_BITS, dtype=np.uint8)
    bits[:72] = np.unpackbits(np.frombuffer(raw, dtype=np.uint8))
    return bits


def _midi_hz(midi: np.ndarray) -> np.ndarray:
    return 440.0 * 2.0 ** ((np.asarray(midi, dtype=float) - 69.0) / 12.0)


def _bar_alphabet(chord: str, party_degree: int) -> np.ndarray:
    indices = np.arange(g.N_NOTES) + party_degree
    semitones = (indices // len(CHORD_STEPS)) * 12 + CHORD_STEPS[indices % len(CHORD_STEPS)]
    return _midi_hz(CHORD_ROOT_MIDI[chord] + semitones)


def _render_chorus(payload: np.ndarray, party_degree: int, gear: g.Gear, phase0: float) -> np.ndarray:
    note_ids = g.frame_notes(fec.encode_frame(payload))
    chunks = []
    symbol_index = 0
    phase = phase0
    previous_frequency = None
    for chord, symbols_in_bar in zip(CHORDS, BAR_SYMBOLS):
        note_seconds = BAR_SECONDS / symbols_in_bar
        length = int(round(note_seconds * SAMPLE_RATE))
        frequencies = _bar_alphabet(chord, party_degree)
        for _ in range(symbols_in_bar):
            target = note_ids[symbol_index]
            target_frequency = frequencies[target]
            start_frequency = target_frequency if previous_frequency is None else previous_frequency
            track = g._pitch_track(start_frequency, target_frequency, length, gear.glide)
            phase_track = phase + 2 * np.pi * np.cumsum(track) / SAMPLE_RATE
            chunks.append(np.sin(np.concatenate([[phase], phase_track[:-1]])))
            phase = float(phase_track[-1] % (2 * np.pi))
            previous_frequency = target_frequency
            symbol_index += 1
    if symbol_index != len(note_ids):
        raise RuntimeError("12-bar symbol schedule does not fit one frame")
    audio = np.concatenate(chunks)
    ramp_n = int(0.01 * SAMPLE_RATE)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(ramp_n) / ramp_n)
    audio[:ramp_n] *= ramp
    audio[-ramp_n:] *= ramp[::-1]
    return audio


def _write_wav(path: str, audio: np.ndarray, gain: float):
    pcm = np.asarray(np.round(np.clip(audio * gain, -1.0, 1.0) * 32767), dtype="<i2")
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(SAMPLE_RATE)
        f.writeframes(pcm.tobytes())


def _band(audio: np.ndarray) -> tuple[float, float]:
    return cr.occupied_band(audio, energy_fraction=0.995)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True)
    parser.add_argument("--gear", type=int, choices=range(1, 6), default=2,
                        help="sets glide shape; bar tempo stays 100 BPM")
    args = parser.parse_args()
    gear = g.GEARS[args.gear]
    rng = np.random.default_rng(0x12B1)
    messages = [_payload(text) for _, text, _, _ in PARTICIPANTS]
    voices = [_render_chorus(payload, role, gear, float(rng.uniform(0, 2 * np.pi)))
              for payload, (_, _, role, _) in zip(messages, PARTICIPANTS)]
    gap = np.zeros(int(0.35 * SAMPLE_RATE))
    turns = np.concatenate([item for i, voice in enumerate(voices)
                            for item in ((voice, gap) if i + 1 < len(voices) else (voice,))])
    ensemble = np.sum(voices, axis=0)

    os.makedirs(args.out, exist_ok=True)
    turns_path = os.path.join(args.out, "glissando-12bar-4party-turns.wav")
    ensemble_path = os.path.join(args.out, "glissando-12bar-4party-overlap.wav")
    _write_wav(turns_path, turns, 0.95)
    _write_wav(ensemble_path, ensemble, 0.95 / len(voices))

    print("A blues quick-change / turnaround:", " | ".join(CHORDS))
    print("Bars 2, 5-6, and 10 use IV; bar 9 and turnaround bar 12 use V.")
    print(f"12 bars x {BAR_SECONDS:.1f}s = {12 * BAR_SECONDS:.1f}s per participant at 100 BPM")
    print("Per-bar symbol counts:", BAR_SYMBOLS, f"({min(BAR_SECONDS / n for n in BAR_SYMBOLS):.3f}–{max(BAR_SECONDS / n for n in BAR_SYMBOLS):.3f}s each)")
    for (name, message, role, role_name), voice in zip(PARTICIPANTS, voices):
        lo, hi = _band(voice)
        print(f"{name}: {message}, chord role {role_name} (+{role} scale degrees), 99.5% band {lo:.0f}–{hi:.0f} Hz")
    for label, audio in (("turns", turns), ("overlap", ensemble)):
        lo, hi = _band(audio)
        print(f"{label} 99.5% band: {lo:.0f}–{hi:.0f} Hz; inside 4 kHz: {hi < 4000}")
    print(f"Wrote {turns_path}")
    print(f"Wrote {ensemble_path}")


if __name__ == "__main__":
    main()
