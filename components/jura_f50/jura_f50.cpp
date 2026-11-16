#include "jura_f50.h"

#include <cctype>
#include <cmath>

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

// Consecutive failed status polls before the power state is reported as unknown.
static constexpr uint8_t STATUS_MAX_FAILURES = 5;

optional<bool> parse_power_status(const std::string &response) {
  if (response.size() != 7 || response.compare(0, 3, "rr:") != 0)
    return {};
  for (size_t i = 3; i < response.size(); i++) {
    if (!isxdigit(static_cast<unsigned char>(response[i])))
      return {};
  }
  return response[3] != '0' || response[4] != '0';
}

static optional<uint16_t> parse_word(const std::string &response, const char *prefix) {
  if (response.size() != 7 || response.compare(0, 3, prefix) != 0)
    return {};
  uint16_t word = 0;
  for (size_t i = 3; i < response.size(); i++) {
    unsigned char c = response[i];
    if (!isxdigit(c))
      return {};
    word = (word << 4) | (isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
  }
  return word;
}

optional<uint16_t> parse_eeprom_word(const std::string &response) { return parse_word(response, "re:"); }

optional<std::string> parse_clock_status(const std::string &response) {
  auto word = parse_word(response, "rr:");
  if (!word.has_value())
    return {};
  unsigned hours = *word >> 8;
  unsigned minutes = *word & 0xFF;
  if (hours > 23 || minutes > 59)
    return {};
  // Unset clocks and midnight both encode as zero.
  if (*word == 0)
    return std::string("--:--");
  return str_sprintf("%02u:%02u", hours, minutes);
}

void JuraF50::setup() {
  this->tx_last_us_ = micros();
  this->rx_last_us_ = micros();
#ifdef USE_BINARY_SENSOR
  if (this->power_sensor_ != nullptr) {
    this->add_on_response_callback([this](const std::string &command, const std::string &response) {
      this->handle_status_(command, parse_power_status(response));
    });
    this->add_on_response_timeout_callback([this](const std::string &command) { this->handle_status_(command, {}); });
    this->set_interval("status", this->status_interval_ms_, [this]() { this->poll_status_(); });
    this->poll_status_();
  }
#endif
#ifdef USE_SENSOR
  if (this->single_coffee_sensor_ != nullptr || this->total_coffee_sensor_ != nullptr) {
    this->add_on_response_callback([this](const std::string &command, const std::string &response) {
      if (this->pending_command_source_ == CommandSource::COUNTER_POLL)
        this->handle_counter_(command, parse_eeprom_word(response));
    });
    this->add_on_response_timeout_callback([this](const std::string &command) {
      if (this->pending_command_source_ == CommandSource::COUNTER_POLL)
        this->handle_counter_(command, {});
    });
    this->set_interval("counters", this->counter_interval_ms_, [this]() { this->poll_counters_(); });
    this->poll_counters_();
  }
#endif
#ifdef USE_TEXT_SENSOR
  if (this->clock_sensor_ != nullptr) {
    this->add_on_response_callback([this](const std::string &command, const std::string &response) {
      if (this->pending_command_source_ == CommandSource::CLOCK_POLL && command == CLOCK_STATUS_COMMAND)
        this->handle_clock_(parse_clock_status(response));
    });
    this->add_on_response_timeout_callback([this](const std::string &command) {
      if (this->pending_command_source_ == CommandSource::CLOCK_POLL && command == CLOCK_STATUS_COMMAND)
        this->handle_clock_({});
    });
    this->set_interval("clock", this->clock_interval_ms_, [this]() { this->poll_clock_(); });
    this->poll_clock_();
  }
#endif
}

#ifdef USE_TEXT_SENSOR
void JuraF50::poll_clock_() {
  if (this->status_polling_paused_ || this->clock_pending_)
    return;
  this->clock_pending_ = true;
  this->command_queue_.push_back({CLOCK_STATUS_COMMAND, CommandSource::CLOCK_POLL});
  this->high_freq_.start();
}

void JuraF50::handle_clock_(const optional<std::string> &clock) {
  this->clock_pending_ = false;
  if (clock.has_value()) {
    this->clock_sensor_->publish_state(*clock);
    return;
  }
  ESP_LOGW(TAG, "Clock status poll failed");
  this->clock_sensor_->publish_state("unknown");
}
#endif

#ifdef USE_SENSOR
static const char *const COUNTER_COMMANDS[] = {"RE:00", "RE:01", "RE:02", "RE:03", "RE:04", "RE:06"};

void JuraF50::poll_counters_() {
  if (this->status_polling_paused_ || this->counter_pending_)
    return;
  this->counter_pending_ = true;
  this->counter_index_ = 0;
  this->counter_total_ = 0;
  this->queue_counter_read_();
}

void JuraF50::queue_counter_read_() {
  this->command_queue_.push_back({COUNTER_COMMANDS[this->counter_index_], CommandSource::COUNTER_POLL});
  this->high_freq_.start();
}

void JuraF50::handle_counter_(const std::string &command, const optional<uint16_t> &word) {
  if (!this->counter_pending_ || command != COUNTER_COMMANDS[this->counter_index_])
    return;
  if (!word.has_value()) {
    ESP_LOGW(TAG, "Coffee counter read failed for %s; retaining last successful values", command.c_str());
    this->counter_pending_ = false;
    return;
  }
  if (this->counter_index_ == 0 && this->single_coffee_sensor_ != nullptr)
    this->single_coffee_sensor_->publish_state(*word);
  this->counter_total_ += *word;
  if (this->total_coffee_sensor_ != nullptr && ++this->counter_index_ < sizeof(COUNTER_COMMANDS) / sizeof(*COUNTER_COMMANDS)) {
    if (this->status_polling_paused_) {
      this->counter_pending_ = false;
      return;
    }
    this->queue_counter_read_();
    return;
  }
  if (this->total_coffee_sensor_ != nullptr)
    this->total_coffee_sensor_->publish_state(this->counter_total_);
  this->counter_pending_ = false;
}
#endif

void JuraF50::poll_status_() {
  if (this->status_polling_paused_ || this->status_pending_)
    return;
  this->status_pending_ = true;
  this->command_queue_.push_back({POWER_STATUS_COMMAND, CommandSource::POWER_POLL});
  this->high_freq_.start();
}

void JuraF50::set_status_polling_paused(bool paused) {
  this->cancel_timeout("status_pause");
  if (!paused) {
    bool was_paused = this->status_polling_paused_;
    this->status_polling_paused_ = false;
    this->status_pause_ack_pending_ = false;
    ESP_LOGI(TAG, "Status polling resumed");
#ifdef USE_BINARY_SENSOR
    if (was_paused && this->power_sensor_ != nullptr)
      this->poll_status_();
#endif
#ifdef USE_SENSOR
    if (was_paused && (this->single_coffee_sensor_ != nullptr || this->total_coffee_sensor_ != nullptr))
      this->poll_counters_();
#endif
#ifdef USE_TEXT_SENSOR
    if (was_paused && this->clock_sensor_ != nullptr)
      this->poll_clock_();
#endif
    return;
  }
  this->status_polling_paused_ = true;
  this->status_pause_ack_pending_ = true;
  for (auto it = this->command_queue_.begin(); it != this->command_queue_.end();) {
    if (it->source != CommandSource::MANUAL) {
      if (it->source == CommandSource::POWER_POLL)
        this->status_pending_ = false;
#ifdef USE_SENSOR
      if (it->source == CommandSource::COUNTER_POLL)
        this->counter_pending_ = false;
#endif
#ifdef USE_TEXT_SENSOR
      if (it->source == CommandSource::CLOCK_POLL)
        this->clock_pending_ = false;
#endif
      it = this->command_queue_.erase(it);
    } else {
      ++it;
    }
  }
  this->set_timeout("status_pause", 60000, [this]() { this->set_status_polling_paused(false); });
  ESP_LOGI(TAG, "Status polling pause requested (expires in 60s)");
  this->acknowledge_status_pause_();
}

void JuraF50::acknowledge_status_pause_() {
  if (this->status_pause_ack_pending_ && !this->awaiting_response_ && this->tx_queue_.empty() &&
      this->tx_index_ >= 4 && this->command_queue_.empty()) {
    this->status_pause_ack_pending_ = false;
    ESP_LOGI(TAG, "Status polling pause ready");
  }
}

void JuraF50::handle_status_(const std::string &command, const optional<bool> &power) {
  if (command != POWER_STATUS_COMMAND)
    return;
  if (this->pending_command_source_ == CommandSource::POWER_POLL)
    this->status_pending_ = false;
#ifdef USE_BINARY_SENSOR
  if (power.has_value()) {
    this->status_failures_ = 0;
    this->power_sensor_->publish_state(*power);
    return;
  }
  ESP_LOGW(TAG, "Power status poll failed");
  if (this->status_failures_ < STATUS_MAX_FAILURES && ++this->status_failures_ == STATUS_MAX_FAILURES)
    this->power_sensor_->invalidate_state();
#endif
}

void JuraF50::dump_config() {
  ESP_LOGCONFIG(TAG, "Jura F50:");
  ESP_LOGCONFIG(TAG, "  Intra-byte delay: %u us", (unsigned) this->intra_delay_us_);
  ESP_LOGCONFIG(TAG, "  Inter-frame delay: %u us", (unsigned) this->inter_delay_us_);
  ESP_LOGCONFIG(TAG, "  RX timeout: %u us", (unsigned) this->rx_timeout_us_);
  ESP_LOGCONFIG(TAG, "  Response timeout: %u us", (unsigned) this->response_timeout_us_);
#ifdef USE_BINARY_SENSOR
  if (this->power_sensor_ != nullptr) {
    LOG_BINARY_SENSOR("  ", "Power", this->power_sensor_);
    ESP_LOGCONFIG(TAG, "  Status interval: %u ms", (unsigned) this->status_interval_ms_);
  }
#endif
#ifdef USE_SENSOR
  if (this->single_coffee_sensor_ != nullptr || this->total_coffee_sensor_ != nullptr) {
    LOG_SENSOR("  ", "Single coffee counter", this->single_coffee_sensor_);
    LOG_SENSOR("  ", "Total coffee counter", this->total_coffee_sensor_);
    ESP_LOGCONFIG(TAG, "  Counter interval: %u ms", (unsigned) this->counter_interval_ms_);
  }
#endif
#ifdef USE_TEXT_SENSOR
  if (this->clock_sensor_ != nullptr) {
    LOG_TEXT_SENSOR("  ", "Clock", this->clock_sensor_);
    ESP_LOGCONFIG(TAG, "  Clock interval: %u ms", (unsigned) this->clock_interval_ms_);
  }
#endif
}

void JuraF50::loop() {
  this->process_rx_();
  this->check_response_timeout_();
  this->process_tx_();
  this->acknowledge_status_pause_();
}

void JuraF50::check_response_timeout_() {
  if (!this->awaiting_response_)
    return;
  if ((uint32_t) (micros() - this->pending_since_us_) < this->response_timeout_us_)
    return;
  ESP_LOGW(TAG, "No response to '%s' within timeout", this->pending_command_.c_str());
  this->awaiting_response_ = false;
  this->response_timeout_callbacks_.call(this->pending_command_);
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
  ESP_LOGD(TAG, "TX command: %s", command.c_str());
  this->write_payload(command);
  this->write_payload("\r\n");
}

void JuraF50::queue_command(const std::string &command) {
  ESP_LOGD(TAG, "Queueing command: %s", command.c_str());
  this->command_queue_.push_back({command, CommandSource::MANUAL});
  this->high_freq_.start();
}

void JuraF50::process_tx_() {
  if (this->tx_index_ >= 4) {
    // Dispatch the next queued command only once the previous one is fully sent
    // and its response has arrived or timed out.
    if (this->tx_queue_.empty() && !this->command_queue_.empty() && !this->awaiting_response_) {
      this->pending_command_ = this->command_queue_.front().command;
      this->pending_command_source_ = this->command_queue_.front().source;
      this->awaiting_response_ = true;
      this->pending_since_us_ = micros();
      this->send_command(this->pending_command_);
      this->command_queue_.pop_front();
    }
    if (this->tx_queue_.empty()) {
      // Let the final inter-frame gap elapse before releasing the fast loop.
      if ((uint32_t) (micros() - this->tx_last_us_) >= this->tx_wait_us_)
        this->high_freq_.stop();
      return;
    }
    this->high_freq_.start();
    uint8_t payload = this->tx_queue_.front();
    ESP_LOGV(TAG, "TX byte 0x%02X ('%c')", payload, isprint(payload) ? (char) payload : '.');
    this->tx_frame_ = encode_byte(payload);
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
  ESP_LOGV(TAG, "RX byte 0x%02X ('%c')", value, isprint(value) ? (char) value : '.');
  this->byte_callbacks_.call(value);
  if (value == '\n') {
    if (!this->line_.empty() && this->line_.back() == '\r')
      this->line_.pop_back();
    ESP_LOGD(TAG, "RX line: %s", this->line_.c_str());
    this->line_callbacks_.call(this->line_);
    if (this->awaiting_response_) {
      this->awaiting_response_ = false;
      this->response_callbacks_.call(this->pending_command_, this->line_);
    }
    this->line_.clear();
  } else {
    this->line_.push_back(static_cast<char>(value));
  }
}

}  // namespace jura_f50
}  // namespace esphome
