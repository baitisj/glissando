"""Glissando modem prototype: a melodic chirp mode for HF.

Every symbol is a glide (a portamento chirp) from the previous note to the
next note of a pentatonic scale, followed by a sustain on the new note. The
data is the sequence of target notes, so the transmission is one continuous,
phase-continuous, constant-envelope melody. Three copies of a Costas-array
"signature motif" provide time and frequency sync, and their complex
correlations double as a channel sounder for gear shifting.

See docs/DESIGN.md for the reasoning behind every number here.
"""
import dataclasses
from dataclasses import dataclass

import numpy as np

import fec

FS = 8000  # audio sample rate, Hz

# A minor pentatonic (equal temperament, A4 = 440 Hz).
VOICE_LOW = np.array([329.63, 392.00, 440.00, 523.25, 587.33, 659.26, 783.99, 880.00])  # E4..A5
VOICE_HIGH = np.array([1046.50, 1174.66, 1318.51, 1567.98, 1760.00, 2093.00, 2349.32, 2637.02])  # C6..E7



def _midi(*nums):
    return 440.0 * 2.0 ** ((np.array(nums, dtype=float) - 69) / 12)


# Alternative scales, for the ear more than for the link budget. Each is a
# (low voice, high voice) pair of 8 notes; the high voice of every tritone
# scale is the low voice a tritone plus an octave up, which maps these
# symmetric scales onto themselves, so a duet is a tritone apart too.
#   wholetone   E4..F#5 in whole tones: notes 3 steps apart are a tritone,
#               so the Costas motif and most data glides land on one.
#   diminished  half-whole octatonic on E: every note has its tritone in
#               the scale; the semitones cost no sensitivity at G3 (DESIGN 3.1a).
#   diabolus    E major and Bb major triads a tritone apart (the Petrushka
#               chord), plus the octave of each root.
SCALES = {
    "pentatonic": (VOICE_LOW, VOICE_HIGH),
    "wholetone": (_midi(64, 66, 68, 70, 72, 74, 76, 78), _midi(82, 84, 86, 88, 90, 92, 94, 96)),
    "diminished": (_midi(64, 65, 67, 68, 70, 71, 73, 74), _midi(82, 83, 85, 86, 88, 89, 91, 92)),
    "diabolus": (_midi(64, 68, 70, 71, 74, 76, 77, 80), _midi(82, 86, 88, 89, 92, 94, 95, 98)),
}

N_NOTES = 8
BITS_PER_SYMBOL = 3
COSTAS7 = [3, 1, 4, 0, 6, 5, 2]  # FT8's 7x7 Costas array, played as a tune
N_DATA = fec.FRAME_BITS // BITS_PER_SYMBOL  # 65
DATA_BLOCKS = (32, 33)
# Layout: S7 D32 S7 D33 S7 = 86 symbols
LAYOUT = ["S"] * 7 + ["D"] * 32 + ["S"] * 7 + ["D"] * 33 + ["S"] * 7
N_SYMBOLS = len(LAYOUT)
SYNC_POS = [i for i, c in enumerate(LAYOUT) if c == "S"]
DATA_POS = [i for i, c in enumerate(LAYOUT) if c == "D"]
MOTIF_STARTS = (0, 39, 79)

GRAY = [0, 1, 3, 2, 6, 7, 5, 4]  # note index -> 3-bit label
GRAY_INV = np.argsort(GRAY)  # 3-bit label -> note index


@dataclass(frozen=True)
class Gear:
    name: str
    tempo: str
    T: float  # symbol duration, seconds
    voices: int  # 1 = solo (low voice), 2 = duet (low + high voice)
    glide: float = 0.4  # fraction of each symbol spent gliding
    scale: str = "pentatonic"  # key of SCALES

    @property
    def L(self):
        return int(round(self.T * FS))

    @property
    def duration(self):
        return N_SYMBOLS * self.T

    @property
    def payload_bits(self):
        return fec.PAYLOAD_BITS * self.voices

    @property
    def voice_notes(self):
        return list(SCALES[self.scale])[: self.voices]

    def with_scale(self, scale):
        return dataclasses.replace(self, scale=scale)


