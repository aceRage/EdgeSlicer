# Legacy gate mocks (copied, not rewritten)

Copied unchanged on 2026-10-20 from `%LOCALAPPDATA%\Temp\snorca_hubtest\`, where a temp cleaner
could wipe them (printer emulator design, D11). The originals stay where they are: the
`snorca_hubtest/gate_*.sh` scripts still reference them by that path.

| File | What it is |
|---|---|
| `mock_printhost.py` | OctoPrint subset plus U1 `objects/query` answers (fw 1.5.2); records uploads, scripts, starts. Trap: `/mock/state` has no print state, and reset leaves the state `complete`. |
| `mock_moonraker.py` | `print_stats` plus pause / resume / cancel. Superseded by the emulator's Moonraker personality. |
| `mock_prusalink.py` | PrusaLink `/api/version`, `/api/v1/status`, `/api/v1/job` with digest / API-key auth. Becomes the emulator's PrusaLink personality in E3. |
| `run_control_app.py`, `wait_sm.py`, `quit_sm.py` | Start a hidden test instance on a `--datadir` with the test token, wait for it, and quit it (and its hub) through its own API. |

New gates use `tools/printer_emulator` instead. These move into the emulator when the gate that
uses them is next touched.
