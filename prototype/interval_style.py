"""Experimental blues-leaning interval mapper for Glissando.

This is deliberately separate from glissando.py: it changes the data-symbol
alphabet from 3 bits per note to a finite-state, rate-2.25-bit-per-note
enumerative mapper (9 bits -> 4 note transitions), so it is not wire compatible
with the current modem. It keeps the existing pentatonic pitches and frame
sync, and uses iterative soft-output trellises for the interval mapper and
existing convolutional FEC as a small AWGN proof of concept.

Run from this directory:
    python3 interval_style.py --trials 20 --snr -20 --rhythm shuffle
    python3 interval_style.py --write-wav /tmp/glissando-interval-style --rhythm shuffle
"""
from __future__ import annotations

import argparse
from functools import lru_cache
import itertools
import math
import os
import wave

import numpy as np

import channel as ch
import fec
import glissando as g

N_CHUNK_BITS = 9
N_CHUNK_NOTES = 4
N_PATHS = 1 << N_CHUNK_BITS
PADDED_BITS = math.ceil(fec.FRAME_BITS / N_CHUNK_BITS) * N_CHUNK_BITS
CHUNKS = PADDED_BITS // N_CHUNK_BITS
DATA_NOTES = CHUNKS * N_CHUNK_NOTES
BLOCK_CHUNKS = CHUNKS // 2
BLOCK_NOTES = BLOCK_CHUNKS * N_CHUNK_NOTES
STYLE_DATA_BLOCKS = (BLOCK_NOTES, BLOCK_NOTES)
STYLE_LAYOUT = ["S"] * 7 + ["D"] * BLOCK_NOTES + ["S"] * 7 + ["D"] * BLOCK_NOTES + ["S"] * 7
STYLE_SYMBOLS = len(STYLE_LAYOUT)
STYLE_SYNC_POS = [i for i, c in enumerate(STYLE_LAYOUT) if c == "S"]
RHYTHMS = {
    "straight": (1, 1),
    # Traditional triplet shuffle: two thirds of the beat, then one third.
    # The pair averages to two ordinary symbol periods.
    "shuffle": (4, 2),
}

_NOTE_SEMITONES = np.rint(12 * np.log2(g.SCALES["pentatonic"][0] / g.SCALES["pentatonic"][0][0])).astype(int)


def _transition_cost(a: int, b: int) -> float:
    """Favor blues-pentatonic interval colors and compact melodic movement."""
    d = int(_NOTE_SEMITONES[b] - _NOTE_SEMITONES[a])
    pc = abs(d) % 12
    # A minor-pentatonic palette: root, minor third, fourth, fifth, minor seventh.
    # The existing pitch set has no semitone or tritone pair, but this rewards
    # the more characteristic blues colors over whole-tone and sixth motions.
    color = {0: -1.0, 3: -0.72, 5: -0.58, 7: -0.58, 10: -0.34,
             2: 0.20, 9: 0.22, 4: 0.42, 8: 0.42, 1: 1.0, 6: 1.0, 11: 1.0}.get(pc, 0.8)
    return color + 0.055 * abs(d)


def _path_cost(path: tuple[int, ...], start: int) -> float:
    prev = start
    cost = 0.0
    directions = []
    for note in path:
        d = int(_NOTE_SEMITONES[note] - _NOTE_SEMITONES[prev])
        cost += _transition_cost(prev, note)
        directions.append(int(np.sign(d)))
        if abs(d) > 7:
            cost += 0.12 * (abs(d) - 7)
        prev = note
    # Short phrases that keep climbing/falling get a mild penalty. This favors
    # a turn or resolution inside each four-note code group without forcing it.
    for i in range(2, len(directions)):
        if directions[i] and directions[i - 1] and directions[i - 2] == directions[i - 1] == directions[i]:
            cost += 0.24
    return cost