GEARS = {
    1: Gear("G1", "Adagio", 0.64, 1),
    2: Gear("G2", "Andante", 0.32, 1),
    3: Gear("G3", "Allegro", 0.16, 1),
    4: Gear("G4", "Presto", 0.08, 1),
    5: Gear("G5", "Presto duet", 0.08, 2),
}


# ---------------------------------------------------------------- waveforms

def _pitch_track(fa, fb, L, glide):
    """Instantaneous frequency for a glide fa -> fb then a sustain on fb.

    The glide is linear in log-frequency (constant cents per second, the
    way a voice or slide whistle moves) with a raised-cosine ease so the
    frequency has no corners: no clicks, a compact spectrum.
    """
    t = np.arange(L) / L
    x = np.clip(t / glide, 0.0, 1.0) if glide > 0 else np.ones(L)
    ease = 0.5 - 0.5 * np.cos(np.pi * x)
    return fa * (fb / fa) ** ease


def _template(fa, fb, L, glide):
    f = _pitch_track(fa, fb, L, glide)
    phase = 2 * np.pi * np.cumsum(f) / FS
    phase = np.concatenate([[0.0], phase[:-1]])
    return np.exp(1j * phase), 2 * np.pi * f.sum() / FS


class Voice:
    """Pre-computed matched-filter templates for every (from, to) note pair."""

    def __init__(self, notes, gear):
        self.notes = notes
        L = gear.L
        self.tmpl = np.zeros((N_NOTES, N_NOTES, L), dtype=complex)
        self.advance = np.zeros((N_NOTES, N_NOTES))
        for a in range(N_NOTES):
            for b in range(N_NOTES):
                self.tmpl[a, b], self.advance[a, b] = _template(notes[a], notes[b], L, gear.glide)
        # Sustain-only template, for sync symbols whose predecessor is data.
        self.hold = np.zeros((N_NOTES, L), dtype=complex)
        n0 = int(np.ceil(gear.glide * L))
        for b in range(N_NOTES):
            self.hold[b, n0:] = self.tmpl[b, b, n0:]


def bits_to_notes(bits):
    bits = np.asarray(bits).reshape(-1, BITS_PER_SYMBOL)
    labels = bits[:, 0] * 4 + bits[:, 1] * 2 + bits[:, 2]
    return GRAY_INV[labels]


def frame_notes(coded_bits):
    data = iter(bits_to_notes(coded_bits))
    motif = COSTAS7 * 3
    notes, m = [], iter(motif)
    for c in LAYOUT:
        notes.append(next(m) if c == "S" else next(data))
    return notes


def modulate_voice(notes, voice_notes, gear):
    L, out, phase = gear.L, [], 0.0
    prev = notes[0]
    for n in notes:
        f = _pitch_track(voice_notes[prev], voice_notes[n], L, gear.glide)
        ph = phase + 2 * np.pi * np.cumsum(f) / FS
        out.append(np.sin(np.concatenate([[phase], ph[:-1]])))
        phase = ph[-1] % (2 * np.pi)
        prev = n
    return np.concatenate(out)


def transmit(payloads, gear, ramp=0.01):
    """Real audio for one transmission (peak amplitude 1, constant envelope)."""
    x = 0
    for p, vn in zip(payloads, gear.voice_notes):
        x = x + modulate_voice(frame_notes(fec.encode_frame(p)), vn, gear)
    x = x / gear.voices
    r = int(ramp * FS)  # short fade in/out, keeps the key-down click off the air
    w = np.ones(len(x))
    w[:r] = 0.5 - 0.5 * np.cos(np.pi * np.arange(r) / r)
    w[-r:] = w[:r][::-1]
    return x * w


# ---------------------------------------------------------------- receiver

