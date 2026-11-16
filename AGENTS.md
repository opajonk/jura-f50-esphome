# Agent notes

ESPHome external component (`components/jura_f50`) that talks to a Jura coffee
machine's service port from an ESP32. The live device is reached through a Home
Assistant installation; `tools/jura_loop.py` closes an edit → push → OTA → test
loop. User-facing details are in [README.md](README.md).

## Start a fresh research session

1. Work from this repository's root. In the parent workspace it is the submodule
   `dev/jura-f50-esphome`; a standalone clone has the same relative paths.
   Check `git status` before editing; preserve unrelated or uncommitted changes.
2. Read the [F50 research backlog](README.md#f50-home-assistant-research-backlog)
   and pick only the ID requested by the user (R01 is the suggested first task).
   Each row has sources, uncertainty, HA value, and a bounded first experiment.
   Do not automatically start the next ID or send experimental control commands.
3. If `.venv` is absent, use the README's devcontainer/manual setup. Live work
   additionally requires a reachable Home Assistant URL and locally provisioned
   token; never assume a fresh clone includes credentials, device YAML, or dumps.
   Use `JURA_HOME_ASSISTANT` or `--home-assistant URL`, then run the `TY:` handshake.
   If access is missing, continue offline research and ask for setup, not secrets
   in chat. Compilation alone does not require the machine.
4. Ask the owner for the machine's current display/state before measuring.
   Take repeated baselines, change one condition, then return to baseline.
   Use paused-status dumps, preserve raw replies, and reject malformed samples.
   Do not implement guessed bit/address mappings.
5. Before ending, update the README row with verified results or a clear
   inconclusive outcome, evidence/dump names, and the next experiment; summarize
   durable findings below. Dumps are ignored, so keep essential values in docs.
   No push, device-YAML write, or physical action without the required approval.

## Layout

- `components/jura_f50/` – C++ component (`jura_f50.h/.cpp`), schema/codegen
  (`__init__.py`), the power `binary_sensor` platform (`binary_sensor.py`),
  coffee-counter `sensor` platform (`sensor.py`), and clock `text_sensor`
  platform (`text_sensor.py`).
- `tools/jura_loop.py` – CLI: `deploy`, `command`, `dump`, `diff`.
- `tests/test_jura_loop.py` – unit tests for the CLI.
- `tests/esphome/jura-test.yaml` – compile-only ESPHome config for the component.
- `.devcontainer/` – dev container that prepares `.venv` and runs the tests.
- `dumps/` – git-ignored EEPROM/RAM dumps from earlier investigations.

## Edit, build, test

```sh
.venv/bin/python -m unittest -q tests.test_jura_loop       # CLI unit tests
.venv/bin/esphome config tests/esphome/jura-test.yaml      # schema check
.venv/bin/esphome compile tests/esphome/jura-test.yaml     # full ESP32 build (slow first time)
.venv/bin/esphome compile tests/esphome/counter-test.yaml  # native counter tests, no device access
tests/esphome/.esphome/build/jura-counter-test/.pioenvs/jura-counter-test/program
.venv/bin/esphome compile tests/esphome/jura-pause-test.yaml  # native clock/pause tests (~63s)
tests/esphome/.esphome/build/jura-pause-test/.pioenvs/jura-pause-test/program
```

Compile locally before publishing component changes.

## Live loop

Requires network access to Home Assistant, a token in
`~/.config/jura-loop/home-assistant-token` (mode 600), and
`JURA_HOME_ASSISTANT` set (or `--home-assistant URL`). The installation uses
plain HTTP, which the owner accepted, so every call needs `--allow-http`.

```sh
J=".venv/bin/python tools/jura_loop.py --allow-http"
$J deploy                              # compile + OTA the published origin/main
$J command                             # read-only TY: handshake, expects ty:<type>
$J command RR:44 --confirm-command --attempts 1  # required for every non-TY command
$J command IC: --confirm-command --attempts 1    # exploratory input/status read
$J dump --pause-status --label off              # EEPROM words 00-FF via RE:
$J dump --pause-status --kind RR --start 40 --end 4F  # RAM via RR:
$J --home-assistant unused diff dumps/a.txt dumps/b.txt
```

`deploy` builds what is on GitHub `main`, not the working tree: commit, get
approval, push, then deploy. It refuses unpublished component changes, an
offline device, or a running Builder job.

The device configuration (`jura-coffee-machine.yaml`) lives in the ESPHome
Builder app, not in this repository. It can be read and written over the
Builder websocket with `devices/get_config` and
`devices/update_config {configuration, content}` (see `connect()` in the CLI).
It contains secrets: never print or commit it, back it up with mode 600 under
`~/.config/jura-loop/` before changing it, and redeploy afterwards.

## Rules

- Get the user's explicit approval before `git push` or anything that writes
  to the device configuration.
- Never print, log, or commit tokens, API/OTA keys, or Wi-Fi passwords.
- Only `TY:`, `RE:xx`, `RR:xx`, and `IC:` are known to be read-only. Ask before
  sending anything else: `AN:`, `FA:`, `FN:` operate the machine, and `AN:0A`
  erases the EEPROM. Never write EEPROM (`WE:`).
- When an experiment needs the machine in a particular state, ask the user to
  operate it physically and confirm before measuring.
- Only one command is in flight at a time, and the component polls `RR:44`
  every 10 s. The CLI ignores traffic sent before its command, but fails a
  test if another command goes out before the reply arrives; `dump` retries
  such words. Testing `RR:44` itself may pick up a poll's identical reply.
  Use `dump --pause-status` for RAM/EEPROM experiments: it drains in-flight
  polls, renews the 60-second pause, and resumes in `finally`. Its API action
  must exist; see README setup. It pauses power, counter, and clock polling,
  not manual commands. Never run two experiments concurrently.
  `command` has no pause flag: repeated `IC:` sampling needs controlled pause/
  resume via the existing action (with renewal and cleanup), not an assumption
  that a previous dump left polling paused. Garbled replies have occurred.

## Protocol findings (machine reports `ty:EPSILON MASK 11`)

- Owner-confirmed button commands: `FA:06` = single cup (mild), `FA:07` =
  two cups, `FA:09` = steam preparation, `FA:02` = rinse. Repeating `FA:06`
  twice/three times may select normal/strong, but this is not yet confirmed.
  Do not use the S90/S95 or F7/J6 product mappings for this machine.
  `FA:0C` presses the rotary dial button; `FA:0D`/`FA:0E` do not rotate the
  dial. `FA:15` starts the cleaning
  programme (tablet). `FA:08` and `FA:10`-`FA:14`
  reply `ok:` but their effect is unknown (see README table); never send them
  without the user's consent.
- Firmware: UART 9600 baud, inter-frame delay 8 ms, response timeout 2 s.
- `TY:` is the reliable handshake; `RT:` replies are unreliable.
- `DA:<text>` writes display text (`ok:`); the owner reports a ten-character
  maximum. `1234567890` and `ABCDEFGHIJ` were confirmed visible. Paused-status
  RAM dumps found no plain-text marker; repeated A/B/A tests found no
  reproducible display representation in `RR:00`-`RR:FF`. Bytes `0x23` and
  `0x92` were false leads. Display writes need approval, like other state changes.
- `RE:` addresses 16-bit words using only the first two hex digits
  (`RE:0100` reads word `01`), so the EEPROM spans words `00`-`FF`.
- `RR:xx` returns RAM bytes `xx` and `xx+1`. Byte `0x44` is `01` while on
  (heating, rinsing, ready) and `00` while off; bytes `0x1B` and `0x54` behave
  the same. This drives the power binary sensor.
- EEPROM holds counters, not the power state. Words `07`, `0D`, `10`, `11`, and
  `7D` changed during power experiments and `7C` on power-off; their exact
  meanings remain unverified.
  Six saved EEPROM dumps, including one complete 256-word/512-byte capture,
  contained no `BEREIT`, `FILTER`, or `HAERTE`/`HÄRTE` in the tested ASCII,
  Latin-1, UTF-8, or UTF-16 encodings and either word byte order. This does not
  exclude encoded strings or message IDs.
- One single coffee incremented EEPROM word `00` from 19,873 to 19,874.
  Summing words `00`–`04` plus `06` then gave 35,690, matching the displayed
  lifetime total. Word `04` is zero, so its inclusion is provisional.
  `sensor.py` exposes `single_coffee` and `total_coffee`, with a shared
  configurable `update_interval` defaulting to 120s (separate from power).
- `IC:` does not reflect power state and changes even while off.
- `RR:BC` reads binary hours/minutes from RAM bytes `0xBC`/`0xBD`.
  The clock text sensor polls every 30s by default. Zero displays `--:--`,
  including at midnight; failures display `unknown`. Status pause also pauses
  clock polling and resumes it with a fresh read.
- The [Q42/coffeehack](https://github.com/Q42/coffeehack) notes describe a
  different model; verify addresses before relying on them.
