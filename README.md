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

### Sending the same command repeatedly from Home Assistant

Use a text input to prepare a command and a separate button to send it.
Editing the text alone does not transmit anything; every button press sends
the current command, even when it has not changed.

```yaml
text:
  - platform: template
    name: "Jura Command"
    id: jura_command
    mode: text
    optimistic: true

button:
  - platform: template
    name: "Send Jura Command"
    on_press:
      - lambda: |-
          const auto &command = id(jura_command).state;
          if (command.empty() || command.find_first_of("\r\n") != std::string::npos) {
            ESP_LOGW("jura_command", "Enter a non-empty, single-line command before sending");
            return;
          }
          id(jura).queue_command(command);
```

Commands can operate the machine. Do not substitute command mappings from
other models.

#### `FA:` commands on the Impressa F50 (`ty:EPSILON MASK 11`)

`FA:xx` appears to simulate a front-panel key press. An `ok:` reply only means
the command was accepted, not that it did anything visible.

| Command | Status | Observed / likely function |
| --- | --- | --- |
| `FA:02` | confirmed | Rinse |
| `FA:06` | confirmed | Single cup (mild). Pressing again may select normal/strong (unconfirmed) |
| `FA:07` | confirmed | Two cups |
| `FA:09` | confirmed | Steam preparation (heats up for steam) |
| `FA:0C` | confirmed | Rotary dial push button (enter/confirm) |
| `FA:15` | confirmed | Starts the cleaning programme (tablet) |
| `FA:08` | `ok:`, effect unknown | Hypothesis: hot water. Q42 notes for another model list `FA:08` as hot water; other models use it for a second espresso product |
| `FA:0D`, `FA:0E` | tested | Not the rotary dial rotation (unlike on other models); effect otherwise unknown |
| `FA:10`–`FA:14` | `ok:`, effect unknown | Hypothesis: other panel/maintenance keys or menu navigation (rotary switch, programme/menu, aroma/pre-ground, decalcify, filter). No community source documents these codes, so they are unverified |

