#pragma once

#include <cstdlib>

#include "esphome/components/jura_f50/jura_f50.h"

inline void pause_check(bool condition, const char *message) {
  if (!condition) {
    ESP_LOGE("pause_test", "%s", message);
    std::exit(1);
  }
}

class FakeUART : public esphome::uart::UARTComponent {
 public:
  void write_array(const uint8_t *, size_t) override {}
  bool peek_byte(uint8_t *) override { return false; }
  bool read_array(uint8_t *, size_t) override { return false; }
  size_t available() override { return 0; }
  void check_logger_conflict() override {}
  esphome::uart::UARTFlushResult flush() override {
    return esphome::uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
  }
};

class PauseProbe : public esphome::jura_f50::JuraF50 {
 public:
  using JuraF50::poll_status_;
  using JuraF50::poll_counters_;
  using JuraF50::poll_clock_;
  bool acknowledgement_pending() const { return this->status_pause_ack_pending_; }
  bool poll_pending() const { return this->status_pending_; }
  size_t queued() const { return this->command_queue_.size(); }
  uint8_t failures() const { return this->status_failures_; }
  bool counter_pending() const { return this->counter_pending_; }
  bool clock_pending() const { return this->clock_pending_; }
  std::string next_command() const { return this->command_queue_.front().command; }
  void dispatch() { this->process_tx_(); }
  void finish_transmission() {
    this->tx_queue_.clear();
    this->tx_index_ = 4;
    this->tx_wait_us_ = 0;
  }
  void respond(const std::string &response) {
    for (char c : response + "\r\n")
      this->handle_decoded_(c);
    this->acknowledge_status_pause_();
  }
};

static PauseProbe expiry_probe;
static PauseProbe clock_interval_probe;
static uint32_t clock_interval_started;
static bool clock_interval_verified{false};

inline void finish_clock_interval_poll() {
  clock_interval_probe.dispatch();
  clock_interval_probe.finish_transmission();
  clock_interval_probe.respond("rr:140F");
  clock_interval_probe.loop();
}

