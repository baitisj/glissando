"""PulseAudio transport for the test bench (Linux; also PipeWire's Pulse server).

Plays a buffer through a sink and records from a source at the same time,
with the stock `pacat` / `parec` / `pactl` command-line tools, so no Python
audio bindings are needed. Point sink and source at a rig's USB sound card
for over-the-air tests, or at a null sink and its monitor for a pure
software loopback (see `null_sink`).
"""
import contextlib
import shutil
import subprocess
import threading
import time

import numpy as np

TOOLS = ("pacat", "parec", "pactl")


def available():
    """True when the tools are installed and a PulseAudio server answers."""
    if not all(shutil.which(t) for t in TOOLS):
        return False
    return subprocess.run(["pactl", "info"], capture_output=True).returncode == 0


def list_devices():
    """(sinks, sources) as lists of names, from `pactl list short`."""
    out = {}
    for kind in ("sinks", "sources"):
        r = subprocess.run(["pactl", "list", "short", kind], capture_output=True, text=True, check=True)
        out[kind] = [line.split("\t")[1] for line in r.stdout.splitlines() if line.strip()]
    return out["sinks"], out["sources"]


@contextlib.contextmanager
def null_sink(name="glissando"):
    """Load a null sink for the duration of the block; yields (sink, source)."""
    r = subprocess.run(["pactl", "load-module", "module-null-sink", f"sink_name={name}",
                        f"sink_properties=device.description={name}"],
                       capture_output=True, text=True, check=True)
    module = r.stdout.strip()
    try:
        yield name, name + ".monitor"
    finally:
        subprocess.run(["pactl", "unload-module", module], capture_output=True)


def _stream_args(fs, device, latency_ms):
    a = ["--raw", "--format=float32le", f"--rate={fs}", "--channels=1", f"--latency-msec={latency_ms}"]
    if device:
        a.append(f"--device={device}")
    return a


def play_and_record(x, fs, sink=None, source=None, pre=0.5, tail=1.0, latency_ms=50):
    """Record from `source` while playing `x` (float, |x| <= 1) on `sink`.

    Recording starts `pre` seconds before playback and stops `tail` seconds
    after it, so the whole transmission lands somewhere inside the returned
    buffer; the receiver's time search finds it. Returns float64 samples.
    """
    x = np.asarray(x, dtype="<f4")
    rec = subprocess.Popen(["parec"] + _stream_args(fs, source, latency_ms),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    chunks = []
    reader = threading.Thread(target=lambda: chunks.extend(iter(lambda: rec.stdout.read(1 << 16), b"")))
    reader.start()
    try:
        time.sleep(pre)
        play = subprocess.run(["pacat", "--playback"] + _stream_args(fs, sink, latency_ms),
                              input=x.tobytes(), capture_output=True)
        if play.returncode != 0:
            raise RuntimeError("pacat failed: " + play.stderr.decode(errors="replace").strip())
        time.sleep(tail)
    finally:
        rec.terminate()
        reader.join()
        rec.wait()
    if rec.returncode not in (0, -15):
        raise RuntimeError("parec failed: " + rec.stderr.read().decode(errors="replace").strip())
    y = np.frombuffer(b"".join(chunks), dtype="<f4").astype(float)
    if len(y) < len(x):
        raise RuntimeError(f"recorded only {len(y)} of at least {len(x)} samples; is the source live?")
    return y
