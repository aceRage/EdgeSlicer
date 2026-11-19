#!/bin/sh
# EdgeSlicer printer emulator (Mac): double-click to start it and open its control page.
# Fake printers on 127.0.0.x only. On a Mac, 127.0.0.11 and up need a loopback alias first:
#   sudo ifconfig lo0 alias 127.0.0.11 up      (the emulator prints the line for each printer)
cd "$(dirname "$0")" || exit 1
if ! command -v python3 >/dev/null 2>&1; then
  echo "Python 3 was not found. Install it from https://www.python.org/downloads/ and run this again."
  read -r _
  exit 1
fi
echo "Starting the EdgeSlicer printer emulator. Close this window (or press Ctrl+C) to stop it."
exec python3 ./emu.py serve "$@"
