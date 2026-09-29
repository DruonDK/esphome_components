#pragma once

#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/button/button.h"
#include "esphome/core/component.h"

#include <vector>
#include <map>
#include <set>
#include <cmath>
#include <string>

namespace esphome {
namespace sensor { class Sensor; }
namespace switch_ { class Switch; }
namespace button { class Button; }
namespace wavinahc9000v3 {

// Forward
class WavinZoneClimate;
class WavinLockSwitch;
class WavinYamlDumpButton;
class WavinRepairButton;

enum class ModuleProfile : uint8_t {
  MODULE_DEFAULT = 0,
  MODULE_USTEPPER = 1,
};

static constexpr ModuleProfile MODULE_DEFAULT = ModuleProfile::MODULE_DEFAULT;
static constexpr ModuleProfile MODULE_USTEPPER = ModuleProfile::MODULE_USTEPPER;

// Which lock bit of the PACKED CONFIGURATION register a lock switch controls.
// The "AC-116 Modbus Register Map" (Jablotron, 18.10.2013) section 1.6.9 documents two bits:
//   INT LOCK  (bit 11): "user is prevented to enter the service menu"
//   CTRL LOCK (bit 10): "user is prevented to make any changes"
// Verified on a Wavin AHC 9000 with wireless Wavin room thermostats (29-09-2026): bit 11 is the
// thermostat's own lock ("LOc" in its menu, padlock symbol in the display, dial disabled). Locking the
// thermostat locally sets bit 11 in the controller, and setting bit 11 from the bus locks the thermostat
// a few minutes later. Bit 10 alone does not lock the dial; it blocks the thermostat's menu, and with both bits
// set the thermostat cannot be unlocked locally at all (that part matches the register map's note). So the child
// lock is INT LOCK, contrary to what the register names suggest, and CTRL LOCK is the menu lock.
enum class LockKind : uint8_t {
  LOCK_CHILD = 0,  // INT LOCK, bit 11
  LOCK_CTRL = 1,   // CTRL LOCK, bit 10
};

static constexpr LockKind LOCK_CHILD = LockKind::LOCK_CHILD;
static constexpr LockKind LOCK_CTRL = LockKind::LOCK_CTRL;

class WavinAHC9000 : public PollingComponent, public uart::UARTDevice {
 public:
  void set_temp_divisor(float d) { this->temp_divisor_ = d; }
  void set_receive_timeout_ms(uint32_t t) { this->receive_timeout_ms_ = t; }
  void set_tx_enable_pin(GPIOPin *p) { this->tx_enable_pin_ = p; }
  // Optional half-duplex RS485 DE/RE (flow control) pin. If provided we drive HIGH to transmit and LOW to receive.
  void set_flow_control_pin(GPIOPin *p) { this->flow_control_pin_ = p; }
  void set_module_profile(ModuleProfile profile) { this->module_profile_ = profile; }
  void set_poll_channels_per_cycle(uint8_t n) { this->poll_channels_per_cycle_ = n == 0 ? 1 : (n > 16 ? 16 : n); }
  void set_allow_mode_writes(bool v) { this->allow_mode_writes_ = v; }
  void set_keep_standby_alive(bool v);
  void set_standby_keepalive_interval(uint32_t ms);
  bool get_allow_mode_writes() const { return this->allow_mode_writes_; }
  // Friendly name support (optional per-channel overrides for generated YAML)
  void set_channel_friendly_name(uint8_t channel, const std::string &name);
  std::string get_channel_friendly_name(uint8_t channel) const;

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  void add_channel_climate(WavinZoneClimate *c);
  void add_group_climate(WavinZoneClimate *c);
  void add_channel_battery_sensor(uint8_t ch, sensor::Sensor *s);
  void add_channel_temperature_sensor(uint8_t ch, sensor::Sensor *s);
  void add_channel_comfort_setpoint_sensor(uint8_t ch, sensor::Sensor *s);
  void add_channel_floor_temperature_sensor(uint8_t ch, sensor::Sensor *s);
  // New read-only floor limit sensors
  void add_channel_floor_min_temperature_sensor(uint8_t ch, sensor::Sensor *s);
  void add_channel_floor_max_temperature_sensor(uint8_t ch, sensor::Sensor *s);
  void add_channel_lock_switch(WavinLockSwitch *s) { this->lock_switches_.push_back(s); }
  void add_active_channel(uint8_t ch);

