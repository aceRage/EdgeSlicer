#!/bin/sh
# DRAFT entrypoint: Xvfb + hub + one supervised GUI slicer instance. Not a product; the spike's recipe in shell.
set -eu
DATA="${ES_DATA:-/data}"
APP=/opt/edgeslicer/AppRun
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe

mkdir -p "$DATA"
# A data dir that has never run needs a printer preset and the answers to the first-run questions, or the
# instance waits for a person (wizard, privacy prompt, TLS store prompt). The conf is the JSON the app writes,
# a newline and an MD5 line over the JSON text.
if [ ! -f "$DATA/EdgeSlicer.conf" ]; then
    body=$(cat /opt/hub/seed.conf.json)
    sum=$(printf '%s' "$body" | md5sum | cut -d' ' -f1 | tr 'a-f' 'A-F')
    printf '%s\n# MD5 checksum %s\n' "$body" "$sum" > "$DATA/EdgeSlicer.conf"
fi

# hub.json and instances/*.json carry PIDs: in a fresh container PID namespace they collide with live processes
# (the hub would see "another hub is running" and exit), so a start always begins without them. Defensive:
# the restart failure actually seen was the stale X lock below.
rm -f "$DATA/hub/hub.json" "$DATA"/hub/instances/*.json
# A restarted container keeps /tmp: a stale X lock makes Xvfb refuse :99 and the hub then dies for want of a display.
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99

Xvfb :99 -screen 0 1280x800x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
sleep 1

# --hub-phone: phone access on. The hub keeps its own switch and token in $DATA/hub/settings.json afterwards.
"$APP" --hub --datadir "$DATA" --hub-phone >/tmp/hub.out 2>&1 &
HUB=$!

json_field() { grep -o "\"$1\": *\"\?[^,\"}]*" "$DATA/hub/hub.json" | head -1 | sed 's/.*: *"\?//'; }
for i in $(seq 1 60); do [ -s "$DATA/hub/hub.json" ] && break; kill -0 "$HUB" 2>/dev/null || { cat /tmp/hub.out; exit 1; }; sleep 1; done
ADMIN=$(json_field admin_port); SECRET=$(json_field secret); PORT=$(json_field port); TOKEN=$(json_field token)
echo "hub up: port $PORT, phone link path /r/$TOKEN/ (host LAN address with --network host)"

admin() { curl -fs -m 10 -H "X-Hub-Secret: $SECRET" "http://127.0.0.1:$ADMIN$1"; }
instances() { admin /hub/instances | grep -o '"pid"' | wc -l; }

while kill -0 "$HUB" 2>/dev/null; do
    # No hidden instance: on Linux/GTK a never-shown window gets no GL context and slicing then crashes.
    # A visible one on Xvfb is fine. The hub never respawns a dead instance itself.
    if [ "$(instances || echo 0)" -lt 1 ]; then
        echo "no slicer instance registered, starting one"
        curl -fs -m 10 -X POST -H "X-Hub-Secret: $SECRET" -d x=1 "http://127.0.0.1:$ADMIN/hub/new" >/dev/null || true
    fi
    sleep 15
done
echo "hub exited"; exit 1
