"""Runs the U1 local-first route (GUI/U1LanRoute) end to end against mock_u1_mqtt.py on 127.0.0.1.

    python run_u1_local_first_e2e.py --exe <build>/tests/slic3rutils/Release/slic3rutils_tests.exe

Starts the mock (a fake Snapmaker cloud broker, a fake U1 pairing broker and LAN broker, and a
control port) on free loopback ports, runs the hidden Catch2 tag [U1LocalFirstE2E] with the ports
in U1MOCK_PORTS ("<cloud>:<lan>:<pairing>:<control>"), and stops the mock again (always, by the
handle this script started it with). No real printer or cloud is contacted.
Exit code: the test executable's.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, help="slic3rutils_tests.exe")
    args, rest = ap.parse_known_args()
    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        raise SystemExit("no test executable at " + exe)
    ports = [free_port() for _ in range(4)]
    mock = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "mock_u1_mqtt.py"),
                             "--cloud", str(ports[0]), "--lan", str(ports[1]),
                             "--pairing", str(ports[2]), "--control", str(ports[3])],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        line = mock.stdout.readline().strip()
        if not line.startswith("READY"):
            raise SystemExit("the mock did not start: %r" % line)
        env = dict(os.environ, U1MOCK_PORTS=":".join(str(p) for p in ports))
        print("mock: " + env["U1MOCK_PORTS"], flush=True)
        started = time.time()
        rc = subprocess.call([exe, "[U1LocalFirstE2E]", "--durations", "yes"] + rest, env=env)
        print("E2E_EXIT=%d (%.0f s)" % (rc, time.time() - started), flush=True)
        return rc
    finally:
        mock.kill()
        mock.wait()


if __name__ == "__main__":
    sys.exit(main())
