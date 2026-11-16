#pragma once

#include <cmath>
#include <cstdlib>
#include "esphome/components/jura_f50/jura_f50.h"

class CounterTestJura : public esphome::jura_f50::JuraF50 {
 public:
  using JuraF50::handle_counter_;
  using JuraF50::poll_counters_;

  std::string take_command() {
    if (this->command_queue_.empty())
      std::abort();
    auto command = this->command_queue_.front().command;
    this->command_queue_.pop_front();
    return command;
  }
  bool pending() const { return this->counter_pending_; }
};

inline void run_counter_tests() {
  using esphome::jura_f50::parse_eeprom_word;
  auto check = [](bool ok) {
    if (!ok)
      std::abort();
  };
  check(parse_eeprom_word("re:4da2").value() == 19874);
  check(parse_eeprom_word("re:FFFF").value() == 65535);
  for (const auto *bad : {"rr:4DA2", "re:re:4DA2", "re:4DG2", "re:4DA", "re:4DA20"})
    check(!parse_eeprom_word(bad).has_value());

  esphome::sensor::Sensor single, total;
  CounterTestJura hub;
  hub.set_single_coffee_sensor(&single);
  hub.set_total_coffee_sensor(&total);
  hub.poll_counters_();
  check(hub.take_command() == "RE:00");
  hub.handle_counter_("RE:00", {});
  check(!single.has_state() && !total.has_state() && !hub.pending());

  hub.poll_counters_();
  hub.poll_counters_();  // Must not start an overlapping sweep.
  check(hub.take_command() == "RE:00");
  hub.handle_counter_("RR:44", {});
  check(hub.pending());
  hub.handle_counter_("RE:00", 19874);
  check(single.state == 19874 && !total.has_state());
  check(hub.take_command() == "RE:01");
  hub.handle_counter_("RE:01", 6735);
  check(hub.take_command() == "RE:02");
  hub.handle_counter_("RE:02", 8204);
  check(hub.take_command() == "RE:03");
  hub.handle_counter_("RE:03", 749);
  check(hub.take_command() == "RE:04");
  hub.handle_counter_("RE:04", 0);
  check(hub.take_command() == "RE:06");
  hub.handle_counter_("RE:06", 128);
  check(total.state == 35690 && !hub.pending());

  hub.poll_counters_();
  check(hub.take_command() == "RE:00");
  hub.handle_counter_("RE:00", 19875);
  check(hub.take_command() == "RE:01");
  hub.handle_counter_("RE:01", {});
  check(total.state == 35690 && single.state == 19875 && !hub.pending());

  hub.poll_counters_();
  check(hub.take_command() == "RE:00");
  hub.handle_counter_("RE:00", {});
  check(single.state == 19875 && total.state == 35690 && !hub.pending());

  // Recovery starts a fresh sum, and six 16-bit counters must not overflow it.
  hub.poll_counters_();
  for (const auto *command : {"RE:00", "RE:01", "RE:02", "RE:03", "RE:04", "RE:06"}) {
    check(hub.take_command() == command);
    hub.handle_counter_(command, 65535);
  }
  check(total.state == 393210 && !hub.pending());

  CounterTestJura single_only;
  single_only.set_single_coffee_sensor(&single);
  single_only.poll_counters_();
  check(single_only.take_command() == "RE:00");
  single_only.handle_counter_("RE:00", 19874);
  check(single.state == 19874 && !single_only.pending());

  CounterTestJura total_only;
  total_only.set_total_coffee_sensor(&total);
  total_only.poll_counters_();
  for (const auto *command : {"RE:00", "RE:01", "RE:02", "RE:03", "RE:04", "RE:06"}) {
    check(total_only.take_command() == command);
    total_only.handle_counter_(command, 1);
  }
  check(total.state == 6 && !total_only.pending());
}
