#!/usr/bin/env python3
"""Reject a macOS Glissando.app that depends on the machine it was built on.

Adapted from Tim Vetter's bundle check for Ragtime
(github.com/baitisj/ragtime, packaging/macos/check-bundle.py).
"""

import argparse
import platform
import plistlib
import re
import subprocess
from pathlib import Path


# The Mac it runs on: make-zip.sh builds for that one.
ARCH = platform.machine()
LICENCES = ('wxWidgets', 'Hamlib', 'libsndfile', 'mpg123', 'LAME',
            'libsamplerate', 'codec2')


def output(*command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)


def version(value):
    parts = tuple(map(int, value.split('.')))
    return parts + (0,) * (3 - len(parts))


def check(app):
    contents = app / 'Contents'
    with (contents / 'Info.plist').open('rb') as source:
        info = plistlib.load(source)
    minimum_macos = info.get('LSMinimumSystemVersion', '')
    if not re.fullmatch(r'\d+(\.\d+){0,2}', minimum_macos):
        raise ValueError('Info.plist names no LSMinimumSystemVersion')
    if not info.get('NSMicrophoneUsageDescription'):
        raise ValueError('Missing microphone usage description')
    if not (contents / 'Resources' / info.get('CFBundleIconFile', '')).is_file():
        raise ValueError('Missing app icon')
    for name in LICENCES:
        if not (contents / 'Resources' / 'licences' / f'{name}.txt').is_file():
            raise ValueError(f'Missing licence: {name}')
    executable = contents / 'MacOS' / info['CFBundleExecutable']
    if not executable.is_file():
        raise ValueError('Missing app executable')

    binaries = [executable] + sorted(contents.rglob('*.dylib'))
    for binary in binaries:
        if not binary.resolve().is_relative_to(app.resolve()):
            raise ValueError(f'Library symlink leaves the bundle: {binary.name}')
        if output('lipo', '-archs', str(binary)).strip() != ARCH:
            raise ValueError(f'Expected an {ARCH} binary: {binary.name}')
        loads = output('otool', '-l', str(binary))
        minimum = re.search(r'\bminos\s+(\S+)', loads)
        if minimum is None or version(minimum[1]) > version(minimum_macos):
            raise ValueError(f'{binary.name} needs a newer macOS than {minimum_macos}')
        for path in re.findall(r'\bpath (.*?) \(offset \d+\)', loads):
            if not path.startswith(('@loader_path', '@executable_path')):
                raise ValueError(f'Rpath outside the bundle in {binary.name}: {path}')
        for line in output('otool', '-L', str(binary)).splitlines()[1:]:
            dependency = line.strip().split(' (', 1)[0]
            if dependency.startswith(('/System/Library/', '/usr/lib/')):
                continue
            if dependency.startswith('@rpath/'):
                target = contents / 'Frameworks' / dependency.removeprefix('@rpath/')
            elif dependency.startswith('@loader_path/'):
                target = binary.parent / dependency.removeprefix('@loader_path/')
            elif dependency.startswith('@executable_path/'):
                target = executable.parent / dependency.removeprefix('@executable_path/')
            else:
                raise ValueError(f'Library outside the bundle in {binary.name}: {dependency}')
            if not target.resolve().is_relative_to(app.resolve()) or not target.is_file():
                raise ValueError(f'Unresolved library in {binary.name}: {dependency}')
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
    print(f'Bundle checked: {ARCH}, macOS {minimum_macos} or later, licences, '
          'no libraries from outside macOS.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    args = parser.parse_args()
    try:
        check(args.app)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Bundle check failed: {error}\n')