inline void run_pause_tests(esphome::jura_f50::JuraF50 *configured_component) {
  static FakeUART uart;
  configured_component->set_uart_parent(&uart);
  configured_component->set_power_binary_sensor(nullptr);
  configured_component->set_single_coffee_sensor(nullptr);
  configured_component->set_total_coffee_sensor(nullptr);
  configured_component->set_clock_text_sensor(nullptr);
  static esphome::binary_sensor::BinarySensor sensor;
  static PauseProbe probe;
  probe.set_uart_parent(&uart);
  probe.set_power_binary_sensor(&sensor);
  probe.setup();
  probe.queue_command("RR:44");
  probe.set_status_polling_paused(true);
  pause_check(probe.queued() == 1 && !probe.poll_pending(), "pause must cancel only automatic queued polls");
  probe.poll_status_();
  pause_check(probe.queued() == 1 && probe.failures() == 0, "skipped polls must not queue or count as failures");
  pause_check(probe.acknowledgement_pending(), "pause must wait for existing traffic");
  probe.dispatch();
  probe.finish_transmission();
  probe.respond("rr:0100");
  pause_check(!probe.acknowledgement_pending() && sensor.state, "manual reads must finish while paused");
  probe.set_status_polling_paused(false);
  pause_check(probe.queued() == 1 && probe.poll_pending(), "resume must request a fresh poll");
  probe.dispatch();
  probe.set_status_polling_paused(true);
  pause_check(probe.poll_pending() && probe.acknowledgement_pending(), "pause must not abort an in-flight poll");
  probe.finish_transmission();
  probe.respond("rr:0000");
  pause_check(!probe.poll_pending() && !probe.acknowledgement_pending(), "pause must acknowledge the drained poll");
  pause_check(!sensor.state && probe.failures() == 0, "in-flight responses must still update the sensor");
  probe.set_status_polling_paused(false);
  probe.set_status_polling_paused(true);
  pause_check(probe.queued() == 0 && !probe.acknowledgement_pending(), "idle pause must acknowledge immediately");
  probe.loop();
  static PauseProbe manual_probe;
  manual_probe.set_uart_parent(&uart);
  manual_probe.set_power_binary_sensor(&sensor);
  manual_probe.queue_command("RR:44");
  manual_probe.setup();
  manual_probe.dispatch();
  manual_probe.finish_transmission();
  manual_probe.respond("rr:0100");
  pause_check(manual_probe.poll_pending(), "a manual RR:44 response must not clear a queued automatic poll");
  manual_probe.set_status_polling_paused(true);
  pause_check(manual_probe.queued() == 0 && !manual_probe.poll_pending(), "the queued automatic poll must cancel");
  manual_probe.loop();
  static esphome::sensor::Sensor single, total;
  static PauseProbe counters;
  counters.set_uart_parent(&uart);
  counters.set_single_coffee_sensor(&single);
  counters.set_total_coffee_sensor(&total);
  single.publish_state(1);
  total.publish_state(10);
  counters.setup();
  counters.queue_command("RE:00");
  counters.set_status_polling_paused(true);
  counters.poll_counters_();
  pause_check(counters.queued() == 1 && !counters.counter_pending(), "pause must cancel only automatic counter reads");
  counters.dispatch();
  counters.finish_transmission();
  counters.respond("re:0064");
  pause_check(single.state == 1 && total.state == 10, "manual EEPROM reads must not update automatic counters");
  counters.set_status_polling_paused(false);
  pause_check(counters.counter_pending() && counters.next_command() == "RE:00", "resume must restart counters");
  counters.dispatch();
  counters.set_status_polling_paused(true);
  pause_check(counters.acknowledgement_pending(), "pause must wait for the active counter read");
  counters.finish_transmission();
  counters.respond("re:0002");
  pause_check(single.state == 2 && total.state == 10, "pause must preserve the last complete total");
  pause_check(counters.queued() == 0 && !counters.counter_pending() && !counters.acknowledgement_pending(),
              "pause must stop the remaining counter sweep before acknowledging");
  counters.set_status_polling_paused(false);
  pause_check(counters.next_command() == "RE:00", "an interrupted sweep must restart from word 00");
  counters.set_status_polling_paused(true);
  counters.loop();
  using esphome::jura_f50::parse_clock_status;
  pause_check(parse_clock_status("rr:1338").value() == "19:56", "clock bytes must be binary, not BCD");
  pause_check(parse_clock_status("rr:140f").value() == "20:15", "clock must accept lowercase hex");
  pause_check(parse_clock_status("rr:173B").value() == "23:59", "clock must support the end of the day");
  pause_check(parse_clock_status("rr:0000").value() == "--:--", "zero must display as an unset clock");
  pause_check(parse_clock_status("rr:0001").value() == "00:01", "clock must zero-pad midnight minutes");
  for (const auto *bad : {"re:1410", "rr:141", "rr:14100", "rr:14GG", "rr:1800", "rr:003C"})
    pause_check(!parse_clock_status(bad).has_value(), "clock must reject malformed or out-of-range responses");
  static esphome::text_sensor::TextSensor clock;
  static PauseProbe clock_probe;
  clock_probe.set_uart_parent(&uart);
  clock_probe.set_clock_text_sensor(&clock);
  clock.publish_state("19:00");
  clock_probe.setup();
  clock_probe.poll_clock_();
  pause_check(clock_probe.queued() == 1 && clock_probe.clock_pending(), "clock must poll at startup without duplicates");
  clock_probe.queue_command("RR:BC");
  clock_probe.set_status_polling_paused(true);
  clock_probe.poll_clock_();
  pause_check(clock_probe.queued() == 1 && !clock_probe.clock_pending(), "pause must cancel only automatic clock reads");
  pause_check(clock.state == "19:00", "skipped clock polls must preserve the displayed time");
  clock_probe.dispatch();
  clock_probe.finish_transmission();
  clock_probe.respond("rr:1410");
  pause_check(clock.state == "19:00", "manual reads must not update the clock poll");
  clock_probe.set_status_polling_paused(false);
  pause_check(clock_probe.next_command() == "RR:BC", "resume must request a fresh clock");
  clock_probe.dispatch();
  clock_probe.finish_transmission();
  clock_probe.respond("rr:1410");
  pause_check(clock.state == "20:16" && !clock_probe.clock_pending(), "clock response must publish HH:MM");
  clock_probe.poll_clock_();
  clock_probe.dispatch();
  clock_probe.set_status_polling_paused(true);
  pause_check(clock_probe.clock_pending() && clock_probe.acknowledgement_pending(), "pause must drain an active clock poll");
  clock_probe.finish_transmission();
  clock_probe.respond("rr:0000");
  pause_check(clock.state == "--:--" && !clock_probe.acknowledgement_pending(), "active clock replies must finish while paused");
  clock_probe.set_status_polling_paused(false);
  clock_probe.dispatch();
  clock_probe.finish_transmission();
  clock_probe.respond("rr:1800");
  pause_check(clock.state == "unknown" && !clock_probe.clock_pending(), "invalid clock replies must report failure");
  clock_probe.poll_clock_();
  clock_probe.dispatch();
  clock_probe.finish_transmission();
  clock_probe.respond("rr:1411");
  pause_check(clock.state == "20:17", "clock must recover from a failed poll");
  clock_probe.poll_clock_();
  clock_probe.dispatch();
  clock_probe.finish_transmission();
  clock_probe.set_response_timeout_us(0);
  clock_probe.loop();
  pause_check(clock.state == "unknown" && !clock_probe.clock_pending(), "clock timeouts must clear the pending poll");
  clock_probe.set_status_polling_paused(true);
  clock_probe.loop();
  static esphome::text_sensor::TextSensor interval_clock;
  clock_interval_probe.set_uart_parent(&uart);
  clock_interval_probe.set_clock_text_sensor(&interval_clock);
  clock_interval_probe.setup();
  finish_clock_interval_poll();
  expiry_probe.set_status_polling_paused(true);
}
