"""Start the phone-controls worktree's slicer detached on the isolated dd_ctl data dir with phone
access on, for test_phone_control.py.

usage: run_control_app.py [file.3mf] [--visible] [--dryrun] [--datadir DIR] [--exe PATH]
  default: hidden, SNORCA_NEW_INSTANCE=1; --dryrun sets SNORCA_SEND_DRYRUN=1
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
EXE = opts.get("--exe", os.path.join(HERE, "inst_ctl", "EdgeSlicer.exe"))
DATADIR = opts.get("--datadir", os.path.join(HERE, "dd_ctl"))
FILE = os.path.abspath(args[0]) if args else os.path.join(HERE, "h2d_copy.3mf")
env = dict(os.environ)
env["SNORCA_PHONE_ACCESS"] = "testtoken12345"
env["SNORCA_NEW_INSTANCE"] = "1"
env["SNORCA_HIDDEN"] = "0" if "--visible" in sys.argv else "1"
if "--dryrun" in sys.argv:
    env["SNORCA_SEND_DRYRUN"] = "1"
p = subprocess.Popen([EXE, "--datadir", DATADIR, FILE], env=env,
                     creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP, close_fds=True)
print("started pid", p.pid, "exe", EXE, "datadir", DATADIR,
      "hidden" if env["SNORCA_HIDDEN"] == "1" else "visible", "DRY RUN" if "--dryrun" in sys.argv else "")
