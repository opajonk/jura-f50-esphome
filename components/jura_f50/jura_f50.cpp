#include "jura_f50.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace jura_f50 {

static const char *const TAG = "jura_f50";

// Payload bit 2*i goes to bit 2 of byte i, payload bit 2*i+1 to bit 5.
static constexpr uint8_t BIT_LOW = 2;
static constexpr uint8_t BIT_HIGH = 5;

Frame encode_byte(uint8_t value) {
  Frame frame;
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t b = 0xFF;
    if (!(value & (1 << (2 * i))))
      b &= ~(1 << BIT_LOW);
    if (!(value & (1 << (2 * i + 1))))
      b &= ~(1 << BIT_HIGH);
    frame[i] = b;
  }
  return frame;
}

uint8_t decode_frame(const Frame &frame) {
  uint8_t value = 0;
  for (uint8_t i = 0; i < 4; i++) {
    value |= ((frame[i] >> BIT_LOW) & 1) << (2 * i);
    value |= ((frame[i] >> BIT_HIGH) & 1) << (2 * i + 1);
  }
  return value;
}

void JuraF50::setup() {
  this->tx_last_us_ = micros();
  this->rx_last_us_ = micros();
}

void JuraF50::dump_config() {
  ESP_LOGCONFIG(TAG, "Jura F50:");
  ESP_LOGCONFIG(TAG, "  Intra-byte delay: %u us", (unsigned) this->intra_delay_us_);
  ESP_LOGCONFIG(TAG, "  Inter-frame delay: %u us", (unsigned) this->inter_delay_us_);
  ESP_LOGCONFIG(TAG, "  RX timeout: %u us", (unsigned) this->rx_timeout_us_);
}

void JuraF50::loop() {
  this->process_rx_();
  this->process_tx_();
}

void JuraF50::write_payload(uint8_t value) {
  this->tx_queue_.push_back(value);
  this->high_freq_.start();
}

void JuraF50::write_payload(const std::string &data) {
  for (char c : data)
    this->tx_queue_.push_back(static_cast<uint8_t>(c));
  this->high_freq_.start();
}

void JuraF50::send_command(const std::string &command) {
  this->write_payload(command);
  this->write_payload("\r\n");
}

void JuraF50::queue_command(const std::string &command) {
  this->command_queue_.push_back(command);
  this->high_freq_.start();
}

void JuraF50::process_tx_() {
  if (this->tx_index_ >= 4) {
    // Dispatch the next queued command only once the previous one is fully sent.
    if (this->tx_queue_.empty() && !this->command_queue_.empty()) {
      this->send_command(this->command_queue_.front());
      this->command_queue_.pop_front();
    }
    if (this->tx_queue_.empty()) {
      // Let the final inter-frame gap elapse before releasing the fast loop.
      if ((uint32_t) (micros() - this->tx_last_us_) >= this->tx_wait_us_)
        this->high_freq_.stop();
      return;
    }
    this->high_freq_.start();
    this->tx_frame_ = encode_byte(this->tx_queue_.front());
    this->tx_queue_.pop_front();
    this->tx_index_ = 0;
  }
  uint32_t now = micros();
  if ((uint32_t) (now - this->tx_last_us_) < this->tx_wait_us_)
    return;

  // Same sequence as the sketch: wait intra, write byte; after byte 4 wait inter.
  this->write_byte(this->tx_frame_[this->tx_index_++]);
  this->tx_last_us_ = now;
  this->tx_wait_us_ = this->tx_index_ >= 4 ? this->inter_delay_us_ : this->intra_delay_us_;
}

void JuraF50::process_rx_() {
  uint32_t now = micros();
  if (this->rx_index_ > 0 && (uint32_t) (now - this->rx_last_us_) > this->rx_timeout_us_) {
    ESP_LOGW(TAG, "Dropping incomplete frame (%u/4 bytes)", this->rx_index_);
    this->rx_index_ = 0;
  }
  uint8_t b;
  while (this->available() && this->read_byte(&b)) {
    this->rx_frame_[this->rx_index_++] = b;
    this->rx_last_us_ = micros();
    if (this->rx_index_ == 4) {
      this->rx_index_ = 0;
      this->handle_decoded_(decode_frame(this->rx_frame_));
    }
  }
}

void JuraF50::handle_decoded_(uint8_t value) {
  ESP_LOGV(TAG, "RX 0x%02X", value);
  this->byte_callbacks_.call(value);
  if (value == '\n') {
    if (!this->line_.empty() && this->line_.back() == '\r')
      this->line_.pop_back();
    this->line_callbacks_.call(this->line_);
    this->line_.clear();
  } else {
    this->line_.push_back(static_cast<char>(value));
  }
}

}  // namespace jura_f50
}  // namespace esphome
