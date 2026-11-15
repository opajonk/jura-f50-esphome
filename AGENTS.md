# Agent notes

ESPHome external component (`components/jura_f50`) that talks to a Jura coffee
machine's service port from an ESP32. The live device is reached through a Home
Assistant installation; `tools/jura_loop.py` closes an edit → push → OTA → test
loop. User-facing details are in [README.md](README.md).

## Layout

- `components/jura_f50/` – C++ component (`jura_f50.h/.cpp`), schema/codegen
  (`__init__.py`), and the power `binary_sensor` platform (`binary_sensor.py`).
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
$J command RR:44 --confirm-command     # any other single command
$J dump --label off                    # EEPROM words 00-FF via RE:
$J dump --kind RR --start 40 --end 4F  # RAM via RR:
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

## Protocol findings (machine reports `ty:EPSILON MASK 11`)

- Firmware: UART 9600 baud, inter-frame delay 8 ms, response timeout 2 s.
- `TY:` is the reliable handshake; `RT:` replies are unreliable.
- `RE:` addresses 16-bit words using only the first two hex digits
  (`RE:0100` reads word `01`), so the EEPROM spans words `00`-`FF`.
- `RR:xx` returns RAM bytes `xx` and `xx+1`. Byte `0x44` is `01` while on
  (heating, rinsing, ready) and `00` while off; bytes `0x1B` and `0x54` behave
  the same. This drives the power binary sensor.
- EEPROM holds counters, not the power state. Words `07`, `0D`, `10`, `11`, and
  `7D` increment on power-on and `7C` on power-off. Coffee counters have not
  been identified yet: compare dumps taken before and after a brew performed
  by the user.
- `IC:` does not reflect power state and changes even while off.
- The [Q42/coffeehack](https://github.com/Q42/coffeehack) notes describe a
  different model; verify addresses before relying on them.