def analytic(x):
    n = len(x)
    X = np.fft.fft(x)
    h = np.zeros(n)
    h[0] = 1
    h[1:(n + 1) // 2] = 2
    if n % 2 == 0:
        h[n // 2] = 1
    return np.fft.ifft(X * h)


def _logi0(x):
    x = np.asarray(x, dtype=float)
    small = x < 30
    out = np.empty_like(x)
    out[small] = np.log(np.i0(x[small]))
    xl = x[~small]
    out[~small] = xl - 0.5 * np.log(2 * np.pi * xl)
    return out


def _logsumexp(a, axis):
    m = np.max(a, axis=axis, keepdims=True)
    return np.squeeze(m, axis) + np.log(np.sum(np.exp(a - m), axis=axis))


@dataclass
class SyncResult:
    start: int  # sample index of symbol 0
    df: float  # frequency offset, Hz
    score: float


def _sync_templates(voice):
    t = []
    for k, pos in enumerate(SYNC_POS):
        note = COSTAS7[k % 7]
        if k % 7 == 0:
            t.append(voice.tmpl[note, note] if pos == 0 else voice.hold[note])
        else:
            t.append(voice.tmpl[COSTAS7[k % 7 - 1], note])
    return np.array(t)


def sync_search(z, gear, voice, t_range, f_max=25.0, top=3):
    """Coarse-to-fine search over start time and frequency offset.

    Each sync symbol is multiplied by the conjugate of its known glide
    ("dechirping"): a correctly timed symbol collapses to a tone at the
    frequency offset, so one zero-padded FFT scores every offset at once.
    Powers are summed non-coherently over the 21 sync symbols.
    """
    L = gear.L
    tmpl = np.conj(_sync_templates(voice))
    offs = np.array(SYNC_POS) * L
    nfft = 4 * L
    freqs = np.fft.fftfreq(nfft, 1 / FS)
    keep = np.abs(freqs) <= f_max
    fk = freqs[keep]

    def score(starts):
        res = []
        for s in starts:
            seg = np.stack([z[s + o: s + o + L] for o in offs])
            P = np.abs(np.fft.fft(seg * tmpl, nfft, axis=1)) ** 2
            res.append(P.sum(axis=0)[keep])
        return np.array(res)

    step = max(1, L // 8)
    t0, t1 = t_range
    t1 = min(t1, len(z) - N_SYMBOLS * L)
    starts = np.arange(max(0, t0), t1, step)
    S = score(starts)
    # noise-normalise: typical (median) score across the search space
    S = S / np.median(S)
    cands = []
    flat = np.argsort(S, axis=None)[::-1]
    for idx in flat:
        i, j = np.unravel_index(idx, S.shape)
        if any(abs(starts[i] - c[0]) < L and abs(fk[j] - c[1]) < 4 / gear.T for c in cands):
            continue
        cands.append((starts[i], fk[j], S[i, j]))
        if len(cands) >= top:
            break
    out = []
    for s0, f0, sc in cands:
        fine = np.arange(max(0, s0 - step), min(t1, s0 + step + 1), max(1, L // 256))
        Sf = score(fine)
        i, j = np.unravel_index(np.argmax(Sf), Sf.shape)
        df = fk[j]
        if 0 < j < len(fk) - 1:  # parabolic peak interpolation
            a, b, c = np.log(Sf[i, j - 1:j + 2] + 1e-30)
            df += 0.5 * (a - c) / (a - 2 * b + c) * (fk[1] - fk[0])
        out.append(SyncResult(int(fine[i]), float(df), float(sc)))
    return out


def _sync_energy(z, gear, tmpl, start, df):
    L = gear.L
    e = 0.0
    for k, pos in enumerate(SYNC_POS):
        s0 = start + pos * L
        r = z[s0: s0 + L] * np.exp(-2j * np.pi * df * (np.arange(L) + s0) / FS)
        e += np.abs(np.dot(r, tmpl[k])) ** 2
    return e


def refine_sync(z, gear, voice, sync):
    """Polish time and frequency after the coarse search.

    A glide is a chirp, and a chirp compresses in time: a glide sweeping a
    few hundred Hz has a timing resolution of a few milliseconds, and at
    G1 a 1.25 ms timing error already costs 1.5 dB. So time is refined to
    the sample on the known sync glides. Frequency is refined on the
    sustained part of every note over all 86 symbols, which has no
    time-frequency coupling.
    """
    L = gear.L
    tmpl = np.conj(_sync_templates(voice))
    lo, hi = 0, len(z) - N_SYMBOLS * L

    def best_time(start, df, half, step):
        cands = [t for t in range(start - half, start + half + 1, step) if lo <= t <= hi]
        return max(cands, key=lambda t: _sync_energy(z, gear, tmpl, t, df))

    start = best_time(sync.start, sync.df, max(1, L // 256) + 2, max(1, L // 512))
    start = best_time(start, sync.df, max(1, L // 512), 1)

    hold = np.conj(voice.hold)
    n = np.arange(N_SYMBOLS * L)
    seg = z[start: start + N_SYMBOLS * L]
    best = (-1.0, sync.df)
    for df in sync.df + np.arange(-6, 7) * (0.0625 / gear.T):
        r = (seg * np.exp(-2j * np.pi * df * (n + start) / FS)).reshape(N_SYMBOLS, L)
        e = np.sum(np.max(np.abs(r @ hold.T) ** 2, axis=1))
        if e > best[0]:
            best = (e, df)
    df = float(best[1])
    start = best_time(start, df, 4, 1)
    return SyncResult(int(start), df, sync.score)


def demod_voice(z, gear, voice, sync):
    """Soft demodulation. Returns (bit LLRs, channel report dict)."""
    L = gear.L
    n = np.arange(N_SYMBOLS * L)
    seg_all = z[sync.start: sync.start + N_SYMBOLS * L] * np.exp(-2j * np.pi * sync.df * (n + sync.start) / FS)
    seg_all = seg_all.reshape(N_SYMBOLS, L)
    # y[k, a, b] = <r_k, template(a -> b)>
    y = np.einsum("kl,abl->kab", seg_all, np.conj(voice.tmpl))
    p = np.abs(y) ** 2

    # Noise level: the median of |y|^2 over wrong-target templates is ~ ln2 * noise.
    # Exclude each symbol's most likely target note; what is left is mostly noise.
    bhat = np.argmax(p.max(axis=1), axis=1)
    mask = np.ones_like(p, dtype=bool)
    mask[np.arange(N_SYMBOLS), :, bhat] = False
    noise = np.median(p[mask]) / np.log(2)
    # Signal amplitude from the sync symbols (targets known).
    sync_c = []
    for k, pos in enumerate(SYNC_POS):
        b = COSTAS7[k % 7]
        a = b if pos == 0 else (COSTAS7[k % 7 - 1] if k % 7 else None)
        sync_c.append(y[pos, a, b] if a is not None else np.max(np.abs(y[pos, :, b])))
    sync_c = np.array(sync_c)
    es = max(np.mean(np.abs(sync_c) ** 2) - noise, 1e-3 * noise)
    amp = np.sqrt(es)
    llm = _logi0(2 * amp * np.abs(y) / noise)  # log-likelihood of each glide

    # BCJR over the note trellis for each data block. The block starts in a
    # known state (last motif note) and is followed by a known target note
    # (first note of the next motif), which is extra evidence for the last
    # data note because that glide starts from it.
    note_post = []
    pos = 7
    for nb in DATA_BLOCKS:
        start_note = COSTAS7[6]
        g = llm[pos: pos + nb]  # (nb, a, b)
        term = llm[pos + nb, :, COSTAS7[0]]  # glide from last data note into the motif
        alpha = np.full((nb + 1, N_NOTES), -np.inf)
        alpha[0, start_note] = 0
        for t in range(nb):
            alpha[t + 1] = _logsumexp(alpha[t][:, None] + g[t], axis=0)
        beta = np.zeros((nb + 1, N_NOTES))
        beta[nb] = term
        for t in range(nb - 1, -1, -1):
            beta[t] = _logsumexp(g[t] + beta[t + 1][None, :], axis=1)
        for t in range(nb):
            post = _logsumexp(alpha[t][:, None] + g[t], axis=0) + beta[t + 1]
            note_post.append(post - _logsumexp(post, axis=0))
        pos += nb + 7
    note_post = np.array(note_post)  # (65, 8)

    labels = np.array(GRAY)
    llr = np.zeros((N_DATA, BITS_PER_SYMBOL))
    for i in range(BITS_PER_SYMBOL):
        bit = (labels >> (BITS_PER_SYMBOL - 1 - i)) & 1
        llr[:, i] = _logsumexp(note_post[:, bit == 0], axis=1) - _logsumexp(note_post[:, bit == 1], axis=1)
    llr = np.clip(llr, -30, 30).reshape(-1)

    report = channel_report(sync_c, es, noise, gear, voice)
    return llr, report


def channel_report(sync_c, es, noise, gear, voice):
    """SNR and Doppler spread measured on the three signature motifs.

    Within a motif the waveform is phase-continuous and every note is known,
    so after removing the known phase advance of each glide, successive
    symbol correlations should have the same phase on a static channel.
    Their normalised correlation at lag T is exp(-2 pi^2 sigma^2 T^2) for
    a Gaussian Doppler spectrum; the Doppler spread is 2 sigma.
    """
    L = gear.L
    # SNR in 2500 Hz: per-sample signal power over per-sample noise power
    # (analytic noise is white over FS), rescaled to 2500 Hz.
    snr = 10 * np.log10(es / noise * FS / (L * 2500.0))
    # Use symbols 1..6 of each motif: their predecessors are known notes, so
    # their correlations carry phase. Symbol k glides COSTAS7[k-1] -> COSTAS7[k].
    num, den, lags = 0j, 0.0, 0
    for m in range(3):
        c = sync_c[7 * m + 1: 7 * m + 7].copy()
        for k in range(1, 6):
            a, b = COSTAS7[k - 1], COSTAS7[k]  # template of the earlier symbol
            c[k:] *= np.exp(-1j * voice.advance[a, b])  # undo known phase ramp
        num += np.sum(c[1:] * np.conj(c[:-1]))
        den += np.sum(np.abs(c[:-1]) ** 2)
        lags += 5
    rho = np.abs(num) / max(den - lags * noise, 1e-9)
    rho = float(np.clip(rho, 1e-6, 1.0))
    sigma = np.sqrt(-np.log(rho) / (2 * np.pi ** 2 * gear.T ** 2)) if rho < 1 else 0.0
    return {"snr_db": float(snr), "doppler_hz": float(2 * sigma), "coherence": rho}


def sound_channel(z, gear, voice, sync, payload):
    """Decision-directed channel sounding after a good decode.

    With the payload known, every note of the melody is known, and so is
    the waveform's running phase. Each symbol's correlation is then a
    sample of the channel gain at that note's pitch. Pairs of symbols on
    the same note at a lag of d symbols estimate the time correlation
    rho(d*T) = exp(-2 pi^2 sigma^2 (d*T)^2) of a Gaussian-scatter path;
    the fit gives the Doppler spread 2*sigma. Using the same note removes
    the frequency selectivity that multipath delay adds between notes.
    """
    L = gear.L
    notes = frame_notes(fec.encode_frame(payload))
    n = np.arange(N_SYMBOLS * L)
    seg = z[sync.start: sync.start + N_SYMBOLS * L] * np.exp(-2j * np.pi * sync.df * (n + sync.start) / FS)
    seg = seg.reshape(N_SYMBOLS, L)
    prev = [notes[0]] + notes[:-1]
    c = np.array([np.vdot(voice.tmpl[a, b], seg[k]) for k, (a, b) in enumerate(zip(prev, notes))])
    adv = np.concatenate([[0.0], np.cumsum([voice.advance[a, b] for a, b in zip(prev, notes)])[:-1]])
    c = c * np.exp(-1j * adv)
    noise = np.median(np.abs(np.einsum("kl,bl->kb", seg, np.conj(voice.hold))) ** 2) / np.log(2)
    noise *= 1.0 / (1 - gear.glide)  # hold templates are shorter than full ones
    power = np.mean(np.abs(c) ** 2)
    if power <= noise:
        return None
    # |E[c(t+tau) c*(t)]|^2 via the unbiased pair statistic, so lags with
    # few same-note pairs do not read as falsely coherent.
    lags, rhos, wts = [], [], []
    for d in range(1, max(2, int(2.0 / gear.T)) + 1):
        zk = np.array([c[k + d] * np.conj(c[k]) for k in range(N_SYMBOLS - d) if notes[k] == notes[k + d]])
        if len(zk) < 3:
            continue
        r2 = (np.abs(zk.sum()) ** 2 - np.sum(np.abs(zk) ** 2)) / (len(zk) * (len(zk) - 1))
        rho = np.sqrt(max(r2, 0.0)) / (power - noise)
        lags.append(d * gear.T)
        rhos.append(rho)
        wts.append(len(zk))
    lags, rhos, wts = np.array(lags), np.clip(rhos, 0, 0.99), np.array(wts)
    # Fit only the first run of lags that still show correlation; single
    # frames have few same-note pairs, so the noisy tail is ignored.
    run = np.cumprod(rhos > 0.15).astype(bool)
    if not run.any():
        # Decorrelated within one symbol: report the bound, i.e. "fast".
        return float(2 * np.sqrt(-np.log(0.15) / (2 * np.pi ** 2)) / gear.T)
    use = run
    x, y, w = lags[use] ** 2, -np.log(rhos[use]), wts[use]
    s2 = np.sum(w * x * y) / np.sum(w * x * x) / (2 * np.pi ** 2)
    return float(2 * np.sqrt(max(s2, 0.0)))


def receive(audio, gear, t_range=None, f_max=25.0, top=3):
    """Decode every voice of a transmission. Returns list of (payload|None, report)."""
    z = analytic(np.asarray(audio, dtype=float))
    if t_range is None:
        t_range = (0, len(z))
    results = []
    for vn in gear.voice_notes:
        voice = _voice_cache(tuple(vn), gear)
        best = (None, None)
        for s in sync_search(z, gear, voice, t_range, f_max, top):
            s = refine_sync(z, gear, voice, s)
            llr, rep = demod_voice(z, gear, voice, s)
            payload, ok = fec.decode_frame(llr)
            rep.update(start=s.start, df=s.df)
            if best[1] is None:
                best = (None, rep)
            if ok:
                dop = sound_channel(z, gear, voice, s, payload)
                if dop is not None:
                    rep["doppler_hz"] = dop
                best = (payload, rep)
                break
        results.append(best)
    return results


_cache = {}


def _voice_cache(notes, gear):
    key = (notes, gear)
    if key not in _cache:
        _cache[key] = Voice(np.array(notes), gear)
    return _cache[key]


# ---------------------------------------------------------------- gear shifting

# SNR (2500 Hz) for 90 % decode on the CCIR "moderate" path, from the
# prototype's sweeps (docs/DESIGN.md, "Measured performance"), and the
# largest symbol-duration x Doppler-spread product a gear is used at. The
# sweeps show G1 still beats G2 on the CCIR "poor" path (T x fd = 0.64),
# so the Doppler limit only bites on flutter / auroral paths.
GEAR_TABLE = {1: -22.2, 2: -19.1, 3: -15.5, 4: -13.0, 5: -7.8}
MAX_T_FD = 1.0
MARGIN_DB = 2.0


def recommend_gear(snr_db, doppler_hz):
    """Pick the fastest gear the measured path supports."""
    ok = [g for g, thr in GEAR_TABLE.items()
          if snr_db >= thr + MARGIN_DB and GEARS[g].T * doppler_hz <= MAX_T_FD]
    if ok:
        return max(ok)
    # Nothing fits with margin: slowest gear the Doppler spread allows.
    fits = [g for g in GEAR_TABLE if GEARS[g].T * doppler_hz <= MAX_T_FD]
    return min(fits) if fits else 3