def _make_codebook():
    """For each starting note, rank 512 favored paths among 5^4 candidates."""
    codebook, reverse = [], []
    moves = []
    for a in range(g.N_NOTES):
        order = sorted(range(g.N_NOTES), key=lambda b: (_transition_cost(a, b), abs(int(_NOTE_SEMITONES[b] - _NOTE_SEMITONES[a])), b))
        moves.append(order[:5])
    for start in range(g.N_NOTES):
        paths = []
        # Generate paths with state-dependent choices.
        def walk(prev, path):
            if len(path) == N_CHUNK_NOTES:
                paths.append(tuple(path))
                return
            for nxt in moves[prev]:
                walk(nxt, path + [nxt])
        walk(start, [])
        paths.sort(key=lambda p: (_path_cost(p, start), p))
        chosen = paths[:N_PATHS]
        if len(chosen) != N_PATHS or len(set(chosen)) != N_PATHS:
            raise RuntimeError("constrained mapper codebook is not one-to-one")
        codebook.append(chosen)
        reverse.append({path: i for i, path in enumerate(chosen)})
    return moves, codebook, reverse


MOVES, CODEBOOK, REVERSE = _make_codebook()


def encode_bits(bits: np.ndarray, start_note: int = g.COSTAS7[6]) -> tuple[list[int], int]:
    """Map 198 padded coded bits to 88 targets, in two independently reset blocks."""
    padded = np.zeros(PADDED_BITS, dtype=np.uint8)
    padded[:len(bits)] = bits
    out: list[int] = []
    state = start_note
    for i in range(CHUNKS):
        value = 0
        for bit in padded[i * N_CHUNK_BITS:(i + 1) * N_CHUNK_BITS]:
            value = (value << 1) | int(bit)
        path = CODEBOOK[state][value]
        out.extend(path)
        state = path[-1]
        if i + 1 == BLOCK_CHUNKS:
            state = start_note
    return out, int(padded[len(bits):].size)


def decode_hard(notes: list[int], start_note: int = g.COSTAS7[6]) -> np.ndarray:
    """Noiseless inverse, useful for checking codebook/rank construction."""
    bits: list[int] = []
    state = start_note
    for i in range(CHUNKS):
        path = tuple(notes[i * 4:(i + 1) * 4])
        value = REVERSE[state][path]
        bits.extend((value >> shift) & 1 for shift in range(8, -1, -1))
        state = path[-1]
        if i + 1 == BLOCK_CHUNKS:
            state = start_note
    return np.asarray(bits[:fec.FRAME_BITS], dtype=np.uint8)


def frame_notes(coded: np.ndarray) -> list[int]:
    data, _ = encode_bits(coded)
    data_iter = iter(data)
    motif = g.COSTAS7 * 3
    sync_iter = iter(motif)
    return [next(sync_iter) if c == "S" else next(data_iter) for c in STYLE_LAYOUT]


def symbol_lengths(gear: g.Gear, rhythm: str, count: int = STYLE_SYMBOLS) -> list[int]:
    """Sample count for each symbol; shuffle pairs preserve average frame rate."""
    if rhythm not in RHYTHMS:
        raise ValueError(f"unknown rhythm {rhythm!r}")
    numerators = RHYTHMS[rhythm]
    if rhythm == "straight":
        return [gear.L] * count
    long = round(gear.L * numerators[0] / sum(numerators))
    short = 2 * gear.L - long
    return [long if i % 2 == 0 else short for i in range(count)]


def _llr0(log_metrics: np.ndarray) -> np.ndarray:
    return g._logi0(log_metrics)


def _lse(values: np.ndarray, axis: int) -> np.ndarray:
    # Unlike the compact helper in the base prototype, this safely preserves
    # unreachable trellis states whose candidate metrics are all -infinity.
    return np.logaddexp.reduce(values, axis=axis)


