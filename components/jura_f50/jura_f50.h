#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace jura_f50 {

// Pure encoder/decoder for the Jura service-port framing: one payload byte is
// spread over 4 UART bytes, 2 bits each, carried in bit positions 2 and 5.
// All other bits are 1.
using Frame = std::array<uint8_t, 4>;

Frame encode_byte(uint8_t value);
uint8_t decode_frame(const Frame &frame);

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

  void set_intra_byte_delay_us(uint32_t us) { this->intra_delay_us_ = us; }
  void set_inter_frame_delay_us(uint32_t us) { this->inter_delay_us_ = us; }
  void set_rx_timeout_us(uint32_t us) { this->rx_timeout_us_ = us; }

 protected:
  void process_rx_();
  void process_tx_();
  void handle_decoded_(uint8_t value);

  // Timing from the original sketch: 1 ms before each UART byte, 7 ms after a frame.
  uint32_t intra_delay_us_{1000};
  uint32_t inter_delay_us_{7000};
  // A partially received frame older than this is discarded to regain sync.
  uint32_t rx_timeout_us_{20000};

  // Active only while a transmission is pending, so the 1 ms / 7 ms pacing is honoured.
  HighFrequencyLoopRequester high_freq_;

  std::deque<uint8_t> tx_queue_;
  std::deque<std::string> command_queue_;
  Frame tx_frame_{};
  uint8_t tx_index_{4};  // 4 == no frame in flight
  uint32_t tx_last_us_{0};
  uint32_t tx_wait_us_{0};

  Frame rx_frame_{};
  uint8_t rx_index_{0};
  uint32_t rx_last_us_{0};

  std::string line_;
  CallbackManager<void(uint8_t)> byte_callbacks_;
  CallbackManager<void(const std::string &)> line_callbacks_;
};

class LineTrigger : public Trigger<std::string> {
 public:
  explicit LineTrigger(JuraF50 *parent) {
    parent->add_on_line_callback([this](const std::string &line) { this->trigger(line); });
  }
};

}  // namespace jura_f50
}  // namespace esphome
