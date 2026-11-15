# jura-f50-esphome

ESPHome external component for talking to a Jura coffeemaker (e.g. F50, Impressa S95) over its service port. Each payload byte is encoded into 4 UART bytes with the timing used in `f50-poc.ino`.

```yaml
external_components:
  - source: github://opajonk/jura-f50-esphome
    components: [jura_f50]

uart:
  id: jura_uart
  tx_pin: GPIO5
  rx_pin: GPIO4
  baud_rate: 9600

jura_f50:
  id: jura
  uart_id: jura_uart
  on_line:
    - logger.log:
        format: "Jura: %s"
        args: ['line.c_str()']
  # Fires with (command, response) when a line answers a queued command.
  on_response:
    - lambda: |-
        if (response.rfind("ok", 0) == 0) {
          if (command == "AN:01") id(coffee_power).publish_state(true);
          if (command == "AN:02") id(coffee_power).publish_state(false);
        }
  # Fires with (command) when no reply arrived within response_timeout.
  on_response_timeout:
    - logger.log:
        format: "Jura did not answer '%s'"
        args: ['command.c_str()']
        level: WARN

switch:
  - platform: template
    name: "Coffeemaker Power"
    id: coffee_power
    icon: mdi:coffee-maker
    # Not optimistic: state is only published once the Jura confirms with "ok:".
    turn_on_action:
      - lambda: id(jura).queue_command("AN:01");
    turn_off_action:
      - lambda: id(jura).queue_command("AN:02");

# Optional: send arbitrary commands from Home Assistant for development
# (Developer Tools -> Actions -> esphome.<node>_send_command).
api:
  actions:
    - action: send_command
      variables:
        command: string
      then:
        - lambda: id(jura).queue_command(command);
    # Optional: required for the CLI's dump --pause-status flag.
    - action: set_status_polling_paused
      variables:
        paused: bool
      then:
        - lambda: id(jura).set_status_polling_paused(paused);
```

Queued commands are sent one at a time; the next command is dispatched once the previous one has been answered or timed out.

Options: `intra_byte_delay` (default 1ms), `inter_frame_delay` (default 7ms), `rx_timeout` (default 20ms), `response_timeout` (default 2s).

### Power state

```yaml
binary_sensor:
  - platform: jura_f50
    power:
      name: "Coffeemaker Powered"
      # Optional: keep the power switch above in sync with the polled state,
      # e.g. when the machine is switched on or off at its own buttons.
      on_state:
        - lambda: id(coffee_power).publish_state(x);
    update_interval: 10s  # default
```

The switch still turns on or off immediately when the Jura acknowledges
`AN:01`/`AN:02` with `ok:`; later polls then keep it in sync.

The component polls `RR:44` through the command queue. RAM byte `0x44` was
`01` while the machine was on (heating, rinsing, and ready) and `00` while it
was off on an `EPSILON MASK 11`; verify it on other models. The sensor becomes
unknown after three consecutive failed polls.

### Coffee counters

```yaml
sensor:
  - platform: jura_f50
    single_coffee:
      name: "Single Coffee Counter"
    total_coffee:
      name: "Total Coffee Counter"
    update_interval: 120s  # default; independent of the power sensor interval
```

Either sensor can be omitted. Counters are read at startup and then at the
configured interval, through the existing command queue. `single_coffee` reads
EEPROM word `00`. `total_coffee` sums words `00`–`04` and `06`, excluding `05`.
On the tested `EPSILON MASK 11`, one single coffee incremented word `00` and the
sum from 35,689 to 35,690, matching the machine's displayed lifetime total.
Other product mappings, including whether zero-valued word `04` belongs in the
sum, remain provisional; verify them on other machines.

A failed or malformed read reports the affected sensor as unknown and retries
at the next polling interval. A total is published only after all six reads
succeed; the reads are sequential, not an atomic snapshot during brewing.
Both sensors use `total_increasing` state class for Home Assistant statistics.

### Temporarily pausing status collection

`id(jura).set_status_polling_paused(true)` pauses automatic power and coffee-counter
polling for 60 seconds.
Calling it again renews the pause; `false` resumes immediately and requests a fresh
status. Pauses are not persisted across reboot. Manual commands, UART reception,
and response timeouts continue normally. Queued automatic polls are removed,
but an in-flight command is allowed to finish before the component logs
`Status polling pause ready`. Skipped polls do not count as failures; the power
and counter sensors keep their last states and may therefore be stale during
the pause. An interrupted counter sweep is restarted from word `00` on resume;
partial totals are never published.

The API action above exposes this control to Home Assistant. Optionally expose
the current flag as a template switch (its state also reflects automatic expiry):

```yaml
switch:
  - platform: template
    name: "Jura Status Polling Paused"
    lambda: return id(jura).is_status_polling_paused();
    turn_on_action:
      - lambda: id(jura).set_status_polling_paused(true);
    turn_off_action:
      - lambda: id(jura).set_status_polling_paused(false);
```

