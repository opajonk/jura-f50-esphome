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

switch:
  - platform: template
    name: "Coffeemaker Power"
    optimistic: true
    turn_on_action:
      - lambda: id(jura).queue_command("AN:01");
    turn_off_action:
      - lambda: id(jura).queue_command("AN:02");
```

Options: `intra_byte_delay` (default 1ms), `inter_frame_delay` (default 7ms), `rx_timeout` (default 20ms).
