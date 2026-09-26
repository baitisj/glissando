"""HF channel simulation: AWGN in a 2500 Hz reference bandwidth, plus the
Watterson two-path Gaussian-scatter model with the ITU-R F.1487 / CCIR
test profiles (delay between paths, two-sided Doppler spread 2*sigma)."""
import numpy as np

from glissando import FS, analytic

PROFILES = {
    "awgn": None,
    "good": (0.5e-3, 0.1),      # CCIR/ITU mid-latitude quiet
    "moderate": (1.0e-3, 0.5),  # mid-latitude moderate
    "poor": (2.0e-3, 1.0),      # mid-latitude disturbed
    "flutter": (0.5e-3, 10.0),  # ITU-R F.1487 "flutter fading", auroral paths
}


def _fading_tap(n, spread, rng, rate=200.0):
    """Unit-power complex gain with a Gaussian Doppler spectrum, 2*sigma = spread."""
    m = int(np.ceil(n / FS * rate)) + 8
    w = rng.standard_normal(m) + 1j * rng.standard_normal(m)
    f = np.fft.fftfreq(m, 1 / rate)
    sigma = spread / 2
    g = np.fft.ifft(np.fft.fft(w) * np.exp(-f ** 2 / (4 * sigma ** 2)))  # |H|^2 is Gaussian, std sigma
    g /= np.sqrt(np.mean(np.abs(g) ** 2))
    t = np.arange(n) / FS * rate
    return np.interp(t, np.arange(m), g.real) + 1j * np.interp(t, np.arange(m), g.imag)


def hf_channel(x, profile, rng):
    p = PROFILES[profile]
    if p is None:
        return x
    delay, spread = p
    z = analytic(x)
    d = int(round(delay * FS))
    y = _fading_tap(len(z), spread, rng) * z
    y[d:] += _fading_tap(len(z), spread, rng)[d:] * z[:-d]
    return np.real(y) / np.sqrt(2)  # two equal-power paths, unit average power


def add_noise(x, snr_db, rng, signal_power):
    """SNR is signal power over noise power in 2500 Hz (the WSJT-X convention)."""
    n0 = signal_power / 10 ** (snr_db / 10) / 2500.0  # noise PSD, per Hz (one-sided)
    sigma = np.sqrt(n0 * FS / 2)
    return x + sigma * rng.standard_normal(len(x))


def freq_shift(x, df):
    z = analytic(x)
    return np.real(z * np.exp(2j * np.pi * df * np.arange(len(z)) / FS))
