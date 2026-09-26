"""Noise-and-fading test bench: decode probability versus SNR, either in
software or through a real audio path.

    python3 bench.py --gears 3 --channels awgn poor --trials 20
    python3 bench.py --backend pulse --null-sink --gears 4 --snr -14 -12 -10
    python3 bench.py --backend pulse --sink alsa_output.usb-... --source alsa_input.usb-... \\
                     --snr none --gears 3 --trials 5      # on the air: no synthetic noise

Each trial: random payload(s), random start time, random frequency offset
(+/-15 Hz), the chosen HF fading profile (channel.py), then AWGN at the
stated SNR (2500 Hz reference, the WSJT-X convention). The `sim` backend
hands that buffer straight to the receiver; the `pulse` backend plays it
on a PulseAudio sink and records from a source, so the sound card, its
resamplers and clocks, a rig, or the air itself sit between transmitter
and receiver. A trial is decoded only if every voice comes back exact.

Results go to a JSON file in the same layout as sim.py's, so
`tabulate.py results.json` prints the 50 % / 90 % thresholds.
"""
import argparse
import contextlib
import json
import sys
import time
import zlib
from multiprocessing import Pool

import numpy as np

import channel as ch
import glissando as g
import pulse
from sim import SNR_SPAN, snr_grid


def impair(gear, chan, snr, rng, lead_range=(0.3, 1.2), df_max=15.0):
    """Transmission plus impairments; returns (buffer, payloads)."""
    payloads = [rng.integers(0, 2, 77) for _ in range(gear.voices)]
    x = g.transmit(payloads, gear)
    lead = int(rng.uniform(*lead_range) * g.FS)
    buf = np.concatenate([np.zeros(lead), x, np.zeros(int(1.5 * g.FS) - lead)])
    buf = ch.freq_shift(buf, rng.uniform(-df_max, df_max))
    buf = ch.hf_channel(buf, chan, rng)
    if snr is not None:
        buf = ch.add_noise(buf, snr, rng, np.mean(x ** 2))
    return buf, payloads


def judge(res, payloads, scale=None):
    """A trial is ok when every voice comes back exact (and, when `scale` is
    given, is reported as heard in that scale)."""
    ok = all(p is not None and np.array_equal(p, q) and (scale is None or r.get("scale") == scale)
             for (p, r), q in zip(res, payloads))
    false = any(p is not None and not np.array_equal(p, q) for (p, _), q in zip(res, payloads))
    rep = res[0][1]
    return dict(ok=bool(ok), false=bool(false), snr_est=rep["snr_db"], doppler_est=rep["doppler_hz"])


def trial_sim(job):
    gi, chan, snr, seed, scale, *rx = job  # optional 6th: scale(s) listened for
    gear = g.GEARS[gi].with_scale(scale)
    rng = np.random.default_rng(seed)
    buf, payloads = impair(gear, chan, snr, rng)
    return job, judge(g.receive(buf, gear, scales=rx[0] if rx else None), payloads, scale)


def trial_pulse(job, sink, source, peak=0.9):
    gi, chan, snr, seed, scale, *rx = job  # optional 6th: scale(s) listened for
    gear = g.GEARS[gi].with_scale(scale)
    rng = np.random.default_rng(seed)
    buf, payloads = impair(gear, chan, snr, rng)
    gain = peak / np.max(np.abs(buf))  # keep noise peaks inside the DAC's range
    y = pulse.play_and_record(buf * gain, g.FS, sink, source)
    r = judge(g.receive(y, gear, scales=rx[0] if rx else None), payloads, scale)
    r.update(rx_peak=float(np.max(np.abs(y))), rx_rms=float(np.sqrt(np.mean(y ** 2))),
             rx_seconds=len(y) / g.FS, tx_gain=float(gain))
    return job, r


