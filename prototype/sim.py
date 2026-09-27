"""Monte-Carlo decode-probability sweeps.

    python3 sim.py --gears 1 2 3 4 5 --channels awgn moderate poor --trials 40

Each trial: random payload(s), random start time (0.3-1.2 s into a buffer)
and random frequency offset (+/-15 Hz), through the chosen HF channel, AWGN
at the stated SNR (2500 Hz reference), then a blind receive (time and
frequency search). A trial counts as decoded only if every voice's payload
comes back exact with a good CRC.
"""
import argparse
import json
import zlib
from multiprocessing import Pool

import numpy as np

import channel as ch
import glissando as g

# (lowest SNR, highest SNR); 1 dB steps in AWGN, 2 dB on fading paths.
SNR_SPAN = {1: (-29, -13), 2: (-26, -10), 3: (-23, -7), 4: (-20, -4), 5: (-17, -1)}


def snr_grid(gi, chan):
    lo, hi = SNR_SPAN[gi]
    return range(lo, hi + 1, 1 if chan == "awgn" else 2)


def trial(args):
    gi, chan, snr, seed, scale, rx_scale = args
    rng = np.random.default_rng(seed)
    gear = g.GEARS[gi].with_scale(scale)
    payloads = [rng.integers(0, 2, 77) for _ in range(gear.voices)]
    x = g.transmit(payloads, gear)
    lead = int(rng.uniform(0.3, 1.2) * g.FS)
    buf = np.concatenate([np.zeros(lead), x, np.zeros(int(1.5 * g.FS) - lead)])
    buf = ch.freq_shift(buf, rng.uniform(-15, 15))
    buf = ch.hf_channel(buf, chan, rng)
    buf = ch.add_noise(buf, snr, rng, np.mean(x ** 2))  # average power, as WSJT-X reports
    res = g.receive(buf, gear, t_range=(0, int(1.5 * g.FS)), scales=rx_scale)
    ok = all(p is not None and np.array_equal(p, q) and r["scale"] == scale for (p, r), q in zip(res, payloads))
    false = any(p is not None and not np.array_equal(p, q) for (p, _), q in zip(res, payloads))
    rep = res[0][1]
    return gi, chan, snr, ok, false, rep["snr_db"], rep["doppler_hz"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gears", type=int, nargs="+", default=[1, 2, 3, 4, 5])
    ap.add_argument("--channels", nargs="+", default=["awgn", "moderate", "poor"])
    ap.add_argument("--trials", type=int, default=40)
    ap.add_argument("--scale", default="pentatonic", choices=list(g.SCALES))
    ap.add_argument("--rx-scale", default=None, choices=list(g.SCALES) + ["auto"],
                    help="scale the receiver listens for (default: --scale; auto: all of them)")
    ap.add_argument("--out", default="sim_results.json")
    a = ap.parse_args()
    jobs = [(gi, c, snr, zlib.crc32(f"{gi}/{c}/{snr}/{t}".encode()), a.scale, a.rx_scale)
            for gi in a.gears for c in a.channels for snr in snr_grid(gi, c) for t in range(a.trials)]
    with Pool() as pool:
        rows = pool.map(trial, jobs, chunksize=4)
    table = {}
    for gi, c, snr, ok, false, est, dop in rows:
        d = table.setdefault(f"{gi}/{c}/{snr}", {"n": 0, "ok": 0, "false": 0, "snr_est": [], "doppler_est": []})
        d["n"] += 1
        d["ok"] += ok
        d["false"] += false
        d["snr_est"].append(est)
        d["doppler_est"].append(dop)
    for k, d in table.items():
        d["snr_est"] = float(np.median(d["snr_est"]))
        d["doppler_est"] = float(np.median(d["doppler_est"]))
    json.dump(table, open(a.out, "w"), indent=1)
    for k, d in table.items():
        print(k, f"{d['ok']}/{d['n']}", "false", d["false"], "snr_est %.1f dop %.2f" % (d["snr_est"], d["doppler_est"]))


if __name__ == "__main__":
    main()
