#!/bin/bash
# Smoke test for an edgeslicer-hub image (CI and local): start it, wait for the instance the hub starts,
# slice a model through the hub, check the thumbnail, check go2rtc and the bundled ffmpeg, kill the instance
# and watch the hub respawn it. Mocks only; the container has no printers. Removes its container.
#
#   docker/hub/smoke.sh <image> [obj-file]        e.g. docker/hub/smoke.sh edgeslicer-hub-test tests/data/frog_legs.obj
set -u
IMAGE="${1:?usage: smoke.sh <image> [obj-file]}"
OBJ="${2:-tests/data/frog_legs.obj}"
C="${SMOKE_NAME:-edgeslicer-hub-test-smoke}"
PORT="${SMOKE_PORT:-13651}"
fail=0
ok()   { echo "PASS  $*"; }
bad()  { echo "FAIL  $*"; fail=1; }
trap 'docker rm -f "$C" >/dev/null 2>&1' EXIT
docker rm -f "$C" >/dev/null 2>&1

docker run -d --name "$C" -p "$PORT":13640 -e EDGESLICER_HUB_PUBLIC_HOST="192.0.2.10:$PORT" "$IMAGE" >/dev/null || { echo "could not start the image"; exit 1; }
TOK=""
for i in $(seq 1 60); do
    TOK=$(docker exec "$C" sh -c "grep -o '\"token\": *\"[^\"]*' /data/hub/hub.json 2>/dev/null | sed 's/.*\"//'")
    [ -n "$TOK" ] && break
    docker ps -q -f name="$C" | grep -q . || { echo "container exited:"; docker logs "$C" 2>&1 | tail -20; exit 1; }
    sleep 2
done
[ -n "$TOK" ] && ok "hub up, token $TOK" || { bad "hub did not come up"; docker logs "$C" 2>&1 | tail -20; exit 1; }
B="http://127.0.0.1:$PORT/r/$TOK"

PID=""
for i in $(seq 1 60); do
    PID=$(curl -s -m 10 "$B/api/instances" | grep -o '"pid":[0-9]*' | head -1 | cut -d: -f2)
    [ -n "$PID" ] && break
    sleep 2
done
[ -n "$PID" ] && ok "instance $PID registered (started by the hub)" || { bad "no instance"; docker logs "$C" 2>&1 | tail -20; }

curl -s -m 10 "$B/summary" | grep -q '"lan_url":"http://192.0.2.10:'"$PORT"'/r/' && ok "advertised host/port from EDGESLICER_HUB_PUBLIC_HOST" || bad "lan_url does not carry the public host"

if [ -n "$PID" ] && [ -f "$OBJ" ]; then
    curl -s -m 180 -X POST -H "X-File-Name: $(basename "$OBJ")" --data-binary @"$OBJ" "$B/i/$PID/open" | grep -q '"objects"' && ok "model opened" || bad "model did not open"
    curl -s -m 30 -X POST -d x=1 "$B/i/$PID/api/slice?plate=0" | grep -q '"job"' && ok "slice started" || bad "slice did not start"
    sliced=0
    for i in $(seq 1 90); do
        curl -s -m 10 "$B/i/$PID/api/plates/0/preview/status" | grep -q '"sliced":true' && { sliced=1; break; }
        sleep 2
    done
    [ $sliced = 1 ] && ok "sliced" || bad "not sliced in 3 minutes"
    size=$(curl -s -m 60 -o /dev/null -w '%{http_code} %{size_download}' "$B/i/$PID/api/plates/0/thumbnail.png")
    case "$size" in "200 "[0-9][0-9][0-9][0-9]*) ok "thumbnail $size";; *) bad "thumbnail $size";; esac
fi

R=/opt/edgeslicer/resources/tools/go2rtc
docker exec "$C" sh -c "pgrep -x go2rtc >/dev/null" && ok "go2rtc running" || bad "go2rtc not running"
docker exec "$C" sh -c "$R/ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libopenh264" && ok "bundled ffmpeg has libopenh264" || bad "bundled ffmpeg missing or without libopenh264"
docker exec "$C" sh -c "$R/ffmpeg -hide_banner -version | grep -q -e '--enable-version3' && ! $R/ffmpeg -hide_banner -version | grep -q -e '--enable-gpl'" && ok "ffmpeg is the LGPL build (version3, no --enable-gpl)" || bad "ffmpeg is not the LGPL build"
docker exec "$C" sh -c "test -s $R/FFMPEG-LICENSE.txt && test -s $R/FFMPEG-NOTICE-LINUX.txt && test -s $R/go2rtc-LICENSE.txt" && ok "licence and notice files present" || bad "licence/notice files missing"
docker exec "$C" sh -c "grep -q 'quality variants on via' /data/log/hub.log.0" && ok "camera quality variants on" || bad "camera quality variants off"

if [ -n "$PID" ]; then
    docker exec "$C" sh -c "kill -9 $PID"
    NEW=""
    for i in $(seq 1 40); do
        NEW=$(curl -s -m 10 "$B/api/instances" | grep -o '"pid":[0-9]*' | head -1 | cut -d: -f2)
        [ -n "$NEW" ] && [ "$NEW" != "$PID" ] && break
        NEW=""
        sleep 2
    done
    [ -n "$NEW" ] && ok "instance killed and respawned as $NEW" || bad "no respawn after kill -9"
fi

sleep 20
docker stats --no-stream --format 'stats: cpu={{.CPUPerc}} mem={{.MemUsage}}' "$C"
docker exec "$C" uname -m | sed 's/^/arch: /'
[ $fail = 0 ] && echo "SMOKE OK" || { echo "SMOKE FAILED"; docker logs "$C" 2>&1 | tail -30; }
exit $fail
