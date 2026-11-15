#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/optional.h"
#include "esphome/components/uart/uart.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome {
namespace jura_f50 {

// Pure encoder/decoder for the Jura service-port framing: one payload byte is
// spread over 4 UART bytes, 2 bits each, carried in bit positions 2 and 5.
// All other bits are 1.
using Frame = std::array<uint8_t, 4>;

Frame encode_byte(uint8_t value);
uint8_t decode_frame(const Frame &frame);

// RR:44 reads RAM bytes 0x44/0x45; byte 0x44 is 01 while the machine is on and 00 while it is off
// (observed on an EPSILON MASK 11 across off, heating, rinsing and ready states).
static const char *const POWER_STATUS_COMMAND = "RR:44";
// RAM bytes 0xBC/0xBD hold binary hours/minutes on the tested EPSILON MASK 11.
static const char *const CLOCK_STATUS_COMMAND = "RR:BC";
// Returns the power state encoded in a "rr:XXYY" reply, or nullopt if the reply is malformed.
optional<bool> parse_power_status(const std::string &response);
optional<uint16_t> parse_eeprom_word(const std::string &response);
optional<std::string> parse_clock_status(const std::string &response);

class JuraF50 : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Queue a raw payload byte / string / command (command gets "\r\n" appended).
  // Non-blocking: bytes are paced out of loop().
  void write_payload(uint8_t value);
  void write_payload(const std::string &data);
  void send_command(const std::string &command);
  // Queue whole commands; each is passed to send_command() once the previous one has been fully sent.
  void queue_command(const std::string &command);

  // Called for each decoded byte and for each complete line (CR/LF stripped).
  void add_on_byte_callback(std::function<void(uint8_t)> &&cb) { this->byte_callbacks_.add(std::move(cb)); }
  void add_on_line_callback(std::function<void(const std::string &)> &&cb) {
    this->line_callbacks_.add(std::move(cb));
  }
  // Called with (command, response) when a line arrives while a queued command is awaiting a reply.
  void add_on_response_callback(std::function<void(const std::string &, const std::string &)> &&cb) {
    this->response_callbacks_.add(std::move(cb));
  }
  // Called with the command when no response arrived within the response timeout.
  void add_on_response_timeout_callback(std::function<void(const std::string &)> &&cb) {
    this->response_timeout_callbacks_.add(std::move(cb));
  }

  void set_intra_byte_delay_us(uint32_t us) { this->intra_delay_us_ = us; }
  void set_inter_frame_delay_us(uint32_t us) { this->inter_delay_us_ = us; }
  void set_rx_timeout_us(uint32_t us) { this->rx_timeout_us_ = us; }
  void set_response_timeout_us(uint32_t us) { this->response_timeout_us_ = us; }
  void set_status_interval_ms(uint32_t ms) { this->status_interval_ms_ = ms; }
  // Setting true also renews the 60-second pause; false resumes immediately.
  void set_status_polling_paused(bool paused);
  bool is_status_polling_paused() const { return this->status_polling_paused_; }
#ifdef USE_BINARY_SENSOR
  void set_power_binary_sensor(binary_sensor::BinarySensor *sensor) { this->power_sensor_ = sensor; }
#endif
#ifdef USE_SENSOR
  void set_single_coffee_sensor(sensor::Sensor *sensor) { this->single_coffee_sensor_ = sensor; }
  void set_total_coffee_sensor(sensor::Sensor *sensor) { this->total_coffee_sensor_ = sensor; }
  void set_counter_interval_ms(uint32_t ms) { this->counter_interval_ms_ = ms; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_clock_text_sensor(text_sensor::TextSensor *sensor) { this->clock_sensor_ = sensor; }
  void set_clock_interval_ms(uint32_t ms) { this->clock_interval_ms_ = ms; }
#endif

 protected:
  void process_rx_();
  void process_tx_();
  void check_response_timeout_();
  void handle_decoded_(uint8_t value);
  void poll_status_();
  void handle_status_(const std::string &command, const optional<bool> &power);
  void acknowledge_status_pause_();
#ifdef USE_SENSOR
  void poll_counters_();
  void queue_counter_read_();
  void handle_counter_(const std::string &command, const optional<uint16_t> &word);
#endif
#ifdef USE_TEXT_SENSOR
  void poll_clock_();
  void handle_clock_(const optional<std::string> &clock);
#endif

  // Timing from the original sketch: 1 ms before each UART byte, 7 ms after a frame.
  uint32_t intra_delay_us_{1000};
  uint32_t inter_delay_us_{7000};
  // A partially received frame older than this is discarded to regain sync.
  uint32_t rx_timeout_us_{20000};
  // A queued command with no reply within this window is reported via the response-timeout callback.
  uint32_t response_timeout_us_{1000000};

  // Active only while a transmission is pending, so the 1 ms / 7 ms pacing is honoured.
  HighFrequencyLoopRequester high_freq_;

  std::deque<uint8_t> tx_queue_;
  enum class CommandSource { MANUAL, POWER_POLL, COUNTER_POLL, CLOCK_POLL };
  struct QueuedCommand {
    std::string command;
    CommandSource source;
  };
  std::deque<QueuedCommand> command_queue_;
  Frame tx_frame_{};
  uint8_t tx_index_{4};  // 4 == no frame in flight
  uint32_t tx_last_us_{0};
  uint32_t tx_wait_us_{0};

  Frame rx_frame_{};
  uint8_t rx_index_{0};
  uint32_t rx_last_us_{0};

  std::string line_;
  std::string pending_command_;
  bool awaiting_response_{false};
  CommandSource pending_command_source_{CommandSource::MANUAL};
  uint32_t pending_since_us_{0};
  uint32_t status_interval_ms_{10000};
  bool status_pending_{false};
  bool status_polling_paused_{false};
  bool status_pause_ack_pending_{false};
  uint8_t status_failures_{0};
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *power_sensor_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *single_coffee_sensor_{nullptr};
  sensor::Sensor *total_coffee_sensor_{nullptr};
  uint32_t counter_interval_ms_{120000};
  bool counter_pending_{false};
  uint8_t counter_index_{0};
  uint32_t counter_total_{0};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *clock_sensor_{nullptr};
  uint32_t clock_interval_ms_{30000};
  bool clock_pending_{false};
#endif
  CallbackManager<void(uint8_t)> byte_callbacks_;
  CallbackManager<void(const std::string &)> line_callbacks_;
  CallbackManager<void(const std::string &, const std::string &)> response_callbacks_;
  CallbackManager<void(const std::string &)> response_timeout_callbacks_;
};

class LineTrigger : public Trigger<std::string> {
 public:
  explicit LineTrigger(JuraF50 *parent) {
    parent->add_on_line_callback([this](const std::string &line) { this->trigger(line); });
  }
};

class ResponseTrigger : public Trigger<std::string, std::string> {
 public:
  explicit ResponseTrigger(JuraF50 *parent) {
    parent->add_on_response_callback(
        [this](const std::string &command, const std::string &response) { this->trigger(command, response); });
  }
};

class ResponseTimeoutTrigger : public Trigger<std::string> {
 public:
  explicit ResponseTimeoutTrigger(JuraF50 *parent) {
    parent->add_on_response_timeout_callback([this](const std::string &command) { this->trigger(command); });
  }
};

}  // namespace jura_f50
}  // namespace esphome
