# Hub: remote settings survive a quit, and nothing can shadow the hub's port

Date: 2026-09-11 · Branch: `fix/hub-remote-persist` (from `feat/ultra-preferences`) · Status: done, gated

Closes the two items left open in the Phase 0b review of
`2026-09-02-remote-access-design.md` ("Two things found on the way").

## Cause

**1. The settings were in the file that has to be deleted.**

`hub.json` is the hub's *runtime record*: pid, port, admin_port, secret - what a slicer instance,
the tray, or the hub page reads to find the live hub and prove itself to it. Its absence is how
those readers know no hub is running, so `HubServer::shutdown()` deletes it on a clean quit, and
that is correct.

The mistake was putting the hub's *settings* in the same file. `remote_on` and the tailnet
allow-list are things the person configured; they have nothing to do with finding a live process.
Deleting the runtime record deleted them too, so after a clean quit the next hub came up with
remote access off and an empty allow-list. Tailscale Serve, whose configuration is Tailscale's and
outlives us, kept forwarding to the hub port - and the hub, seeing a `Tailscale-User-Login` it no
longer had on any allow-list, answered 403. The phone stayed broken until somebody opened the hub
page and switched remote access on again. Section 1c of the design doc claimed these persisted;
they did not.

**2. A bind that succeeds where it should fail.**

On Windows, binding `127.0.0.1:13640` while another socket holds `0.0.0.0:13640` succeeds unless
the first socket asked for exclusivity. The more specific address wins for loopback traffic, so a
second hub started on another data dir would silently take over every loopback connection meant
for the first one - the slicer instances, the tray, the hub page, and anything Tailscale Serve
forwarded - while the first hub sat there apparently healthy. The acceptor set no
`SO_EXCLUSIVEADDRUSE`, and POSIX's `SO_REUSEADDR` (which the hub does set, and wants) does not
imply it.

## Fix

`src/slic3r/GUI/RemoteHub.cpp`:

- **The split already existed and is now complete.** `<datadir>/hub/settings.json` holds
  `remote_on`, `allowed_logins`, the phone token and old tokens, and the notification /
  Web Push / app-push settings; `hub.json` keeps the runtime record and is still removed by
  `shutdown()`. `write_hub_json()` writes both on every change; `start()` reads settings.json.
  Readers are unchanged: `hub.json` still carries `port`, `admin_port`, `pid`, `secret`, `token`,
  `phone`, `go2rtc_port` and `version`, so `RemoteAccess.cpp`, the hub page's `/hub/*` endpoints
  and the `snorca_hubtest` helpers (`run_hub_app.py`, `quit_sm.py`, `wait_sm.py`, `test_remote.py`
  and friends) keep their contract. `remote_on` / `allowed_logins` are still written into
  `hub.json` as well, which costs nothing and is what makes the migration below self-healing.
- **Migration.** A data dir whose last hub was killed rather than quit still has an old `hub.json`
  carrying the two keys. When there is no `settings.json`, `start()` imports them once and then
  writes settings.json, which wins from then on.
- **At start, if `remote_on` is true**, the hub re-points Tailscale Serve when it is not serving
  or is serving a different port - unchanged behaviour, but it now actually runs, because
  `remote_on` survives.
- **`SO_EXCLUSIVEADDRUSE` on both acceptors on Windows.** It was already on the main listener
  (`bind()`); it is now also on the loopback control plane (`bind_admin()`) - an ephemeral port is
  still a port. POSIX keeps `SO_REUSEADDR` exactly as before. The main listener's failure message
  now names the range and says another hub is probably already running; the existing behaviour of
  walking `13640..13659` and, failing that, returning false into the "hub already running"
  detection path is unchanged.
- **`SNORCA_TAILSCALE_EXE`** overrides the located `tailscale` binary. This is what lets a gate
  exercise the remote-access paths with a stand-in CLI instead of a real tailnet, and without
  touching this PC's own Serve configuration. Unset or empty keeps the previous behaviour
  (`%ProgramFiles%\Tailscale\tailscale.exe`, else `tailscale` on PATH).

## Tests

`snorca_hubtest\test_hub_persist.py`, wired into `gate_all.sh` as section `persist` and into
`gate_smart.sh`'s map for `RemoteHub.cpp`. It runs a hidden instance on its own scratch data dir
(`dd_hubpersist`) with `SNORCA_TAILSCALE_EXE` pointing at `fake_tailscale.bat` /
`fake_tailscale.py`, a stand-in that answers the four command lines the hub actually uses
(`status --json`, `serve status --json`, `serve --bg --https=443 http://127.0.0.1:<port>`,
`serve --https=443 off`), keeps Serve's state in a file the way Tailscale keeps its own, and logs
every invocation so the gate can prove what the hub asked for.

- **persist** - turn remote access on through `POST /hub/remote?on=1` and add a second allow-list
  login through the hub's own HTTP API; check the owner was seeded and the stand-in Serve points
  at the hub's port. Quit through the existing `/hub/quit` route, then assert `hub.json` is gone,
  `settings.json` is not, and it still carries `remote_on` plus both logins. Restart with the old
  port held by a squatter so the hub is pushed to a different one, and assert remote access is
  still on, the allow-list is intact, and the stand-in CLI saw a fresh `serve --bg` re-pointing
  Serve at the new port.
- **bind** - with the hub running, binding `127.0.0.1:<its port>` fails, and the hub still answers
  on loopback afterwards.
- **migrate** - a data dir seeded with only an old-style `hub.json` (remote_on plus an allow-list,
  no settings.json) comes up with both imported, and writes them through to settings.json.

The gate refuses to run against the real data dir, and never invokes the real `tailscale`.

Not covered by the gate: the true two-process shadowing race (a second full hub instance on a
second data dir) is approximated by a plain socket bind, which is the same kernel check the second
hub would fail; and the real Tailscale path stays covered by `test_remote.py`, which runs against
the owner's own tailnet and data dir.
