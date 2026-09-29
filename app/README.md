# Glissando app

This directory holds the Glissando desktop app: the console, the visi-scope
waterfall and the text chat window. It started as a copy of FreeDV's
freedv-gui and keeps its licence (`COPYING`, LGPL 2.1); the shared audio
backend in `backend/` is BSD 2-Clause (`backend/LICENSE`).

How to build it, run it and what each control does is in
[`docs/APP.md`](../docs/APP.md). The chat protocol is described in
[`doc/TEXT_MESSAGING.md`](doc/TEXT_MESSAGING.md), and the modem it drives
lives in [`modem/`](../modem).
