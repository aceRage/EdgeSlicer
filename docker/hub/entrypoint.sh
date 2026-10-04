#!/bin/sh
# Xvfb + the EdgeSlicer hub in service mode. The hub itself starts the slicer instance, keeps it running and
# respawns it after a crash (with backoff); a data dir that has never run is seeded by the app (no wizard).
set -eu
DATA="${ES_DATA:-/data}"
APP=/opt/edgeslicer/AppRun
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe EDGESLICER_SERVICE=1

mkdir -p "$DATA"

# hub.json and instances/*.json carry PIDs: in a fresh container PID namespace they collide with live processes
# (the hub would see "another hub is running" and exit), so a start always begins without them. A restarted
# container also keeps /tmp: a stale X lock makes Xvfb refuse :99 and the hub then dies for want of a display.
rm -f "$DATA/hub/hub.json" "$DATA"/hub/instances/*.json
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99

Xvfb :99 -screen 0 1280x800x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
XVFB=$!
sleep 1
kill -0 "$XVFB" 2>/dev/null || { echo "Xvfb did not start:"; cat /tmp/xvfb.log; exit 1; }

# --hub-phone: phone access on (the hub keeps its own switch and token in $DATA/hub/settings.json afterwards).
# --hub-service: no prompts anywhere, minimal config seeded, instance supervised.
"$APP" --hub --hub-service --datadir "$DATA" --hub-phone >/tmp/hub.out 2>&1 &
HUB=$!

json_field() { grep -o "\"$1\": *\"\?[^,\"}]*" "$DATA/hub/hub.json" | head -1 | sed 's/.*: *"\?//'; }
for i in $(seq 1 60); do [ -s "$DATA/hub/hub.json" ] && break; kill -0 "$HUB" 2>/dev/null || { cat /tmp/hub.out; exit 1; }; sleep 1; done
PORT=$(json_field port); TOKEN=$(json_field token)
echo "hub up: port $PORT, phone link path /r/$TOKEN/"
if [ -n "${EDGESLICER_HUB_PUBLIC_HOST:-}" ]; then
    echo "advertised to phones as: http://${EDGESLICER_HUB_PUBLIC_HOST}/r/$TOKEN/ (host:port as given; the port defaults to the hub's)"
else
    echo "phone link host: the container's own address unless --network host is used; set EDGESLICER_HUB_PUBLIC_HOST=<lan address>[:port] otherwise"
fi

# Stay up while both are; either one dying ends the container so the restart policy can start it again.
while kill -0 "$HUB" 2>/dev/null; do
    kill -0 "$XVFB" 2>/dev/null || { echo "Xvfb exited"; cat /tmp/xvfb.log; kill "$HUB" 2>/dev/null || true; exit 1; }
    sleep 5
done
echo "hub exited"; exit 1
