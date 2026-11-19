"""Uploaded files: kept on disk under <home>/uploads/<printer>/, capped (default 200 MB, oldest
removed first), never executed or parsed beyond their size and hash (design 3.3, rail 6)."""
import hashlib
import os
import threading
import time

DEFAULT_CAP = 200 * 1024 * 1024


def safe_name(name):
    name = (name or "").replace("\\", "/").split("/")[-1]
    name = "".join(c if (c.isalnum() or c in "-_.() ") else "_" for c in name).strip(" .")
    return name or "upload.gcode"


class UploadStore:
    def __init__(self, root, cap_bytes=DEFAULT_CAP):
        self.root = root
        self.cap = cap_bytes
        self.lock = threading.Lock()
        if root:
            os.makedirs(root, exist_ok=True)

    def put(self, printer_id, name, data):
        info = {"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "stored_at": time.time()}
        if not self.root:
            return info
        d = os.path.join(self.root, safe_name(printer_id))
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, safe_name(name))
        with self.lock:
            with open(path, "wb") as f:
                f.write(data)
            self._enforce_cap()
        info["stored"] = path
        return info

    def _enforce_cap(self):
        files = []
        for dirpath, _dirs, names in os.walk(self.root):
            for n in names:
                p = os.path.join(dirpath, n)
                try:
                    st = os.stat(p)
                    files.append((st.st_mtime, st.st_size, p))
                except OSError:
                    pass
        total = sum(f[1] for f in files)
        for _m, size, p in sorted(files):
            if total <= self.cap:
                break
            try:
                os.remove(p)
                total -= size
            except OSError:
                pass