  // Send commands
  void write_channel_setpoint(uint8_t channel, float celsius);
  void write_group_setpoint(const std::vector<uint8_t> &members, float celsius);
  void write_channel_mode(uint8_t channel, climate::ClimateMode mode);
  // Set or clear one lock bit (INT LOCK or CTRL LOCK) without touching the rest of the register.
  void write_channel_lock(uint8_t channel, LockKind kind, bool enable);
  void write_channel_child_lock(uint8_t channel, bool enable) { this->write_channel_lock(channel, LockKind::LOCK_CHILD, enable); }
  // Write floor temperature limits (Celsius), clamped to sane bounds
  void write_channel_floor_min_temperature(uint8_t channel, float celsius);
  void write_channel_floor_max_temperature(uint8_t channel, float celsius);
  void refresh_channel_now(uint8_t channel);
  void set_strict_mode_write(uint8_t channel, bool enable);
  bool is_strict_mode_write(uint8_t channel) const;
  void request_status();
  void request_status_channel(uint8_t ch_index);
  void normalize_channel_config(uint8_t channel, bool off);
  void generate_yaml_suggestion();
  // Debug helper to dump registers for a channel (to identify floor min/max addresses)
  void dump_channel_floor_limits(uint8_t channel);
  bool is_channel_locked(uint8_t ch, LockKind kind) const;
  bool is_channel_child_locked(uint8_t ch) const { return this->is_channel_locked(ch, LockKind::LOCK_CHILD); }
  bool is_channel_ctrl_locked(uint8_t ch) const { return this->is_channel_locked(ch, LockKind::LOCK_CTRL); }

  // Data access
  float get_channel_current_temp(uint8_t channel) const;
  float get_channel_setpoint(uint8_t channel) const;
  float get_channel_floor_temp(uint8_t channel) const;
  float get_channel_floor_min_temp(uint8_t channel) const;
  float get_channel_floor_max_temp(uint8_t channel) const;
  climate::ClimateMode get_channel_mode(uint8_t channel) const;
  climate::ClimateAction get_channel_action(uint8_t channel) const;

 protected:
  // Result of waiting for a response frame
  enum class RxResult : uint8_t { OK, TIMEOUT, CRC_ERROR, EXCEPTION };

  // Low-level protocol helpers (framing per "AC-116 Modbus Register Map", chapter 4)
  bool read_registers(uint8_t category, uint8_t page, uint8_t index, uint8_t count, std::vector<uint16_t> &out);
  bool write_register(uint8_t category, uint8_t page, uint8_t index, uint16_t value);
  // Masked write (function code 0x45): result is (reg & and_mask) | or_mask.
  // The wire format expected by the controller (DATA + MASK, "1 in MASK keeps the bit") is derived from these.
  bool write_masked_register(uint8_t category, uint8_t page, uint8_t index, uint16_t and_mask, uint16_t or_mask);
  // Wait for a response to function code `fc`. Recognises exception frames (fc | 0x80) and reports
  // their exception code instead of letting them time out.
  RxResult receive_frame_(uint8_t fc, std::vector<uint8_t> &buf, uint8_t &exception_code);

  void publish_updates();
  void prepare_for_tx_();
  void finish_tx_();
  void clear_stale_rx_();
  void handle_standby_keepalive_(uint8_t channel, bool is_off, std::vector<uint8_t> &reassert_list);
  // Inter-frame silence to ensure bus turnaround between consecutive transactions
  void inter_frame_delay_();
  bool standby_keepalive_enabled_() const {
    return this->keep_standby_alive_ && this->standby_keepalive_interval_ms_ > 0;
  }

