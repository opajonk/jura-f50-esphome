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
  bool acknowledgement_pending() const { return this->status_pause_ack_pending_; }
  bool poll_pending() const { return this->status_pending_; }
  size_t queued() const { return this->command_queue_.size(); }
  uint8_t failures() const { return this->status_failures_; }
  bool counter_pending() const { return this->counter_pending_; }
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

inline void run_pause_tests(esphome::jura_f50::JuraF50 *configured_component) {
  static FakeUART uart;
  configured_component->set_uart_parent(&uart);
  configured_component->set_power_binary_sensor(nullptr);
  configured_component->set_single_coffee_sensor(nullptr);
  configured_component->set_total_coffee_sensor(nullptr);
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
  expiry_probe.set_status_polling_paused(true);
}