Notes from other models (not verified on the F50):
- [Q42/coffeehack](https://github.com/Q42/coffeehack) lists `FA:01`-`FA:07` as products, `FA:08` as hot water, `FA:09` as steam and `FA:0B` as flush.
- [ryanalden/esphome-jura-component](https://github.com/ryanalden/esphome-jura-component) maps `FA:0B`-`FA:0F` to user-interface actions: light/exit menu, enter menu, dial counter-clockwise, dial clockwise, and a freeze. `FA:0C` matches the F50's dial button, but `FA:0D`/`FA:0E` do not rotate the dial on the F50 (tested).
- The F50's maintenance programmes (clean, decalcify, filter) sit next to each other. Since `FA:15` cleans, decalcify or filter replacement may be on a neighbouring code. Test those codes only with water and the drip tray in place, and be ready to switch the machine off.

To identify an unknown code, send it with the machine on and idle, then watch the
display and the `RR:44` power byte. Diff `RE:` dumps taken before and after to
see which maintenance counters change.

#### Display text on the F50

`DA:<text>` writes text to the display and replies `ok:`. The owner confirmed
`DA:1234567890` displays all ten characters and reported a ten-character maximum.
`DA:ABCDEFGHIJ` was also confirmed visible during RAM testing.

RAM dumps with automatic status polls paused were taken for `1234567890`,
`ABCDEFGHIJ`, and `AAAAAAAAAA` / `BBBBBBBBBB` / `AAAAAAAAAA`. No plain-text
marker was found in the readable `RR:00`-`RR:FF` window. Bytes `0x23` and `0x92`
initially appeared to track the repeated-letter changes, but a second A/B/A
test ruled them out. No reproducible display representation has been identified;
this does not rule out an encoded buffer or storage outside this RAM window.
Display writes change the visible text; get approval before using them.

Options: `intra_byte_delay` (default 1ms), `inter_frame_delay` (default 7ms), `rx_timeout` (default 20ms), `response_timeout` (default 1s).

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
unknown after five consecutive failed polls.

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

A failed or malformed read is logged and leaves the last successful counter
values unchanged, retrying at the next polling interval. Until a sensor's first
successful read after boot, its value remains unknown. A total is published only
after all six reads succeed; the reads are sequential, not an atomic snapshot
during brewing.
Both sensors use `total_increasing` state class for Home Assistant statistics.

### Machine clock

```yaml
text_sensor:
  - platform: jura_f50
    clock:
      name: "Coffeemaker Clock"
    update_interval: 30s  # default; independent of power and counter polling
```

This read-only text sensor displays the machine's clock as `HH:MM` in Home
Assistant. It reads `RR:BC` at startup and then at the configured interval.
RAM bytes `0xBC` and `0xBD` contain binary hours and minutes on the tested
`EPSILON MASK 11`; verify the mapping on other models. It does not synchronize
or set the machine's clock.

A zero reading displays `--:--`, matching the observed unset clock. Midnight
(`00:00`) has the same encoding and therefore also displays `--:--`; no separate
clock-validity flag has been identified. Malformed, out-of-range, or timed-out
replies are logged and display `unknown`, with recovery on the next valid poll.

### Temporarily pausing status collection

`id(jura).set_status_polling_paused(true)` pauses automatic power, coffee-counter,
and clock polling for 60 seconds.
Calling it again renews the pause; `false` resumes immediately and requests a fresh
status. Pauses are not persisted across reboot. Manual commands, UART reception,
and response timeouts continue normally. Queued automatic polls are removed,
but an in-flight command is allowed to finish before the component logs
`Status polling pause ready`. Skipped polls do not count as failures; all sensors
keep their last states and may therefore be stale during the pause.
An interrupted counter sweep is restarted from word `00` on resume;
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

## F50 Home Assistant research backlog

Internet references checked on 2026-11-16. Pick **one ID per session**; start
with R01. These are research leads, not supported F50 entities or permission to
operate the machine. Existing power, clock, and single/total coffee sensors are
described above. The target reports `ty:EPSILON MASK 11`.

**Evidence:** "other-model implementation" means source code actually polls or
parses the command, not that its interpretation is correct on the F50.
Archived register tables are explicitly unverified. In particular, the F7
and newer machines have different product layouts and even disagree about which
`IC:` byte contains a flag. Never copy their bit positions or addresses as facts.

| ID / priority | Home Assistant use | Command / evidence | First bounded investigation |
| --- | --- | --- | --- |
| **R01 / high** | Tray / grounds-container present binary sensor | `IC:`. [F7 implementation][research-f7] parses tray presence; [Q42 notes][research-q42] record an input change when the grounds container is removed. Exact F50 bit unknown. | Record repeated samples with the tray/container inserted, removed, then reinserted, with the owner operating it while idle. Require a reproducible bit change; do not confuse presence with "grounds full". |
| **R02 / high** | Water-tank present / fill-water notification | `IC:`. [F7][research-f7] and [legacy parser][research-legacy-code] interpret a tank flag, but use different payload bytes. | Compare tank installed, removed, and reinstalled. Separately test a naturally low-water condition: removal alone cannot establish that the bit means "empty". Never run a pump without water. |
| **R03 / high** | Ready, heating, brewing, or blocked status; coffee-finished automation | `IC:` and `RR:xx`. [Legacy parser][research-legacy-code] derives ready/busy states from inputs. F50 `RR:44` is already known to mean power, **not readiness**. | Capture idle baselines, then time-series samples during one owner-approved normal startup or coffee. Repeat to distinguish persistent flags from transient activity. Use physical/display observations as ground truth, not `ok:` or an incrementing counter alone. |
| **R04 / high** | Grounds-bin count / empty-bin reminder | `RE:00`-`RE:15` initially. [Archived map][research-registers] labels its 15th field as grounds level; the equivalent sequential-word candidate would be `RE:0E`, **not a verified F50 address**. | Compare dumps before/after one normal coffee and an owner-performed emptying/reinsertion. Verify whether a candidate resets and whether it counts pucks, cycles, or drinks. |
| **R05 / high** | Cleaning/descaling totals and maintenance history | `RE:xx`. [F7 parser][research-f7] labels its 9th/10th fields cleanings/descalings: `RE:08`/`RE:09` are candidates only if the layouts align. | Start with saved EEPROM data and the owner's maintenance-menu totals. Confirm a change at the next genuinely needed maintenance cycle; do not launch cleaning/descaling just to obtain a sample. |
| **R06 / medium** | Cleaning, descaling, or filter-change due notification | `RE:xx` / `IC:`. [Archived map][research-registers] suggests "since maintenance" fields (including fields 14/16, possible words `0D`/`0F`). Its meanings are unverified, and it establishes no F50 due flag or threshold. | Compare normal readings with the actual maintenance display over time; include words `0D`, `10`, and `11`, which changed in earlier F50 experiments. Distinguish lifetime totals, resettable counters, and real warning flags. Do not invent a percentage or fixed threshold. |
| **R07 / medium** | Two-cup / other product counters; verify total-coffee formula | `RE:01`-`RE:06`, extending the existing `RE:00` single-coffee sensor. [F7 parser][research-f7] exposes per-product counters, but its product names do not match this F50. | Have the owner make one two-cup serving when wanted, diff before/after, and establish whether counts mean servings or cups. Keep the existing total formula unchanged until evidence warrants changing it; word `04` remains provisional. |
| **R08 / medium** | Ten-character notification text from HA | `DA:<text>` is **F50-confirmed**; [J6 notes][research-j6] also document display writing. | Test supported characters, shorter strings/padding, persistence, and normal display recovery with approval. Decide how notifications avoid hiding machine warnings. Do not assume a display-read counterpart: five full marker dumps found no reproducible buffer in `RR:00`-`RR:FF`. |
| **R09 / medium** | Hot-water / steam-preparation controls | `FA:08` is hot water in [Q42 notes][research-q42] but a different beverage on J6. F50 accepts it with unknown effect; `FA:09` is owner-confirmed steam preparation. | First inspect the F50's normal physical hot-water/steam workflow with the owner. Any command test requires explicit approval and supervision; acceptance does not prove dispensing, readiness, or valve control. |
| **R10 / low** | Menu navigation / settings controls | `FA:0C` is the F50 dial push button. [J6 notes][research-j6] describe other menu keys, but F50 tests already rejected `FA:0D`/`FA:0E` as dial rotation. `FA:10`-`FA:14` remain unknown despite `ok:`. | Choose one explicitly approved code and one known menu state, record its visible effect, then stop. These can trigger maintenance, not just navigation (`FA:15` cleans). Do not bulk-scan commands or expose unknown keys as normal HA controls. |
| **R11 / low** | Firmware/loader information diagnostic entity | `TY:` already works; [J6 notes][research-j6] report `TL: LOADER V3.0`. F50 `TL:` support and effects are unverified. | After approval, make one `TL:` query and record its exact reply or timeout. This is informational only, not a reason to enter a loader/test mode. |
| **R12 / deferred** | Rich alerts, maintenance percentages, brew progress, recipe options, panel lock | [Modern dongle protocol][research-at] documents `@TF:`, `@TG:43`, `@TG:C0`, `@TR:32,NN`, progress `@TV:`, recipes `@TP:`, and lock `@TS:` on newer machines. **No evidence this F50 supports that protocol.** | Research F50/EPSILON compatibility first. These commands need encrypted framing and a dongle handshake; our clear-text queue/CLI does not implement them. Do not send them as ordinary strings or replace the working component. Treat writes/locking as a separate, opt-in project only if compatibility is established. |

### Research boundaries and handoff

- `TY:`, `RE:`, `RR:`, and `IC:` are the current read-only allowlist.
  Other-model claims do not extend it: get approval before any other command.
  Even a known `FA:` mapping must not be sent without approval for that experiment.
- Keep `WE:` (EEPROM writes), `AN:0A` (erase), test/loader modes, and direct
  `FN:` actuator control out of this backlog. They are unnecessary for normal HA
  sensors and can change persistent state or operate hardware outside normal cycles.
- The archived map also mentions `TT:` and `RU:`, but explicitly marks its
  interpretations unverified and supplies no F50 results. Park these until a
  stronger source establishes syntax and safety; do not assume temperature or
  runtime readings exist.
- For each ID, record exact commands/replies, observed machine/display state,
  timestamped dump names, repeatability, rejected hypotheses, and the next step.
  Update its status here and the concise findings in [AGENTS.md](AGENTS.md);
  mark an entity supported only after F50 verification and focused tests.
  Status of R01-R12: **not started**, except the prior observations noted above.
- Use [the live-loop setup](#local-edit-deploy-and-test-loop) and
  `dump --pause-status` for comparisons. Dumps remain sequential snapshots:
  neighbouring `RR:` reads overlap and can disagree when live bytes change.
  A failed or malformed read is missing evidence, not a zero or a state transition.
- Existing dumps are git-ignored and will not come with a fresh clone. Preserve
  conclusions and essential sample values in these docs; collect fresh baselines
  if raw captures are unavailable. A clean clone still needs LAN access, local
  credentials, and the Builder configuration before live work.

Sources (external model claims are not F50 verification):

[research-f7]: https://github.com/alco28/Jura-F7-ESPHOME/blob/main/custom_components/jura_coffee/jura_coffee.cpp
[research-legacy-code]: https://github.com/tiaanv/jura/blob/main/components/jura/jura.h
[research-registers]: https://github.com/tiaanv/jura/blob/main/legacy/docs/uart-register-map.md
[research-q42]: https://github.com/Q42/coffeehack/blob/master/reverse-engineering/commands.txt
[research-j6]: https://github.com/ryanalden/esphome-jura-component/blob/master/README.md
[research-at]: https://github.com/harryzz/jura-uart-esphome/blob/main/components/jura_link/AT-PROTOCOL.md

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
   explicitly provide it with `--confirm-command --attempts 1`; non-`TY:`
   commands reject the default retry count and may operate the machine.

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
# Native fake-UART tests, including clock polling and pause expiry (about 63s):
.venv/bin/esphome compile tests/esphome/jura-pause-test.yaml
tests/esphome/.esphome/build/jura-pause-test/.pioenvs/jura-pause-test/program
```