  // Helpers
  float raw_to_c(float raw) const { return raw / this->temp_divisor_; }
  uint16_t c_to_raw(float c) const { return static_cast<uint16_t>(c * this->temp_divisor_ + 0.5f); }
  // Temperature registers report 0x7FFF when the value is unknown (spec 1.3.6 / 1.3.7)
  float raw_temp_to_c_(uint16_t raw) const { return raw == TEMP_UNKNOWN_RAW ? NAN : this->raw_to_c((float) raw); }
  // Build a new CONFIGURATION value for the requested mode, preserving every bit except MODE[2:0] and SCHED ENA
  uint16_t compose_mode_config_(uint16_t current, climate::ClimateMode mode) const;
  static uint16_t lock_mask_for_(LockKind kind);

  // Simple cache per channel
  struct ChannelState {
    float current_temp_c{NAN};
    float floor_temp_c{NAN};
    // New read-only floor limits (Celsius)
    float floor_min_c{NAN};
    float floor_max_c{NAN};
    float setpoint_c{NAN};
    climate::ClimateMode mode{climate::CLIMATE_MODE_HEAT};
    climate::ClimateAction action{climate::CLIMATE_ACTION_OFF};
    uint8_t battery_pct{255}; // 0..100; 255=unknown
    uint16_t primary_index{0};
    bool all_tp_lost{false};
    bool has_floor_sensor{false};
    bool int_lock{false};   // INT LOCK (bit 11): the thermostat's own lock, i.e. the child lock
    bool ctrl_lock{false};  // CTRL LOCK (bit 10): menu lock; with INT LOCK the thermostat cannot be unlocked locally
  };

  // Decode a freshly read CONFIGURATION register into the channel cache; returns true when the channel is in standby (OFF)
  bool apply_packed_configuration_(ChannelState &st, uint16_t raw_cfg);

  std::map<uint8_t, ChannelState> channels_;
  std::vector<WavinZoneClimate *> single_ch_climates_;
  std::vector<WavinZoneClimate *> group_climates_;
  std::map<uint8_t, sensor::Sensor *> battery_sensors_;
  std::map<uint8_t, sensor::Sensor *> temperature_sensors_;
  std::map<uint8_t, sensor::Sensor *> floor_temperature_sensors_;
  // New read-only floor limit sensor maps
  std::map<uint8_t, sensor::Sensor *> floor_min_temperature_sensors_;
  std::map<uint8_t, sensor::Sensor *> floor_max_temperature_sensors_;
  std::map<uint8_t, sensor::Sensor *> comfort_setpoint_sensors_;
  std::vector<WavinLockSwitch *> lock_switches_;
  std::vector<std::string> channel_friendly_names_; // 1-based index mapping (size >=17)
  std::vector<uint8_t> active_channels_;
  std::map<uint8_t, climate::ClimateMode> desired_mode_; // desired mode to reconcile after refresh
  std::set<uint8_t> strict_mode_channels_; // channels opting into strict baseline writes

  float temp_divisor_{10.0f};
  uint32_t last_poll_ms_{0};
  uint32_t receive_timeout_ms_{350};  // FIX: 350ms balances responsiveness with Wavin's variable response latency
  uint32_t suspend_polling_until_{0};
  GPIOPin *tx_enable_pin_{nullptr};
  GPIOPin *flow_control_pin_{nullptr};
  ModuleProfile module_profile_{ModuleProfile::MODULE_DEFAULT};
  uint8_t poll_channels_per_cycle_{2};
  uint8_t next_active_index_{0};
  uint8_t channel_step_[16] = {0};
  std::vector<uint8_t> urgent_channels_{}; // channels scheduled for immediate refresh on next update
  bool allow_mode_writes_{true};
  uint32_t pre_tx_delay_us_{0};
  uint32_t post_tx_guard_us_{300};
  uint32_t inter_frame_delay_us_{0};  // FIX: silence gap between consecutive transactions (set by setup())
  bool flush_rx_before_tx_{false};
  bool keep_standby_alive_{false};
  uint32_t standby_keepalive_interval_ms_{180000};
  std::map<uint8_t, uint32_t> standby_keepalive_deadlines_;

  // Protocol constants
  static constexpr uint8_t DEVICE_ADDR = 0x01;
  static constexpr uint8_t FC_READ = 0x43;
  static constexpr uint8_t FC_WRITE = 0x44;
  static constexpr uint8_t FC_WRITE_MASKED = 0x45;
  static constexpr uint8_t FC_EXCEPTION_FLAG = 0x80;  // error responses carry fc | 0x80 (0xC3/0xC4/0xC5)