This is a shared pause flag, not an exclusive command lock. Avoid concurrent
dumps or other pause controllers; manual commands can still interfere.

## Local edit, deploy, and test loop

### Dev container

Open the repository in VS Code and choose **Reopen in Container** (or create a
Codespace). The container provides Python 3.13, the GitHub CLI, the ESPHome
extension, and a `.venv` with `requirements-dev.txt` (including the ESPHome
version used by the Builder app); the unit tests run once after creation.
Each start prints what is still missing for the live loop.

- The host directory `~/.config/jura-loop` is created if needed and mounted
  into the container, so the Home Assistant token stays on the host. Linux and
  macOS hosts are supported as-is; on Windows, run the container from WSL.
- Export `JURA_HOME_ASSISTANT` on the host before opening the container to
  forward it; otherwise pass `--home-assistant`.
- Run `gh auth login && gh auth setup-git` inside the container to push.
- The PlatformIO toolchain cache is kept in a Docker volume, so only the first
  `.venv/bin/esphome compile tests/esphome/jura-test.yaml` downloads it.

The live loop needs network access to Home Assistant; a Codespace normally
cannot reach a home LAN, but can still edit, test, and compile.

### Manual setup

The helper below targets the `jura-coffee-machine` ESPHome Builder configuration
on Home Assistant. It requires the device's `send_command` API action and DEBUG
logs, and the configuration must fetch this repository with `refresh: 0s`.

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
```

Save a Home Assistant long-lived access token in
`~/.config/jura-loop/home-assistant-token` with file mode 600; never commit it.
The account must be permitted to access the ESPHome Builder app and call its
device action. Set `JURA_HOME_ASSISTANT` to your Home Assistant URL or pass
`--home-assistant https://...`; use HTTPS when available.
If the installation uses HTTP, commands require `--allow-http`:
the token and other session credentials are exposed to the LAN. File permissions
do not protect against other processes using the same operating-system account.

1. Edit and commit changes on `main`. Obtain approval before pushing, then run
   `git push origin main`.
2. Run `.venv/bin/python tools/jura_loop.py --allow-http deploy`. The helper
   refuses unpublished component changes, an unexpected external source, or
   an offline device or already-running device job. It waits for both compile
   and OTA upload. If the device goes offline during a build, check the
   dashboard for a queued update before retrying.
3. Run `.venv/bin/python tools/jura_loop.py --allow-http command` to send the
   read-only `TY:` handshake and check for a nonempty `ty:<type>` response.
   Avoid other concurrent commands while testing. If it times out,
   `command --attempts 3` allows up to three attempts; a late response cannot
   be distinguished from a response to a retry. To send a different command,
   explicitly provide it and `--confirm-command`; commands may operate the machine.

### Reading EEPROM and RAM

`dump` reads 16-bit words with the read-only `RE:` (EEPROM, default) or `RR:`
(RAM) commands, retrying timeouts and malformed replies, and writes them to the
git-ignored `dumps/` directory. `diff` lists addresses whose read values differ.

```sh
.venv/bin/python tools/jura_loop.py --allow-http dump --label off
.venv/bin/python tools/jura_loop.py --allow-http dump --kind RR --start 40 --end 4F --label on
.venv/bin/python tools/jura_loop.py --allow-http dump --pause-status --kind RR --label on
.venv/bin/python tools/jura_loop.py --home-assistant unused diff dumps/*-re-*.txt
```

`--pause-status` requires the updated component and the
`set_status_polling_paused` API action above. For this installation, Home
Assistant must expose it as `esphome.jura_coffee_machine_set_status_polling_paused`.
The CLI waits for the drain acknowledgement before reading, renews the pause
between reads when 20 seconds have elapsed, and resumes in `finally`, including on
Ctrl-C or failed acquisition. A missing action, lost acknowledgement, unexpected
resume, or cleanup failure is an error, not a fallback to unpaused dumping.
If the CLI crashes or loses connectivity, the device resumes within 60 seconds
of the last renewal. The flag does not eliminate late replies after timeouts;
normal validation and retries remain enabled. Dumps without the flag are unchanged.

The machine uses only the first two hex digits of an address, so `RE:0100`
reads word `01`; `RE:` therefore covers words `00`-`FF`. `RR:xx` addresses bytes
and returns bytes `xx` and `xx+1`. Missing words are stored as `????`.

Deployment fetches GitHub's default branch rather than an immutable commit. The
helper checks that `origin/main` matches the local commit before and after the
build, but cannot prove which revision was fetched if the branch changes during
the build. An OTA restarts the ESP32; failed or interrupted updates may require
physical recovery. The helper does not commit or push changes automatically.

Local validation:

```sh
.venv/bin/python -m unittest -q tests.test_jura_loop
.venv/bin/esphome compile tests/esphome/jura-test.yaml
# Native fake-UART tests, including renewal and expiry (about 63s after startup):
.venv/bin/esphome compile tests/esphome/jura-pause-test.yaml
tests/esphome/.esphome/build/jura-pause-test/.pioenvs/jura-pause-test/program
```
