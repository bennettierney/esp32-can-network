#!/bin/sh
# Launch PulseView on this Mac. The nightly build's own launcher points Python at a
# bundled 3.13 that does not match the 3.12 it links against, so set PYTHONHOME to the
# python.org 3.12 framework instead. Needs /usr/local/opt/python@3.12 symlinked to
# /Library/Frameworks/Python.framework and Homebrew dbus installed.
cd /Applications/pulseview.app/Contents/MacOS || exit 1
export PYTHONHOME=/Library/Frameworks/Python.framework/Versions/3.12
export SIGROK_FIRMWARE_PATH=../share/sigrok-firmware
export SIGROKDECODE_PATH=../share/libsigrokdecode/decoders
exec ./pulseview.real "$@"