  // Categories & indices (spec table 1.1)
  static constexpr uint8_t CAT_CHANNELS = 0x03;
  static constexpr uint8_t CAT_ELEMENTS = 0x01;
  static constexpr uint8_t CAT_PACKED = 0x02;

  static constexpr uint8_t CH_TIMER_EVENT = 0x00; // status incl. output bit
  static constexpr uint16_t CH_TIMER_EVENT_OUTP_ON_MASK = 0x0010;
  static constexpr uint8_t CH_PRIMARY_ELEMENT = 0x02;
  static constexpr uint16_t CH_PRIMARY_ELEMENT_ELEMENT_MASK = 0x003f;
  static constexpr uint16_t CH_PRIMARY_ELEMENT_ALL_TP_LOST_MASK = 0x0400;

  static constexpr uint8_t ELEM_AIR_TEMPERATURE = 0x04; // index within block
  static constexpr uint8_t ELEM_FLOOR_TEMPERATURE = 0x05; // index for floor probe
  static constexpr uint8_t ELEM_BATTERY_STATUS = 0x0A;
  static constexpr uint16_t TEMP_UNKNOWN_RAW = 0x7FFF;  // "temperature not known" marker (spec 1.3.6)

  static constexpr uint8_t PACKED_MANUAL_TEMPERATURE = 0x00;
  static constexpr uint8_t PACKED_STANDBY_TEMPERATURE = 0x04;
  static constexpr uint8_t PACKED_CONFIGURATION = 0x07;
  static constexpr uint8_t PACKED_FLOOR_MIN_TEMPERATURE = 0x0A;
  static constexpr uint8_t PACKED_FLOOR_MAX_TEMPERATURE = 0x0B;
  // Note: PACKED_FLOOR_MIN_TEMPERATURE and PACKED_FLOOR_MAX_TEMPERATURE are contiguous; reads
  // have been consolidated (count=2 starting at MIN) to reduce RS485 transactions.

  // CONFIGURATION register bit map (spec 1.6.9)
  static constexpr uint16_t PACKED_CONFIGURATION_MODE_MASK = 0x0007;        // MODE[2:0]
  static constexpr uint16_t PACKED_CONFIGURATION_MODE_MANUAL = 0x00;        // with SCHED ENA = 0
  static constexpr uint16_t PACKED_CONFIGURATION_MODE_STANDBY = 0x01;       // PERMANENT STANDBY with SCHED ENA = 0
  static constexpr uint16_t PACKED_CONFIGURATION_SCHED_ENA_MASK = 0x0008;   // bit 3
  static constexpr uint16_t PACKED_CONFIGURATION_HOTEL_MODE_MASK = 0x0200;  // bit 9
  static constexpr uint16_t PACKED_CONFIGURATION_CTRL_LOCK_MASK = 0x0400;   // bit 10: "CTRL LOCK" = menu lock on Wavin thermostats
  static constexpr uint16_t PACKED_CONFIGURATION_INT_LOCK_MASK = 0x0800;    // bit 11: "INT LOCK" = the thermostat's lock (child lock)
  static constexpr uint16_t PACKED_CONFIGURATION_ADAPT_MODE_MASK = 0x1000;  // bit 12
  static constexpr uint16_t PACKED_CONFIGURATION_COOL_MODE_MASK = 0x2000;   // bit 13
  static constexpr uint16_t PACKED_CONFIGURATION_FLOOR_ENA_MASK = 0x4000;   // bit 14
  static constexpr uint16_t PACKED_CONFIGURATION_FLOOR_SENS_MASK = 0x8000;  // bit 15 (read-only)
  // The child lock is INT LOCK (verified on hardware, see LockKind above)
  static constexpr uint16_t PACKED_CONFIGURATION_CHILD_LOCK_MASK = PACKED_CONFIGURATION_INT_LOCK_MASK;

