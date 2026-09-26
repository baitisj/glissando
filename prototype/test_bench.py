"""python3 -m pytest prototype/  (the PulseAudio test skips without a server)."""
import numpy as np
import pytest

import bench
import glissando as g
import pulse


def test_sim_decodes_clean_signal():
    _, r = bench.trial_sim((4, "awgn", 0, 1))
    assert r["ok"] and not r["false"]


def test_sim_misses_buried_signal_without_false_decode():
    _, r = bench.trial_sim((4, "awgn", -40, 2))
    assert not r["ok"] and not r["false"]


def test_summary_layout_matches_sim_results():
    rows = [((4, "awgn", -10, s), dict(ok=s % 2 == 0, false=False, snr_est=-10.0, doppler_est=0.1)) for s in range(4)]
    t = bench.summarize(rows)
    assert t == {"4/awgn/-10": {"n": 4, "ok": 2, "false": 0, "snr_est": -10.0, "doppler_est": 0.1}}


@pytest.mark.skipif(not pulse.available(), reason="no PulseAudio server")
def test_pulse_loopback_roundtrip():
    with pulse.null_sink("glissando-test") as (sink, source):
        _, r = bench.trial_pulse((4, "awgn", 0, 3), sink, source)
    assert r["ok"] and not r["false"]
    assert r["rx_peak"] > 0.1 and r["rx_seconds"] > g.GEARS[4].duration