def summarize(rows):
    table = {}
    for (gi, c, snr, *_), r in rows:
        key = f"{gi}/{c}/{'none' if snr is None else snr}"
        d = table.setdefault(key, {"n": 0, "ok": 0, "false": 0, "snr_est": [], "doppler_est": []})
        d["n"] += 1
        d["ok"] += r["ok"]
        d["false"] += r["false"]
        d["snr_est"].append(r["snr_est"])
        d["doppler_est"].append(r["doppler_est"])
        for k in ("rx_peak", "rx_rms"):
            if k in r:
                d.setdefault(k, []).append(r[k])
    for d in table.values():
        for k in ("snr_est", "doppler_est", "rx_peak", "rx_rms"):
            if k in d:
                d[k] = float(np.median(d[k]))
    return table


def print_table(table):
    for k, d in table.items():
        extra = f"  rx peak {d['rx_peak']:.2f} rms {d['rx_rms']:.3f}" if "rx_peak" in d else ""
        print(f"{k:16s} {d['ok']:3d}/{d['n']:<3d} false {d['false']}  "
              f"snr_est {d['snr_est']:6.1f}  dop {d['doppler_est']:.2f}{extra}")


def parse_snr(values, gi, chan):
    if values is None:
        return list(snr_grid(gi, chan))
    if values == ["none"]:
        return [None]
    return [int(v) for v in values]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backend", choices=["sim", "pulse"], default="sim")
    ap.add_argument("--gears", type=int, nargs="+", default=[3])
    ap.add_argument("--channels", nargs="+", default=["awgn"], choices=list(ch.PROFILES))
    ap.add_argument("--snr", nargs="+", help="SNR values in dB, 'none' for no added noise; default: sim.py's grid")
    ap.add_argument("--trials", type=int, default=10)
    ap.add_argument("--scale", default="pentatonic", choices=list(g.SCALES))
    ap.add_argument("--rx-scale", default=None, choices=list(g.SCALES) + ["auto"],
                    help="scale the receiver listens for (default: --scale; auto: all of them)")
    ap.add_argument("--seed", default="bench", help="salt for the per-trial seeds")
    ap.add_argument("--out", default="bench_results.json")
    ap.add_argument("--sink", help="pulse: sink to play on (default: the server's default sink)")
    ap.add_argument("--source", help="pulse: source to record from (default: the default source)")
    ap.add_argument("--null-sink", action="store_true",
                    help="pulse: load a temporary null sink and record its monitor (software loopback)")
    ap.add_argument("--list-devices", action="store_true", help="pulse: print sinks and sources, then exit")
    a = ap.parse_args(argv)

    if a.backend == "pulse" and not pulse.available():
        sys.exit("PulseAudio backend needs pacat/parec/pactl and a running server (pulseaudio or pipewire-pulse)")
    if a.list_devices:
        sinks, sources = pulse.list_devices()
        print("sinks:\n  " + "\n  ".join(sinks) + "\nsources:\n  " + "\n  ".join(sources))
        return

    jobs = [(gi, c, snr, zlib.crc32(f"{a.seed}/{gi}/{c}/{snr}/{t}".encode()), a.scale, a.rx_scale)
            for gi in a.gears for c in a.channels for snr in parse_snr(a.snr, gi, c) for t in range(a.trials)]
    t0 = time.time()
    if a.backend == "sim":
        with Pool() as pool:
            rows = pool.map(trial_sim, jobs, chunksize=4)
    else:
        sink, source = a.sink, a.source
        with (pulse.null_sink() if a.null_sink else contextlib.nullcontext((sink, source))) as (sink, source):
            rows = []
            for i, job in enumerate(jobs):
                rows.append(trial_pulse(job, sink, source))
                r = rows[-1][1]
                print(f"[{i + 1}/{len(jobs)}] G{job[0]} {job[1]} {job[2]} dB: "
                      f"{'ok' if r['ok'] else 'FALSE' if r['false'] else 'miss'}  "
                      f"snr_est {r['snr_est']:.1f}  rx peak {r['rx_peak']:.2f}", flush=True)
    table = summarize(rows)
    json.dump(table, open(a.out, "w"), indent=1)
    print_table(table)
    print(f"{len(jobs)} trials in {time.time() - t0:.0f} s, results in {a.out}")


if __name__ == "__main__":
    main()
