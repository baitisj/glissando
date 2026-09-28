"""Small multi-party degree-offset experiment for Glissando.

Builds four independent coded frames. Each simulated participant keeps the
same pentatonic note alphabet, shifted by a different number of scale degrees.
It writes a turn-taking chat and an aligned four-way pileup, then tries
known-time sync detection and payload decoding for each offset.

    python3 multi_party.py --out /path/to/output
"""
from __future__ import annotations

import argparse
import os
import wave

import numpy as np

import chord_reply as cr
import fec
import glissando as g


def _ramp(audio: np.ndarray) -> np.ndarray:
    x = audio.copy()
    n = int(0.01 * g.FS)
    w = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
    x[:n] *= w
    x[-n:] *= w[::-1]
    return x


def _signal(payload: np.ndarray, degree: int, gear: g.Gear, phase: float) -> np.ndarray:
    notes = g.frame_notes(fec.encode_frame(payload))
    audio = _ramp(g.modulate_voice(notes, cr.degree_shifted_notes(degree), gear))
    if phase:
        audio = np.real(g.analytic(audio) * np.exp(1j * phase))
    return audio


def _sync_scores(audio: np.ndarray, degrees: list[int], gear: g.Gear) -> dict[int, float]:
    return {degree: cr.sync_score(audio, degree, gear) for degree in degrees}


def _decode(audio: np.ndarray, degree: int, gear: g.Gear, expected: np.ndarray) -> bool:
    z = g.analytic(audio)
    voice = g.Voice(cr.degree_shifted_notes(degree), gear)
    llr, _ = g.demod_voice(z, gear, voice, g.SyncResult(0, 0.0, 1.0))
    payload, crc_ok = fec.decode_frame(llr)
    return bool(crc_ok and np.array_equal(payload, expected))


def _write_wav(path: str, audio: np.ndarray, amplitude: float = 0.95):
    pcm = np.asarray(np.round(np.clip(audio * amplitude, -1.0, 1.0) * 32767), dtype="<i2")
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(g.FS)
        f.writeframes(pcm.tobytes())


def _active_band(audio: np.ndarray) -> tuple[float, float]:
    return cr.occupied_band(audio, energy_fraction=0.995)


def _payload_for_text(message: str) -> np.ndarray:
    raw = message.encode("ascii")[:9].ljust(9, b" ")
    payload = np.zeros(fec.PAYLOAD_BITS, dtype=np.uint8)
    payload[:72] = np.unpackbits(np.frombuffer(raw, dtype=np.uint8))
    return payload


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True)
    ap.add_argument("--gear", type=int, choices=range(1, 6), default=4)
    ap.add_argument("--degrees", type=int, nargs="+", default=[0, 1, 2, 3],
                    help="one scale-degree offset per participant")
    args = ap.parse_args()
    if len(args.degrees) not in (3, 4) or len(set(args.degrees)) != len(args.degrees):
        ap.error("provide three or four distinct degree offsets")

    gear = g.GEARS[args.gear]
    rng = np.random.default_rng(0xD0AC + len(args.degrees))
    messages = ["CQ CHAT A", "HEARD YOU", "GOOD DX C", "73 TO ALL"][:len(args.degrees)]
    payloads = [_payload_for_text(message) for message in messages]
    signals = [_signal(payload, degree, gear, float(rng.uniform(0, 2 * np.pi)))
               for payload, degree in zip(payloads, args.degrees)]
    silence = np.zeros(int(0.25 * g.FS))
    turn_tape = np.concatenate([part for i, signal in enumerate(signals)
                                for part in ((signal, silence) if i + 1 < len(signals) else (signal,))])
    pileup = np.sum(signals, axis=0)

    os.makedirs(args.out, exist_ok=True)
    count = len(args.degrees)
    turn_path = os.path.join(args.out, f"glissando-{count}-party-turns-g{args.gear}.wav")
    pile_path = os.path.join(args.out, f"glissando-{count}-party-pileup-g{args.gear}.wav")
    _write_wav(turn_path, turn_tape)
    # Normalize only the listening artifact. The decoder below sees the actual
    # sum of the participant waveforms, with each transmitter at full level.
    _write_wav(pile_path, pileup, amplitude=0.95 / count)

    print(f"{count} participants, G{args.gear}, {gear.T:.3f}s symbols")
    for i, (degree, signal, payload, message) in enumerate(zip(args.degrees, signals, payloads, messages), 1):
        band = _active_band(signal)
        scores = _sync_scores(signal, args.degrees, gear)
        detected = max(scores, key=scores.get)
        print(f"  participant {i} ({message}): degree +{degree}, {band[0]:.0f}–{band[1]:.0f} Hz, "
              f"sync selects +{detected}, decode={_decode(signal, detected, gear, payload)}")

    pile_scores = _sync_scores(pileup, args.degrees, gear)
    print("  aligned pileup sync scores:", {f"+{k}": round(v, 1) for k, v in pile_scores.items()})
    for degree, payload in zip(args.degrees, payloads):
        print(f"  pileup decode at +{degree}: {_decode(pileup, degree, gear, payload)}")
    print(f"  occupied band, turns: {_active_band(turn_tape)[0]:.0f}–{_active_band(turn_tape)[1]:.0f} Hz")
    print(f"  occupied band, pileup: {_active_band(pileup)[0]:.0f}–{_active_band(pileup)[1]:.0f} Hz")
    print(f"Wrote {turn_path}")
    print(f"Wrote {pile_path}")


if __name__ == "__main__":
    main()
