"""Forward error correction for the Glissando prototype.

Frame: 77-bit payload (same size as FT8/FT4 messages) + CRC-14 = 91 bits,
then a K=7, rate-1/2 convolutional code (the NASA/Voyager 0o133/0o171 pair)
with 6 tail bits = 194 coded bits, padded to 195 and bit-interleaved.

The production design calls for the FT8 LDPC(174,91) code instead (see
docs/DESIGN.md); the convolutional code is used here because it is short,
well understood, and easy to decode with soft decisions in pure NumPy.
"""
import numpy as np

PAYLOAD_BITS = 77
CRC_BITS = 14
INFO_BITS = PAYLOAD_BITS + CRC_BITS
K = 7
TAIL = K - 1
G = (0o133, 0o171)
CODED_BITS = 2 * (INFO_BITS + TAIL)  # 194
FRAME_BITS = 195  # 65 symbols x 3 bits

CRC14_POLY = 0x2757  # the polynomial FT8 uses


def crc14(bits):
    reg = 0
    for b in list(bits) + [0] * CRC_BITS:
        reg = (reg << 1) | int(b)
        if reg & (1 << CRC_BITS):
            reg ^= CRC14_POLY | (1 << CRC_BITS)
    return [(reg >> (CRC_BITS - 1 - i)) & 1 for i in range(CRC_BITS)]


def _parity(x):
    return bin(x).count("1") & 1


# Trellis tables: state = last K-1 input bits, newest in the LSB.
NSTATES = 1 << (K - 1)
_next = np.zeros((NSTATES, 2), dtype=int)
_out = np.zeros((NSTATES, 2, 2), dtype=int)
for s in range(NSTATES):
    for u in (0, 1):
        reg = (s << 1) | u  # K bits, newest in LSB
        _next[s, u] = reg & (NSTATES - 1)
        _out[s, u] = [_parity(reg & g) for g in G]


def conv_encode(bits):
    s, out = 0, []
    for u in list(bits) + [0] * TAIL:
        out.extend(_out[s, u])
        s = _next[s, u]
    return np.array(out, dtype=np.uint8)


# Predecessor tables for a vectorised Viterbi.
_prev = [[] for _ in range(NSTATES)]
for s in range(NSTATES):
    for u in (0, 1):
        _prev[_next[s, u]].append((s, u))
_prev_s = np.array([[p[0] for p in ps] for ps in _prev])  # (NSTATES, 2)
_prev_u = np.array([[p[1] for p in ps] for ps in _prev])
_prev_out = np.array([[_out[s, u] for s, u in ps] for ps in _prev])  # (NSTATES,2,2)


def viterbi_decode(llr):
    """Soft Viterbi. llr > 0 means bit 0 is more likely. Returns info bits."""
    n = len(llr) // 2
    llr = np.asarray(llr[: 2 * n]).reshape(n, 2)
    pm = np.full(NSTATES, -1e18)
    pm[0] = 0.0
    sign = 1 - 2 * _prev_out  # bit 0 -> +1, bit 1 -> -1
    decisions = np.zeros((n, NSTATES), dtype=np.int8)
    for t in range(n):
        bm = 0.5 * (sign * llr[t]).sum(axis=2)  # (NSTATES, 2)
        cand = pm[_prev_s] + bm
        pick = np.argmax(cand, axis=1)
        pm = cand[np.arange(NSTATES), pick]
        decisions[t] = pick
    s = 0  # tail forces the zero state
    bits = np.zeros(n, dtype=np.uint8)
    for t in range(n - 1, -1, -1):
        p = decisions[t, s]
        bits[t] = _prev_u[s, p]
        s = _prev_s[s, p]
    return bits[: n - TAIL]


_rng = np.random.default_rng(0x61155A)
INTERLEAVE = _rng.permutation(FRAME_BITS)


def encode_frame(payload):
    info = list(payload) + crc14(payload)
    coded = np.concatenate([conv_encode(info), [0] * (FRAME_BITS - CODED_BITS)])
    return coded[INTERLEAVE].astype(np.uint8)


def decode_frame(llr_frame):
    """Returns (payload bits, crc_ok)."""
    llr = np.empty(FRAME_BITS)
    llr[INTERLEAVE] = llr_frame
    info = viterbi_decode(llr[:CODED_BITS])
    payload, crc = list(info[:PAYLOAD_BITS]), list(info[PAYLOAD_BITS:])
    return np.array(payload, dtype=np.uint8), crc14(payload) == crc