def _inner_soft_decode(llm: list[np.ndarray], bit_priors: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray]:
    """BCJR over the interval mapper; return posterior and extrinsic bit LLRs."""
    output = []
    priors = np.zeros(PADDED_BITS) if bit_priors is None else np.asarray(bit_priors, dtype=float)
    data_start = 7
    for block_no, nnotes in enumerate(STYLE_DATA_BLOCKS):
        n_chunks = nnotes // N_CHUNK_NOTES
        block = llm[data_start:data_start + nnotes]
        terminal = llm[data_start + nnotes][:, g.COSTAS7[0]]
        # Precompute each block's 512 input branches from every entering note.
        branch_metric = np.zeros((n_chunks, g.N_NOTES, N_PATHS))
        branch_end = np.zeros((g.N_NOTES, N_PATHS), dtype=np.int8)
        for a in range(g.N_NOTES):
            for u, path in enumerate(CODEBOOK[a]):
                branch_end[a, u] = path[-1]
                for c in range(n_chunks):
                    # Add the four transition likelihoods for this chunk.
                    for t, nxt in enumerate(path):
                        if t == 0:
                            src = a
                        else:
                            src = path[t - 1]
                        branch_metric[c, a, u] += block[c * 4 + t][src, nxt]
                    chunk = block_no * BLOCK_CHUNKS + c
                    input_bits = np.asarray([(u >> shift) & 1 for shift in range(8, -1, -1)])
                    bit_prior = priors[chunk * N_CHUNK_BITS:(chunk + 1) * N_CHUNK_BITS]
                    branch_metric[c, a, u] += 0.5 * np.sum((1 - 2 * input_bits) * bit_prior)
                    # The last three padded bits are known zero, so only the
                    # 64 compatible 9-bit branches are legal in the final group.
                    if chunk == CHUNKS - 1 and (u & 0b111) != 0:
                        branch_metric[c, a, u] = -np.inf

        alpha = np.full((n_chunks + 1, g.N_NOTES), -np.inf)
        alpha[0, g.COSTAS7[6]] = 0.0
        for c in range(n_chunks):
            for a in range(g.N_NOTES):
                vals = alpha[c, a] + branch_metric[c, a]
                np.logaddexp.at(alpha[c + 1], branch_end[a], vals)
        beta = np.full((n_chunks + 1, g.N_NOTES), -np.inf)
        beta[-1] = terminal
        for c in range(n_chunks - 1, -1, -1):
            for a in range(g.N_NOTES):
                beta[c, a] = _lse(branch_metric[c, a] + beta[c + 1, branch_end[a]], axis=0)

        for c in range(n_chunks):
            llrs = []
            chunk = block_no * BLOCK_CHUNKS + c
            for bitpos in range(N_CHUNK_BITS):
                mask = (np.arange(N_PATHS) >> (N_CHUNK_BITS - 1 - bitpos)) & 1
                scores0, scores1 = [], []
                for a in range(g.N_NOTES):
                    scores = alpha[c, a] + branch_metric[c, a] + beta[c + 1, branch_end[a]]
                    scores0.append(_lse(scores[mask == 0], axis=0))
                    scores1.append(_lse(scores[mask == 1], axis=0))
                posterior = _lse(np.asarray(scores0), axis=0) - _lse(np.asarray(scores1), axis=0)
                llrs.append(posterior)
            output.extend(llrs)
        data_start += nnotes + 7
    posterior = np.asarray(output)
    ext = posterior - priors
    return np.clip(posterior[:fec.FRAME_BITS], -30, 30), np.clip(ext[:fec.FRAME_BITS], -30, 30)


