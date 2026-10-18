"""Runs the hub's Elegoo SDCP connection (Utils/ElegooSdcpMonitor) end to end against
tests/phone/mock_elegoo_sdcp.py on 127.0.0.1.

    python run_elegoo_sdcp_e2e.py --exe <build>/tests/slic3rutils/slic3rutils_tests.exe

Starts the mock on a free loopback port, runs the hidden Catch2 tag [.ElegooSdcpE2E] with the port
in ELEGOO_SDCP_MOCK_PORT, and stops the mock again (always, by the handle this script started it
with). No real printer is contacted. Exit code: the test executable's.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MOCK = os.path.join(HERE, "..", "phone", "mock_elegoo_sdcp.py")


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
    port = free_port()
    mock = subprocess.Popen([sys.executable, "-u", MOCK, "--port", str(port)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    try:
        line = mock.stdout.readline().strip()
        if not line.startswith("READY"):
            raise SystemExit("the mock did not start: " + line)
        env = dict(os.environ, ELEGOO_SDCP_MOCK_PORT=str(port))
        t0 = time.time()
        rc = subprocess.call([exe, "[ElegooSdcpE2E]", "--durations", "yes"] + rest, env=env)
        print("ElegooSdcpE2E exit %d after %.1f s" % (rc, time.time() - t0))
        return rc
    finally:
        mock.terminate()
        try:
            mock.wait(5)
        except subprocess.TimeoutExpired:
            mock.kill()


if __name__ == "__main__":
    sys.exit(main())
