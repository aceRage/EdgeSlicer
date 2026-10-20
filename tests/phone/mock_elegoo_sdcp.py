"""A stand-in Elegoo Centauri Carbon 1 (CC1) speaking SDCP V3.0.0 over its websocket, for the hub's
Elegoo backend (ElegooSdcpMonitor / ElegooSdcp) - the Catch2 [.ElegooSdcpE2E] cases and the gate.

Frames are shaped like the owner's capture (firmware V1.4.49, protocol V3.0.0); every id, name and
colour here is made up. Nothing here talks to a real printer: it binds 127.0.0.1 only.

  ws://127.0.0.1:<port>/websocket
    "ping"            -> "pong"
    Cmd 0             -> a status frame (Topic sdcp/status/...), the whole Status object
    Cmd 1             -> an attributes frame (FirmwareVersion V1.4.49, Capabilities, CameraStatus 1,
                         MaximumVideoStreamAllowed 4)
    Cmd 324           -> Ack 0 on sdcp/response, then the CANVAS payload on sdcp/response (two trays
                         loaded, one an untagged spool whose brand is dashes, tray 3 in the toolhead);
                         Ack 1 while /mock/canvas?attached=0
    Cmd 129 / 131 / 130  pause / resume / stop: Ack 0, then the status moves on (pausing -> paused,
                         resuming -> printing, stopping -> stopped) and is pushed
    Cmd 403           -> Ack 0; LightStatus.SecondLight and TargetFanSpeed.* applied and pushed
    Cmd 386           -> Ack 0 with VideoUrl "127.0.0.1:<video port>/video" (no scheme, as on V1.4.49)
  --video-port V: an MJPEG camera on http://127.0.0.1:V/video (multipart JPEG frames), which turns
    the first /mock/video?refuse=N requests away by closing them, as a camera that needs Cmd 386
    while printing, a status push every second with the progress moving on
  HTTP on the same port (the test's control surface):
    GET /mock/state                 {"connections_total", "open", "max_open", "commands": [[cmd, data]...],
                                     "pings", "state": {...}}
    GET /mock/reset                 idle, nothing recorded
    GET /mock/print?state=printing|paused|idle|complete|preheating
    GET /mock/canvas?attached=1|0   AmsConnectStatus
    GET /mock/drop                  close every websocket now
    GET /mock/refuse?on=1|0         decline new websocket handshakes (HTTP 403), as a full printer does
    GET /mock/limit?n=4             decline a handshake beyond n open clients (default 4)
    GET /mock/mute?on=1|0           stop answering anything (and stop pushing): a printer that hangs
    GET /mock/video?refuse=N        the camera turns the next N stream requests away
    POST /mock/hook                 a notification webhook: counted in /mock/state "hooks"

usage: mock_elegoo_sdcp.py --port P [--video-port V]   (prints "READY <port>", runs until killed)
"""
import argparse
import base64
import hashlib
import json
import socket
import socketserver
import struct
import sys
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
BOARD = "MOCKBOARD00000001"
LOCK = threading.Lock()
SOCKETS = {}  # socket -> send function
STATS = {"connections_total": 0, "open": 0, "max_open": 0, "commands": [], "pings": 0, "hooks": 0}
FLAGS = {"refuse": False, "limit": 4, "mute": False, "video_refuse": 0, "video_port": 0}
VIDEO = {"requests": 0, "streaming": 0}


def fresh_state():
    return {
        "machine": 0, "print": 0, "layer": 0, "total": 0, "ticks": 0, "total_ticks": 0, "progress": 0,
        "file": "", "nozzle": 26.0, "nozzle_t": 0, "bed": 24.0, "bed_t": 0, "box": 25.0, "box_t": 0,
        "model_fan": 0, "aux_fan": 0, "box_fan": 0, "light": 1, "rgb": [255, 255, 255], "ams": 1,
    }


STATE = fresh_state()


def set_print(kind):
    s = STATE
    if kind == "printing":
        s.update(machine=1, print=13, layer=12, total=150, ticks=600, total_ticks=3600, progress=17,
                 file="Cube_PLA_mock.gcode", nozzle=220.0, nozzle_t=220, bed=60.0, bed_t=60, model_fan=100, aux_fan=50,
                 box_fan=30)
    elif kind == "preheating":
        s.update(machine=1, print=16, layer=0, total=150, ticks=20, total_ticks=3600, progress=0,
                 file="Cube_PLA_mock.gcode", nozzle=150.0, nozzle_t=220, bed=40.0, bed_t=60)
    elif kind == "paused":
        set_print("printing")
        s.update(print=6)
    elif kind == "complete":
        s.update(machine=0, print=9, progress=100, ticks=3600, layer=150, nozzle_t=0, bed_t=0, model_fan=0)
    else:  # idle
        ams = s.get("ams", 1)
        s.update(fresh_state())
        s["ams"] = ams


