"""Safety rails (design section 3.3). Not optional, not configurable away.

1. A printer binds to loopback unless its entry says "lan": true, and then only to a private
   (RFC 1918) or link-local IPv4 address this PC actually owns. 0.0.0.0, public addresses and host
   names are refused.
2. The control page and API always bind to 127.0.0.1.
3. The emulator is a server only. Nothing here opens an outbound connection.
"""
import ipaddress
import socket
import subprocess
import sys

CONTROL_HOST = "127.0.0.1"


class BindRefused(ValueError):
    pass


def is_loopback(host):
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def is_private_lan(host):
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        return False
    if ip.version != 4 or ip.is_loopback or ip.is_unspecified or ip.is_multicast:
        return False
    return any(ip in ipaddress.ip_network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "169.254.0.0/16"))


def check_bind(host, lan=False):
    """The address a printer may listen on, or BindRefused with the reason."""
    if not isinstance(host, str) or not host:
        raise BindRefused("no bind address")
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        raise BindRefused("'%s' is not an IP address; the emulator binds to addresses, never host names" % host)
    if ip.is_unspecified:
        raise BindRefused("%s would listen on every network; pick 127.0.0.x (or one LAN address with lan: true)" % host)
    if ip.is_loopback:
        return host
    if not lan:
        raise BindRefused("%s is not loopback; set \"lan\": true on this printer to bind to a LAN address" % host)
    if not is_private_lan(host):
        raise BindRefused("%s is not a private (RFC 1918) or link-local address; LAN binds are limited to those" % host)
    return host


def mac_alias_command(host):
    """The one command a Mac needs before a printer can listen on `host` (only 127.0.0.1 exists
    there until an alias is added; it lasts until the Mac restarts). "" when none is needed."""
    if is_loopback(host) and host != "127.0.0.1":
        return "sudo ifconfig lo0 alias %s up" % host
    return ""


def mac_alias_hint(host):
    if sys.platform == "darwin" and is_loopback(host) and host != "127.0.0.1":
        return " On a Mac only 127.0.0.1 exists until you add the alias: sudo ifconfig lo0 alias %s up" % host
    return ""


def who_holds(port):
    """Best effort, read-only: which process listens on `port` (Windows netstat / tasklist)."""
    if sys.platform != "win32":
        return ""
    try:
        out = subprocess.run(["netstat", "-ano", "-p", "TCP"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return ""
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[3].upper() == "LISTENING" and parts[1].rsplit(":", 1)[-1] == str(port):
            pid = parts[4]
            name = ""
            try:
                t = subprocess.run(["tasklist", "/FI", "PID eq %s" % pid, "/FO", "CSV", "/NH"], capture_output=True,
                                   text=True, timeout=10).stdout.strip()
                if t.startswith('"'):
                    name = t.split('","', 1)[0].strip('"')
            except (OSError, subprocess.SubprocessError):
                pass
            return "%s (pid %s) on %s" % (name or "a process", pid, parts[1])
    return ""


def port_free(host, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()
