"""python3 -m pytest prototype/  (the PulseAudio test skips without a server)."""
import numpy as np
import pytest

import bench
import glissando as g
import pulse


def test_sim_decodes_clean_signal():
    _, r = bench.trial_sim((4, "awgn", 0, 1, "pentatonic"))
    assert r["ok"] and not r["false"]


def test_sim_misses_buried_signal_without_false_decode():
    _, r = bench.trial_sim((4, "awgn", -40, 2, "pentatonic"))
    assert not r["ok"] and not r["false"]


def test_summary_layout_matches_sim_results():
    rows = [((4, "awgn", -10, s, "pentatonic"), dict(ok=s % 2 == 0, false=False, snr_est=-10.0, doppler_est=0.1)) for s in range(4)]
    t = bench.summarize(rows)
    assert t == {"4/awgn/-10": {"n": 4, "ok": 2, "false": 0, "snr_est": -10.0, "doppler_est": 0.1}}


@pytest.mark.parametrize("scale", list(g.SCALES))
def test_every_scale_round_trips_solo_and_duet(scale):
    for gi in (4, 5):
        _, r = bench.trial_sim((gi, "awgn", 0, 5, scale))
        assert r["ok"] and not r["false"]


@pytest.mark.parametrize("scale", list(g.SCALES))
def test_auto_scale_hears_every_scale(scale):
    for gi in (4, 5):
        _, r = bench.trial_sim((gi, "awgn", 0, 6, scale, "auto"))
        assert r["ok"] and not r["false"]


@pytest.mark.skipif(not pulse.available(), reason="no PulseAudio server")
def test_pulse_loopback_roundtrip():
    with pulse.null_sink("glissando-test") as (sink, source):
        _, r = bench.trial_pulse((4, "awgn", 0, 3, "pentatonic"), sink, source)
    assert r["ok"] and not r["false"]
    assert r["rx_peak"] > 0.1 and r["rx_seconds"] > g.GEARS[4].duration