def _fec_siso(frame_llr: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """BCJR for the prototype convolutional code, with soft output-bit LLRs."""
    orig = np.zeros(fec.FRAME_BITS)
    orig[fec.INTERLEAVE] = frame_llr
    coded_prior = orig[:fec.CODED_BITS]
    steps = fec.INFO_BITS + fec.TAIL
    alpha = np.full((steps + 1, fec.NSTATES), -np.inf)
    alpha[0, 0] = 0.0
    metric = np.full((steps, fec.NSTATES, 2), -np.inf)
    for t in range(steps):
        for state in range(fec.NSTATES):
            for bit in (0, 1):
                if t >= fec.INFO_BITS and bit:
                    continue
                outbits = fec._out[state, bit]
                signs = 1 - 2 * outbits
                metric[t, state, bit] = 0.5 * np.dot(signs, coded_prior[2 * t:2 * t + 2])
                nxt = fec._next[state, bit]
                alpha[t + 1, nxt] = np.logaddexp(alpha[t + 1, nxt], alpha[t, state] + metric[t, state, bit])
    beta = np.full((steps + 1, fec.NSTATES), -np.inf)
    beta[steps, 0] = 0.0
    for t in range(steps - 1, -1, -1):
        for state in range(fec.NSTATES):
            vals = [metric[t, state, bit] + beta[t + 1, fec._next[state, bit]] for bit in (0, 1)]
            beta[t, state] = _lse(np.asarray(vals), axis=0)

    coded_ext = np.zeros(fec.FRAME_BITS)
    info_post = np.zeros(fec.INFO_BITS)
    for t in range(steps):
        input_scores = [[], []]
        output_scores = [[[], []], [[], []]]
        for state in range(fec.NSTATES):
            for bit in (0, 1):
                if not np.isfinite(metric[t, state, bit]):
                    continue
                nxt = fec._next[state, bit]
                score = alpha[t, state] + metric[t, state, bit] + beta[t + 1, nxt]
                input_scores[bit].append(score)
                for j, outbit in enumerate(fec._out[state, bit]):
                    output_scores[j][int(outbit)].append(score)
        if t < fec.INFO_BITS:
            info_post[t] = _lse(np.asarray(input_scores[0]), axis=0) - _lse(np.asarray(input_scores[1]), axis=0)
        for j in range(2):
            post = _lse(np.asarray(output_scores[j][0]), axis=0) - _lse(np.asarray(output_scores[j][1]), axis=0)
            coded_ext[2 * t + j] = post - coded_prior[2 * t + j]
    coded_ext[fec.CODED_BITS:] = 30.0
    return coded_ext[fec.INTERLEAVE], info_post


@lru_cache(maxsize=10)
def _voice_for_length(gear: g.Gear, length: int):
    timed_gear = g.dataclasses.replace(gear, T=length / g.FS)
    return g.Voice(g.SCALES["pentatonic"][0], timed_gear)


def detect_rhythm(audio: np.ndarray, gear: g.Gear, choices: tuple[str, ...] = tuple(RHYTHMS)) -> tuple[str, dict[str, float]]:
    """Identify a known rhythm by scoring its expected Costas sync glides."""
    ranked = {}
    z_audio = g.analytic(audio)
    for rhythm in choices:
        lengths = symbol_lengths(gear, rhythm)
        starts = np.cumsum([0] + lengths[:-1]).tolist()
        score = 0.0
        for pos in STYLE_SYNC_POS:
            length = lengths[pos]
            cursor = starts[pos]
            segment = z_audio[cursor:cursor + length]
            if len(segment) != length:
                score = -np.inf
                break
            voice = _voice_for_length(gear, length)
            y = segment @ np.conj(voice.tmpl).transpose(0, 2, 1)
            target = g.COSTAS7[STYLE_SYNC_POS.index(pos) % 7]
            k = STYLE_SYNC_POS.index(pos)
            if k % 7 == 0 and pos != 0:
                best = np.max(np.abs(y[:, target]))
            elif pos == 0:
                best = np.max(np.abs(y[:, target]))
            else:
                source = g.COSTAS7[k % 7 - 1]
                best = abs(y[source, target])
            score += float(best ** 2 / (length * length))
        ranked[rhythm] = score
    best_name = max(ranked, key=ranked.get)
    return best_name, ranked


def _decode_style(audio: np.ndarray, gear: g.Gear, rhythm: str = "straight") -> tuple[np.ndarray, dict]:
    lengths = symbol_lengths(gear, rhythm)
    ys = []
    z_audio = g.analytic(audio)
    cursor = 0
    for length in lengths:
        segment = z_audio[cursor:cursor + length]
        if len(segment) != length:
            raise ValueError("audio is shorter than the selected rhythm frame")
        voice = _voice_for_length(gear, length)
        ys.append(segment @ np.conj(voice.tmpl).transpose(0, 2, 1))
        cursor += length
    power = np.asarray([np.abs(y) ** 2 for y in ys])
    # Exact-start AWGN bench: estimate correlation-noise power from nonwinning
    # templates, then use the same noncoherent likelihood as the production
    # prototype. This bench intentionally omits blind timing/frequency search.
    chosen = np.argmax(power.max(axis=1), axis=1)
    mask = np.ones_like(power, dtype=bool)
    mask[np.arange(STYLE_SYMBOLS), :, chosen] = False
    sync_c = []
    for k, pos in enumerate(STYLE_SYNC_POS):
        note = g.COSTAS7[k % 7]
        if k % 7 == 0:
            sync_c.append(np.max(np.abs(ys[pos][:, note])))
        else:
            sync_c.append(abs(ys[pos][g.COSTAS7[k % 7 - 1], note]))
    sync_c = np.asarray(sync_c)
    noises = np.maximum(np.median(power[mask].reshape(STYLE_SYMBOLS, -1), axis=1) / np.log(2), 1e-8)
    amp_norm = np.sqrt(max(np.mean(np.abs(sync_c) ** 2 / np.square(np.asarray(lengths)[STYLE_SYNC_POS])) -
                           np.mean(noises[STYLE_SYNC_POS] / np.square(np.asarray(lengths)[STYLE_SYNC_POS])), 1e-8))
    llm = [_llr0(2 * amp_norm * length * np.abs(y) / noises[i]) for i, (length, y) in enumerate(zip(lengths, ys))]
    bit_priors = np.zeros(PADDED_BITS)
    info_post = np.zeros(fec.INFO_BITS)
    for _ in range(3):
        _, inner_ext = _inner_soft_decode(llm, bit_priors)
        outer_ext, info_post = _fec_siso(inner_ext)
        bit_priors[:fec.FRAME_BITS] = outer_ext
    # info_post is the standard LLR log P(0)/P(1), like the modem's FEC LLRs.
    info_bits = (info_post < 0).astype(np.uint8)
    payload = info_bits[:fec.PAYLOAD_BITS]
    crc_ok = fec.crc14(payload) == info_bits[fec.PAYLOAD_BITS:fec.INFO_BITS].tolist()
    return payload, {"crc_ok": bool(crc_ok), "noise_est": float(np.mean(noises)), "amp_est": float(amp_norm)}


def _audio_for_notes(notes: list[int], gear: g.Gear, rhythm: str = "straight") -> np.ndarray:
    lengths = symbol_lengths(gear, rhythm, len(notes))
    frequencies = g.SCALES["pentatonic"][0]
    output = []
    phase = 0.0
    prev = notes[0]
    for note, length in zip(notes, lengths):
        track = g._pitch_track(frequencies[prev], frequencies[note], length, gear.glide)
        ph = phase + 2 * np.pi * np.cumsum(track) / g.FS
        output.append(np.sin(np.concatenate([[phase], ph[:-1]])))
        phase = ph[-1] % (2 * np.pi)
        prev = note
    return np.concatenate(output)


def _write_wav(path: str, samples: np.ndarray):
    x = np.clip(samples * 0.95, -1.0, 1.0)
    pcm = np.asarray(np.round(x * 32767), dtype="<i2")
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(g.FS)
        f.writeframes(pcm.tobytes())


def _interval_stats(data: list[int]) -> dict:
    d = np.diff(_NOTE_SEMITONES[np.asarray(data, dtype=int)])
    hist = {int(k): int(v) for k, v in zip(*np.unique(np.abs(d), return_counts=True))}
    preferred = sum(int(v) for k, v in hist.items() if k % 12 in (0, 3, 5, 7, 10))
    return {"mean_abs_semitones": float(np.mean(np.abs(d))), "within_5_semitones": float(np.mean(np.abs(d) <= 5)),
            "preferred_blues_classes": preferred / max(len(d), 1), "histogram": hist}


def _baseline_payload(payload: np.ndarray, gear: g.Gear) -> tuple[np.ndarray, list[int]]:
    coded = fec.encode_frame(payload)
    notes = g.frame_notes(coded)
    return coded, notes


def _run_awgn(args):
    rng = np.random.default_rng(args.seed)
    gear = g.GEARS[args.gear]
    baseline_ok = style_ok = rhythm_hits = 0
    for _ in range(args.trials):
        payload = rng.integers(0, 2, fec.PAYLOAD_BITS, dtype=np.uint8)
        coded, base_notes = _baseline_payload(payload, gear)
        style_notes = frame_notes(coded)
        base_audio = _audio_for_notes(base_notes, gear)
        style_audio = _audio_for_notes(style_notes, gear, args.rhythm)
        base_rx = ch.add_noise(base_audio, args.snr, rng, np.mean(base_audio ** 2))
        style_rx = ch.add_noise(style_audio, args.snr, rng, np.mean(style_audio ** 2))

        base_z = g.analytic(base_rx)
        base_voice = _voice_for_length(gear, gear.L)
        base_sync = g.SyncResult(0, 0.0, 1.0)
        base_llr, _ = g.demod_voice(base_z, gear, base_voice, base_sync)
        base_payload, base_crc = fec.decode_frame(base_llr)
        baseline_ok += bool(base_crc and np.array_equal(base_payload, payload))

        detected_rhythm, _ = detect_rhythm(style_rx, gear)
        rhythm_hits += detected_rhythm == args.rhythm
        style_payload, report = _decode_style(style_rx, gear, detected_rhythm)
        style_ok += bool(report["crc_ok"] and np.array_equal(style_payload, payload))
    print(f"known timing, AWGN {args.snr:+.1f} dB, G{args.gear}, {args.trials} trials")
    print(f"  current mapper: {baseline_ok}/{args.trials} decoded")
    print(f"  interval + {args.rhythm}: {style_ok}/{args.trials} decoded; rhythm identified {rhythm_hits}/{args.trials}")
    print(f"  frame duration: {gear.duration:.2f}s -> {sum(symbol_lengths(gear,args.rhythm))/g.FS:.2f}s")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trials", type=int, default=5)
    ap.add_argument("--snr", type=float, default=-20.0)
    ap.add_argument("--gear", type=int, choices=range(1, 6), default=3)
    ap.add_argument("--seed", type=int, default=731)
    ap.add_argument("--rhythm", choices=tuple(RHYTHMS), default="shuffle")
    ap.add_argument("--write-wav", metavar="DIR", help="write current, straight interval, and selected rhythm G3 clips")
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    payload = rng.integers(0, 2, fec.PAYLOAD_BITS, dtype=np.uint8)
    coded, base_notes = _baseline_payload(payload, g.GEARS[3])
    style_notes = frame_notes(coded)
    base_data = [base_notes[i] for i, c in enumerate(g.LAYOUT) if c == "D"]
    style_data = [style_notes[i] for i, c in enumerate(STYLE_LAYOUT) if c == "D"]
    print("Current data intervals:", _interval_stats(base_data))
    print("Interval-style data intervals:", _interval_stats(style_data))
    print(f"Mapper: {fec.FRAME_BITS} coded bits -> {DATA_NOTES} data notes ({100*(DATA_NOTES/65-1):.1f}% more data symbols)")
    print(f"Rhythm {args.rhythm}: symbol lengths {sorted(set(symbol_lengths(g.GEARS[3], args.rhythm)))} samples; detected as {detect_rhythm(_audio_for_notes(style_notes, g.GEARS[3], args.rhythm), g.GEARS[3])[0]}")
    print("Choices from each pitch:", MOVES)
    if args.write_wav:
        os.makedirs(args.write_wav, exist_ok=True)
        _write_wav(os.path.join(args.write_wav, "glissando-current-g3.wav"), _audio_for_notes(base_notes, g.GEARS[3]))
        _write_wav(os.path.join(args.write_wav, "glissando-interval-style-g3.wav"), _audio_for_notes(style_notes, g.GEARS[3]))
        _write_wav(os.path.join(args.write_wav, f"glissando-interval-style-{args.rhythm}-g3.wav"),
                   _audio_for_notes(style_notes, g.GEARS[3], args.rhythm))
    _run_awgn(args)


if __name__ == "__main__":
    main()
