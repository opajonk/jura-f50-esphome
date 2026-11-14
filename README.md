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
