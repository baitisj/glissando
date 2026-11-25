Glissando @VERSION@ (@HASH@) for macOS, @ARCH@
==============================================

Glissando is a weak-signal HF digital mode for keyboard chat that sounds
like a melody. https://github.com/baitisj/glissando

This build is for Macs running macOS 11 (Big Sur) or later: the zip named
arm64 is for Apple Silicon, the one named x86_64 for Intel. Unzip it, copy
Glissando.app to Applications and open it. Nothing else needs installing.

Unless it is signed with a Developer ID, macOS stops Glissando the first
time it opens, saying it could not verify it. Click Done, then open System
Settings > Privacy & Security, scroll down to Glissando and click Open
Anyway. macOS remembers the choice.

Glissando listens to your radio through its sound card, so macOS asks for
microphone access the first time you press Engage. If you said no, turn
Glissando on in System Settings > Privacy & Security > Microphone and
start it again.

Licences
--------

Glissando is LGPL 2.1 (the app, from freedv-gui) and MIT (the modem); see
COPYING.txt. wxWidgets, Hamlib, libsndfile, mpg123, LAME, libsamplerate
and codec2 are built into it; their licences are inside Glissando.app,
under Contents/Resources/licences. The source, and app/macos/make-zip.sh,
the recipe for rebuilding this zip with changed libraries, are at
https://github.com/baitisj/glissando.

The macOS build follows Tim Vetter's macOS port of Ragtime, Glissando's
sibling.