def status_frame():
    s = STATE
    return {
        "Status": {
            "CurrentStatus": [s["machine"]],
            "PreviousStatus": 0,
            "TempOfNozzle": s["nozzle"], "TempTargetNozzle": s["nozzle_t"],
            "TempOfHotbed": s["bed"], "TempTargetHotbed": s["bed_t"],
            "TempOfBox": s["box"], "TempTargetBox": s["box_t"],
            "CurrenCoord": "120.00,110.00,3.20",
            "CurrentFanSpeed": {"ModelFan": s["model_fan"], "AuxiliaryFan": s["aux_fan"], "BoxFan": s["box_fan"]},
            "ZOffset": 0.0,
            "LightStatus": {"SecondLight": s["light"], "RgbLight": s["rgb"]},
            "TimeLapseStatus": 0,
            "AmsConnectStatus": s["ams"],
            "PrintInfo": {
                "Status": s["print"], "CurrentLayer": s["layer"], "TotalLayer": s["total"],
                "CurrentTicks": s["ticks"], "TotalTicks": s["total_ticks"], "Progress": s["progress"],
                "Filename": s["file"], "ErrorNumber": 0, "TaskId": "mock-task-0001", "PrintSpeedPct": 100,
            },
        },
        "MainboardID": BOARD,
        "TimeStamp": int(time.time()),
        "Topic": "sdcp/status/" + BOARD,
    }


def attributes_frame():
    return {
        "Attributes": {
            "Name": "Mock CC1", "MachineName": "Centauri Carbon", "BrandName": "ELEGOO",
            "ProtocolVersion": "V3.0.0", "FirmwareVersion": "V1.4.49", "MainboardID": BOARD,
            "Capabilities": ["FILE_TRANSFER", "PRINT_CONTROL", "VIDEO_STREAM"],
            "CameraStatus": 1, "MaximumVideoStreamAllowed": 4, "NumberOfVideoStreamConnected": 0,
        },
        "MainboardID": BOARD,
        "TimeStamp": int(time.time()),
        "Topic": "sdcp/attributes/" + BOARD,
    }


def response(cmd, request_id, data):
    return {
        "Id": "",
        "Data": {"Cmd": cmd, "Data": data, "RequestID": request_id, "MainboardID": BOARD, "TimeStamp": int(time.time())},
        "Topic": "sdcp/response/" + BOARD,
    }


def canvas_payload():
    dashes = "—— ——"
    return {
        "Ack": 0,
        "active_canvas_id": 0,
        "active_tray_id": 3,
        "auto_refill": 0,
        "canvas_list": [{
            "canvas_id": 0, "connected": 1,
            "tray_list": [
                {"tray_id": 0, "brand": "ELEGOO", "filament_type": "PLA", "filament_name": "ELEGOO PLA",
                 "filament_code": "0x0001", "filament_color": "#1E7A3C", "min_nozzle_temp": 190, "max_nozzle_temp": 230,
                 "status": 1},
                {"tray_id": 1, "brand": "", "filament_type": "", "filament_name": "", "filament_code": "0x00000",
                 "filament_color": "", "min_nozzle_temp": 0, "max_nozzle_temp": 0, "status": 0},
                {"tray_id": 2, "brand": "", "filament_type": "", "filament_name": "", "filament_code": "0x00000",
                 "filament_color": "", "min_nozzle_temp": 0, "max_nozzle_temp": 0, "status": 0},
                {"tray_id": 3, "brand": dashes, "filament_type": "PLA", "filament_name": "PLA PRO",
                 "filament_code": "0x00000", "filament_color": "#D02A2A", "min_nozzle_temp": 190, "max_nozzle_temp": 230,
                 "status": 2},
            ],
        }],
    }


def broadcast(obj):
    text = json.dumps(obj).encode()
    with LOCK:
        senders = list(SOCKETS.values())
    for send in senders:
        try:
            send(1, text)
        except OSError:
            pass


def http_reply(wfile, code, obj):
    body = json.dumps(obj).encode()
    reason = {200: "OK", 403: "Forbidden", 404: "Not Found"}.get(code, "OK")
    head = "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % (
        code, reason, len(body))
    wfile.write(head.encode() + body)


def send_frame(wfile, opcode, data):
    head = bytes([0x80 | opcode])
    n = len(data)
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack(">H", n)
    else:
        head += bytes([127]) + struct.pack(">Q", n)
    wfile.write(head + data)
    wfile.flush()