  // I/O reliability: number of attempts for read/write before escalating to WARN
  static constexpr uint8_t IO_RETRY_ATTEMPTS = 2; // first failure logged at DEBUG, final at WARN
};

class WavinRepairButton : public button::Button {
 public:
  void set_parent(WavinAHC9000 *p) { this->parent_ = p; }
  void set_channel(uint8_t ch) { this->channel_ = ch; }
  void set_extended(bool v) { this->extended_ = v; }
  void set_aggressive(bool v) { this->aggressive_ = v; }
  void set_normalize(bool v) { this->normalize_ = v; }
  void set_normalize_off(bool v) { this->normalize_off_ = v; }

 protected:
  void press_action() override;

  WavinAHC9000 *parent_{nullptr};
  uint8_t channel_{0};
  bool extended_{false};
  bool aggressive_{false};
  bool normalize_{false};
  bool normalize_off_{false};
};

// Switch for one lock bit of a channel (child lock = INT LOCK, ctrl lock = CTRL LOCK; see LockKind).
// The state is optimistic; the urgent refresh scheduled by write_channel_lock() and the regular
// polling reconcile it with the register value.
class WavinLockSwitch : public switch_::Switch {
 public:
  void set_parent(WavinAHC9000 *p) { this->parent_ = p; }
  void set_channel(uint8_t ch) { this->channel_ = ch; }
  void set_lock_kind(LockKind kind) { this->kind_ = kind; }
  uint8_t get_channel() const { return this->channel_; }
  LockKind get_lock_kind() const { return this->kind_; }

 protected:
  void write_state(bool state) override {
    if (this->parent_ != nullptr) {
      this->parent_->write_channel_lock(this->channel_, this->kind_, state);
    }
    // Optimistic publish; hub publish_updates() will correct after refresh.
    this->publish_state(state);
  }
  WavinAHC9000 *parent_{nullptr};
  uint8_t channel_{0};
  LockKind kind_{LockKind::LOCK_CHILD};
};

class WavinYamlDumpButton : public button::Button {
 public:
  void set_parent(WavinAHC9000 *p) { this->parent_ = p; }

 protected:
  void press_action() override;

  WavinAHC9000 *parent_{nullptr};
};

// Inline helpers for configuring sensors
inline void WavinAHC9000::add_channel_battery_sensor(uint8_t ch, sensor::Sensor *s) {
  this->battery_sensors_[ch] = s;
}

inline void WavinAHC9000::add_channel_temperature_sensor(uint8_t ch, sensor::Sensor *s) {
  this->temperature_sensors_[ch] = s;
}

inline void WavinAHC9000::add_channel_comfort_setpoint_sensor(uint8_t ch, sensor::Sensor *s) {
  this->comfort_setpoint_sensors_[ch] = s;
}

inline void WavinAHC9000::add_channel_floor_temperature_sensor(uint8_t ch, sensor::Sensor *s) {
  this->floor_temperature_sensors_[ch] = s;
}

inline void WavinAHC9000::add_channel_floor_min_temperature_sensor(uint8_t ch, sensor::Sensor *s) {
  this->floor_min_temperature_sensors_[ch] = s;
}

inline void WavinAHC9000::add_channel_floor_max_temperature_sensor(uint8_t ch, sensor::Sensor *s) {
  this->floor_max_temperature_sensors_[ch] = s;
}

class WavinZoneClimate : public climate::Climate, public Component {
 public:
  void set_parent(WavinAHC9000 *p) { this->parent_ = p; }
  void set_single_channel(uint8_t ch) {
    this->single_channel_ = ch;
    this->single_channel_set_ = true;
    this->members_.clear();
  }
  void set_use_floor_temperature(bool v) { this->use_floor_temperature_ = v; }
  void set_members(const std::vector<int> &members) {
    this->members_.clear();
    for (int m : members) this->members_.push_back(static_cast<uint8_t>(m));
    this->single_channel_set_ = false;
  }

  void dump_config() override;

  void update_from_parent();

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  WavinAHC9000 *parent_{nullptr};
  uint8_t single_channel_{0};
  bool single_channel_set_{false};
  std::vector<uint8_t> members_{};
  bool use_floor_temperature_{false};
};

}  // namespace wavinahc9000v3
}  // namespace esphome
