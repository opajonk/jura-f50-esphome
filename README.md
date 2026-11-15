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

## Local edit, deploy, and test loop

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
.venv/bin/python tools/jura_loop.py --home-assistant unused diff dumps/*-re-*.txt
```

The machine uses only the first two hex digits of an address, so `RE:0100`
reads word `01`; `RE:` therefore covers words `00`-`FF`. `RR:xx` addresses bytes
and returns bytes `xx` and `xx+1`. Missing words are stored as `????`.

Deployment fetches GitHub's default branch rather than an immutable commit. The
helper checks that `origin/main` matches the local commit before and after the
build, but cannot prove which revision was fetched if the branch changes during
the build. An OTA restarts the ESP32; failed or interrupted updates may require
physical recovery. The helper does not commit or push changes automatically.