def read_exact(rfile, n):
    data = b""
    while len(data) < n:
        chunk = rfile.read(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def read_frame(rfile):
    b1, b2 = read_exact(rfile, 2)
    opcode = b1 & 0x0F
    n = b2 & 0x7F
    if n == 126:
        n = struct.unpack(">H", read_exact(rfile, 2))[0]
    elif n == 127:
        n = struct.unpack(">Q", read_exact(rfile, 8))[0]
    mask = read_exact(rfile, 4) if b2 & 0x80 else b"\0\0\0\0"
    data = bytearray(read_exact(rfile, n))
    for i in range(n):
        data[i] ^= mask[i % 4]
    return opcode, bytes(data)


def query_of(path):
    out = {}
    if "?" in path:
        for kv in path.split("?", 1)[1].split("&"):
            k, _, v = kv.partition("=")
            out[k] = v
    return out


def later(seconds, fn):
    t = threading.Timer(seconds, fn)
    t.daemon = True
    t.start()


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline().decode("latin-1").strip()
        if not line:
            return
        parts = line.split()
        path = parts[1] if len(parts) > 1 else "/"
        headers = {}
        while True:
            h = self.rfile.readline().decode("latin-1")
            if h in ("\r\n", "\n", ""):
                break
            k, _, v = h.partition(":")
            headers[k.strip().lower()] = v.strip()
        base = path.split("?")[0]
        q = query_of(path)
        if base == "/websocket" and headers.get("upgrade", "").lower() == "websocket":
            with LOCK:
                full = STATS["open"] >= FLAGS["limit"]
            if FLAGS["refuse"] or full:
                return http_reply(self.wfile, 403, {"error": "too many clients"})
            accept = base64.b64encode(hashlib.sha1((headers.get("sec-websocket-key", "") + GUID).encode()).digest()).decode()
            self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
            self.wfile.flush()
            return self.websocket()
        if base == "/mock/hook":
            # A notification destination (the gate's webhook): counted, body read and dropped.
            n = int(headers.get("content-length", "0") or 0)
            if n > 0:
                self.rfile.read(n)
            with LOCK:
                STATS["hooks"] += 1
            return http_reply(self.wfile, 200, {"ok": True})
        if base == "/mock/state":
            with LOCK:
                return http_reply(self.wfile, 200, dict(STATS, state=dict(STATE), video=dict(VIDEO)))
        if base == "/mock/reset":
            with LOCK:
                STATS.update(commands=[], pings=0, hooks=0, connections_total=STATS["open"], max_open=STATS["open"])
                FLAGS.update(refuse=False, limit=4, mute=False)
            STATE.update(fresh_state())
            return http_reply(self.wfile, 200, {"reset": True})
        if base == "/mock/print":
            set_print(q.get("state", "idle"))
            broadcast(status_frame())
            return http_reply(self.wfile, 200, {"state": dict(STATE)})
        if base == "/mock/canvas":
            STATE["ams"] = 1 if q.get("attached", "1") == "1" else 0
            broadcast(status_frame())
            return http_reply(self.wfile, 200, {"ams": STATE["ams"]})
        if base == "/mock/drop":
            with LOCK:
                socks = list(SOCKETS.keys())
            for s in socks:
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            return http_reply(self.wfile, 200, {"dropped": len(socks)})
        if base == "/mock/refuse":
            FLAGS["refuse"] = q.get("on", "1") == "1"
            return http_reply(self.wfile, 200, {"refuse": FLAGS["refuse"]})
        if base == "/mock/limit":
            FLAGS["limit"] = int(q.get("n", "4"))
            return http_reply(self.wfile, 200, {"limit": FLAGS["limit"]})
        if base == "/mock/video":
            FLAGS["video_refuse"] = int(q.get("refuse", "0"))
            return http_reply(self.wfile, 200, {"video_refuse": FLAGS["video_refuse"]})
        if base == "/mock/mute":
            FLAGS["mute"] = q.get("on", "1") == "1"
            return http_reply(self.wfile, 200, {"mute": FLAGS["mute"]})
        http_reply(self.wfile, 404, {"error": "not found"})

    def websocket(self):
        wlock = threading.Lock()

        def send(opcode, payload):
            with wlock:
                send_frame(self.wfile, opcode, payload)

        with LOCK:
            SOCKETS[self.request] = send
            STATS["connections_total"] += 1
            STATS["open"] += 1
            STATS["max_open"] = max(STATS["max_open"], STATS["open"])

        def send_json(obj):
            send(1, json.dumps(obj).encode())

        try:
            while True:
                opcode, data = read_frame(self.rfile)
                if opcode == 8:
                    try:
                        send(8, data[:2])
                    except OSError:
                        pass
                    return
                if opcode == 9:
                    send(10, data)
                    continue
                if opcode != 1:
                    continue
                if FLAGS["mute"]:
                    continue
                text = data.decode("utf-8", "replace")
                if text.strip() == "ping":
                    with LOCK:
                        STATS["pings"] += 1
                    send(1, b"pong")
                    continue
                try:
                    req = json.loads(text)
                except ValueError:
                    continue
                d = req.get("Data", {})
                cmd = d.get("Cmd")
                rid = d.get("RequestID", "")
                body = d.get("Data", {})
                with LOCK:
                    STATS["commands"].append([cmd, body])
                if cmd == 0:
                    send_json(status_frame())
                elif cmd == 1:
                    send_json(attributes_frame())
                elif cmd == 324:
                    if STATE["ams"] != 1:
                        send_json(response(324, rid, {"Ack": 1}))
                    else:
                        send_json(response(324, rid, {"Ack": 0}))
                        send_json(response(324, rid, canvas_payload()))
                elif cmd in (129, 130, 131):
                    ok = (cmd == 129 and STATE["print"] == 13) or (cmd == 131 and STATE["print"] == 6) or \
                         (cmd == 130 and STATE["machine"] == 1)
                    send_json(response(cmd, rid, {"Ack": 0 if ok else 1}))
                    if ok:
                        step, final = {129: (5, 6), 131: (12, 13), 130: (7, 8)}[cmd]
                        STATE["print"] = step
                        broadcast(status_frame())

                        def finish(final=final, cmd=cmd):
                            STATE["print"] = final
                            if cmd == 130:
                                STATE["machine"] = 0
                            broadcast(status_frame())
                        later(1.0, finish)
                elif cmd == 386:
                    data = {"Ack": 0}
                    if FLAGS["video_port"]:
                        data["VideoUrl"] = "127.0.0.1:%d/video" % FLAGS["video_port"]
                    send_json(response(386, rid, data))
                elif cmd == 403:
                    if isinstance(body.get("LightStatus"), dict):
                        STATE["light"] = int(body["LightStatus"].get("SecondLight", STATE["light"]))
                        if isinstance(body["LightStatus"].get("RgbLight"), list):
                            STATE["rgb"] = body["LightStatus"]["RgbLight"]
                    fans = body.get("TargetFanSpeed")
                    if isinstance(fans, dict):
                        for k, key in (("ModelFan", "model_fan"), ("AuxiliaryFan", "aux_fan"), ("BoxFan", "box_fan")):
                            if k in fans:
                                STATE[key] = int(fans[k])
                    send_json(response(403, rid, {"Ack": 0}))
                    broadcast(status_frame())
                else:
                    send_json(response(cmd if cmd is not None else -1, rid, {"Ack": 2}))
        except (EOFError, ConnectionError, OSError, ValueError):
            return
        finally:
            with LOCK:
                SOCKETS.pop(self.request, None)
                STATS["open"] -= 1


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


class VideoHandler(socketserver.StreamRequestHandler):
    def handle(self):
        while True:
            h = self.rfile.readline()
            if h in (b"\r\n", b"\n", b""):
                break
        with LOCK:
            VIDEO["requests"] += 1
            refuse = FLAGS["video_refuse"] > 0
            if refuse:
                FLAGS["video_refuse"] -= 1
        if refuse:
            return
        self.wfile.write(b"HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=f\r\n\r\n")
        jpeg = b"\xff\xd8" + b"\x11" * 100 + b"\xff\xd9"
        with LOCK:
            VIDEO["streaming"] += 1
        try:
            while True:
                self.wfile.write(b"--f\r\nContent-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n")
                self.wfile.flush()
                time.sleep(0.05)
        except OSError:
            pass
        finally:
            with LOCK:
                VIDEO["streaming"] -= 1


def pusher():
    # A printing printer pushes its status on every change; here, once a second.
    while True:
        time.sleep(1.0)
        if FLAGS["mute"] or STATE["machine"] != 1 or STATE["print"] != 13:
            continue
        STATE["ticks"] = min(STATE["total_ticks"], STATE["ticks"] + 1)
        if STATE["ticks"] % 10 == 0:
            STATE["layer"] = min(STATE["total"], STATE["layer"] + 1)
            STATE["progress"] = min(99, STATE["progress"] + 1)
        broadcast(status_frame())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--video-port", type=int, default=0)
    args = ap.parse_args()
    srv = Server(("127.0.0.1", args.port), Handler)
    if args.video_port:
        FLAGS["video_port"] = args.video_port
        vsrv = Server(("127.0.0.1", args.video_port), VideoHandler)
        threading.Thread(target=vsrv.serve_forever, daemon=True).start()
    threading.Thread(target=pusher, daemon=True).start()
    print("READY %d" % args.port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
