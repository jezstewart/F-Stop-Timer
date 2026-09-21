// relay_core.h
//
// Portable timing core for the F-stop timer relay unit.
//
// This file has no Arduino or ESP32 dependencies. It is compiled on a normal
// computer for the unit tests (firmware/tests) and on the ESP32-S3 inside
// relay.ino. All hardware access goes through the Hal interface.
//
// The relay is the timing authority: the tablet arms it with a plan and the
// relay switches the enlarger lamp on and off by itself, so a tablet that
// sleeps, crashes or loses Bluetooth cannot change an exposure in progress.
//
// Wire format: see docs/PROTOCOL.md.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace fstop {

constexpr uint8_t  PROTOCOL_VERSION   = 1;
constexpr uint8_t  FW_MAJOR           = 1;
constexpr uint8_t  FW_MINOR           = 0;
constexpr uint8_t  MAX_STEPS          = 12;
constexpr uint32_t MAX_STEP_MS        = 30UL * 60UL * 1000UL;   // longest single step: 30 minutes
constexpr uint32_t MAX_LAMP_MS        = 60UL * 60UL * 1000UL;   // hard cap on continuous lamp time: 1 hour
constexpr uint32_t HEARTBEAT_TIMEOUT  = 1500;                   // ms of silence before the link counts as lost
constexpr uint32_t STATUS_PERIOD_MS   = 100;
constexpr size_t   MAX_FRAME          = 20;                     // fits the default BLE payload (ATT MTU 23)

enum State : uint8_t { S_IDLE = 0, S_ARMED = 1, S_FOCUS = 2, S_EXPOSING = 3, S_GAP = 4, S_PAUSED = 5, S_FAULT = 6 };
enum Kind : uint8_t { K_SINGLE = 0, K_STRIP = 1, K_RECIPE = 2 };

enum Cmd : uint8_t {
  C_PING = 0x01,
  C_ARM_BEGIN = 0x10, C_ARM_STEPS = 0x11, C_ARM_END = 0x12,
  C_START = 0x20, C_PAUSE = 0x21, C_RESUME = 0x22, C_CANCEL = 0x23,
  C_FOCUS = 0x30, C_SAFELIGHT = 0x31,
  C_CONFIG = 0x40
};
enum Msg : uint8_t { M_STATUS = 0x81, M_EVENT = 0x82 };
enum Ev : uint8_t {
  E_DONE = 1, E_CANCELLED = 2, E_PEDAL = 3, E_FOCUS_TIMEOUT = 4,
  E_FAULT = 5, E_STEP_DONE = 6, E_ARM_OK = 7, E_ARM_ERR = 8
};
enum Fault : uint8_t { F_NONE = 0, F_LINK_IN_FOCUS = 1, F_LAMP_TIMEOUT = 2 };
enum ArmErr : uint8_t { A_BUSY = 1, A_BAD_FRAME = 2, A_INCOMPLETE = 3, A_TOO_LONG = 4, A_TOO_MANY = 5, A_NO_SESSION = 6 };
enum Beep : uint8_t { B_TICK = 0, B_STEP = 1, B_DONE = 2 };

struct Config {
  int16_t  latencyMs       = 0;      // added to every lamp switch-on
  uint16_t focusTimeoutSec = 300;    // 0 = no timeout
  uint8_t  volume          = 60;     // 0..100
  bool     mute            = false;
  bool     safeAuto        = false;  // safelight off while the enlarger is exposing
};

// Everything the core needs from the outside world.
struct Hal {
  virtual void setEnlarger(bool on) = 0;
  virtual void setSafelight(bool on) = 0;
  virtual void beep(Beep kind, uint8_t volume) = 0;             // only called when not muted
  virtual bool notify(const uint8_t* data, size_t len) = 0;     // false when no client is connected
  virtual void configChanged(const Config&) {}
  virtual ~Hal() {}
};

class RelayCore {
 public:
  explicit RelayCore(Hal& hal) : hal_(hal) {}

  // Call once at boot. Outputs go to their safe state: enlarger off, safelight on.
  void begin(uint32_t now) {
    last_ = now; lastStatus_ = now; lamp_ = false; safeOut_ = false;
    hal_.setEnlarger(false);
    updateSafe();
  }
  void setConfig(const Config& c) { cfg_ = c; updateSafe(); }

  // ---- inputs -------------------------------------------------------------
  void onConnect(uint32_t now) { connected_ = true; heard_ = false; lastRx_ = now; stageActive_ = false; flushNext_ = true; }
  void onDisconnect(uint32_t now) {
    (void)now;
    connected_ = false; stageActive_ = false;
    if (hbOk_) { hbOk_ = false; linkLost(); }
  }
  void onFrame(const uint8_t* d, size_t n, uint32_t now) {
    if (n == 0) return;
    lastRx_ = now; heard_ = true;
    if (!hbOk_ || flushNext_) { hbOk_ = true; flushNext_ = false; flushPending(); }
    switch (d[0]) {
      case C_PING: break;
      case C_ARM_BEGIN: armBegin(d, n); break;
      case C_ARM_STEPS: armSteps(d, n); break;
      case C_ARM_END: armEnd(d, n); break;
      case C_START: start(now); break;
      case C_PAUSE: pause(now); break;
      case C_RESUME: resume(now); break;
      case C_CANCEL: cancel(now); break;
      case C_FOCUS: if (n >= 2) focus(d[1] != 0, now); break;
      case C_SAFELIGHT: if (n >= 2) { safeUser_ = d[1] != 0; } break;
      case C_CONFIG: config(d, n); break;
      default: break;
    }
    updateSafe();
  }
  // Foot pedal. Short press: start / pause / resume. Long press: cancel.
  void pedal(bool longPress, uint32_t now) {
    uint8_t e[3] = { M_EVENT, E_PEDAL, (uint8_t)(longPress ? 1 : 0) };
    emit(e, 3, false);
    if (longPress) { if (running()) cancel(now); return; }
    if (state_ == S_ARMED || state_ == S_FOCUS) start(now);
    else if (state_ == S_EXPOSING || state_ == S_GAP) pause(now);
    else if (state_ == S_PAUSED) resume(now);
    updateSafe();
  }
  // Call as often as possible (every millisecond or so).
  void tick(uint32_t now) {
    uint32_t dt = now - last_; last_ = now;
    if (dt) step(dt, now);
    if (heard_ && connected_ && hbOk_ && (now - lastRx_) > HEARTBEAT_TIMEOUT) { hbOk_ = false; linkLost(); }
    if (lamp_ && (now - lampSince_) > MAX_LAMP_MS) {                 // last-resort guard
      setLamp(false, now); state_ = S_FAULT; fault_ = F_LAMP_TIMEOUT;
      uint8_t e[3] = { M_EVENT, E_FAULT, F_LAMP_TIMEOUT }; emit(e, 3, true);
    }
    updateSafe();
    if (now - lastStatus_ >= STATUS_PERIOD_MS) { lastStatus_ = now; sendStatus(); }
  }

  // ---- read-only view (used by tests) --------------------------------------
  State state() const { return state_; }
  bool lamp() const { return lamp_; }
  bool safelight() const { return safeOut_; }
  const Config& config() const { return cfg_; }
  Fault fault() const { return fault_; }
  bool planValid() const { return planValid_; }

 private:
  struct Plan { uint8_t id = 0, kind = 0, n = 0; uint32_t gap = 0; uint32_t ms[MAX_STEPS]; uint16_t mask = 0; };

  bool running() const { return state_ == S_EXPOSING || state_ == S_GAP || state_ == S_PAUSED; }

  static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
  static void wr32(uint8_t* p, uint32_t v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; }

  // Send an event. Events that matter after the fact (done, faults) are kept
  // and re-sent when the tablet next talks to us if nobody could receive them.
  void emit(const uint8_t* d, size_t n, bool keep) {
    if (!hal_.notify(d, n) && keep) {
      if (pendN_ == PEND) { memmove(pend_[0], pend_[1], sizeof(pend_[0]) * (PEND - 1)); memmove(pendLen_, pendLen_ + 1, PEND - 1); pendN_--; }
      memcpy(pend_[pendN_], d, n); pendLen_[pendN_] = (uint8_t)n; pendN_++;
    }
  }
  void flushPending() {
    uint8_t n = pendN_; pendN_ = 0;
    for (uint8_t i = 0; i < n; i++) hal_.notify(pend_[i], pendLen_[i]);
  }
  void armErr(uint8_t id, uint8_t code) { uint8_t e[4] = { M_EVENT, E_ARM_ERR, id, code }; emit(e, 4, false); }

  void setLamp(bool on, uint32_t now) {
    if (on == lamp_) return;
    lamp_ = on; if (on) lampSince_ = now;
    hal_.setEnlarger(on);
  }
  void updateSafe() {
    bool s = safeUser_ && !(cfg_.safeAuto && state_ == S_EXPOSING);
    if (s != safeOut_) { safeOut_ = s; hal_.setSafelight(s); }
  }
  void beepIf(Beep b) { if (!cfg_.mute && cfg_.volume > 0) hal_.beep(b, cfg_.volume); }
  void linkLost() {
    if (state_ == S_FOCUS) {
      setLamp(false, last_); state_ = S_FAULT; fault_ = F_LINK_IN_FOCUS;
      uint8_t e[3] = { M_EVENT, E_FAULT, F_LINK_IN_FOCUS }; emit(e, 3, true);
    }
  }

  // ---- arming (the plan arrives as BEGIN, STEPS..., END) --------------------
  void armBegin(const uint8_t* d, size_t n) {
    if (n < 8) { armErr(n > 1 ? d[1] : 0, A_BAD_FRAME); return; }
    uint8_t id = d[1], kind = d[2], cnt = d[3]; uint32_t gap = rd32(d + 4);
    stageActive_ = false;
    if (running()) { armErr(id, A_BUSY); return; }
    if (kind > K_RECIPE) { armErr(id, A_BAD_FRAME); return; }
    if (cnt == 0 || cnt > MAX_STEPS) { armErr(id, A_TOO_MANY); return; }
    if (gap > MAX_STEP_MS) { armErr(id, A_TOO_LONG); return; }
    stage_ = Plan(); stage_.id = id; stage_.kind = kind; stage_.n = cnt; stage_.gap = gap;
    stageActive_ = true;
  }
  void armSteps(const uint8_t* d, size_t n) {
    if (n < 4) { armErr(0, A_BAD_FRAME); return; }
    uint8_t id = d[1], first = d[2], cnt = d[3];
    if (!stageActive_ || id != stage_.id) { armErr(id, A_NO_SESSION); return; }
    if (cnt < 1 || cnt > 4 || n != (size_t)(4 + 4 * cnt) || (unsigned)first + cnt > stage_.n) { armErr(id, A_BAD_FRAME); stageActive_ = false; return; }
    for (uint8_t i = 0; i < cnt; i++) {
      uint32_t ms = rd32(d + 4 + 4 * i);
      if (ms > MAX_STEP_MS) { armErr(id, A_TOO_LONG); stageActive_ = false; return; }
      stage_.ms[first + i] = ms; stage_.mask |= (uint16_t)(1u << (first + i));
    }
  }
  void armEnd(const uint8_t* d, size_t n) {
    uint8_t id = n > 1 ? d[1] : 0;
    if (!stageActive_ || id != stage_.id) { armErr(id, A_NO_SESSION); return; }
    stageActive_ = false;
    if (running()) { armErr(id, A_BUSY); return; }
    uint16_t full = (uint16_t)((1u << stage_.n) - 1u);
    if (stage_.mask != full) { armErr(id, A_INCOMPLETE); return; }
    plan_ = stage_; planValid_ = true;
    if (state_ == S_IDLE || state_ == S_FAULT) { state_ = S_ARMED; fault_ = F_NONE; }
    uint8_t e[3] = { M_EVENT, E_ARM_OK, id }; emit(e, 3, false);
  }

  // ---- exposure control ------------------------------------------------------
  void start(uint32_t now) {
    if (!planValid_ || (state_ != S_ARMED && state_ != S_FOCUS)) return;
    sumMs_ = 0; lampSince_ = now;            // a lamp already on for focusing simply stays on
    beginStep(0, now);
  }
  void beginStep(uint8_t i, uint32_t now) {
    idx_ = i;
    int32_t t = (int32_t)plan_.ms[i] + (int32_t)cfg_.latencyMs; if (t < 1) t = 1;
    total_ = (uint32_t)t; rem_ = total_; sumMs_ += total_;
    state_ = S_EXPOSING; lastTickSec_ = 0;
    setLamp(true, now);
  }
  void step(uint32_t dt, uint32_t now) {
    switch (state_) {
      case S_EXPOSING: {
        if (dt >= rem_) { rem_ = 0; endStep(now); break; }
        rem_ -= dt;
        uint32_t sec = (total_ - rem_) / 1000;
        if (sec > lastTickSec_) { lastTickSec_ = sec; beepIf(B_TICK); }
        break;
      }
      case S_GAP:
        if (dt >= rem_) beginStep((uint8_t)(idx_ + 1), now); else rem_ -= dt;
        break;
      case S_FOCUS:
        focusEl_ += dt;
        if (cfg_.focusTimeoutSec && focusEl_ >= (uint32_t)cfg_.focusTimeoutSec * 1000UL) {
          setLamp(false, now); state_ = planValid_ ? S_ARMED : S_IDLE;
          uint8_t e[2] = { M_EVENT, E_FOCUS_TIMEOUT }; emit(e, 2, true);
        }
        break;
      default: break;
    }
  }
  void endStep(uint32_t now) {
    setLamp(false, now); rem_ = 0;
    if (idx_ + 1 < plan_.n) {
      beepIf(B_STEP);
      uint8_t e[3] = { M_EVENT, E_STEP_DONE, idx_ }; emit(e, 3, false);
      if (plan_.gap > 0) { state_ = S_GAP; total_ = plan_.gap; rem_ = plan_.gap; }
      else beginStep((uint8_t)(idx_ + 1), now);
    } else {
      beepIf(B_DONE); state_ = S_ARMED;
      uint8_t e[6] = { M_EVENT, E_DONE, 0, 0, 0, 0 }; wr32(e + 2, sumMs_); emit(e, 6, true);
    }
  }
  void pause(uint32_t now) {
    if (state_ == S_EXPOSING || state_ == S_GAP) { pf_ = state_; state_ = S_PAUSED; setLamp(false, now); }
  }
  void resume(uint32_t now) {
    if (state_ == S_PAUSED) { state_ = pf_; setLamp(state_ == S_EXPOSING, now); }
  }
  void cancel(uint32_t now) {
    if (running()) {
      setLamp(false, now); state_ = S_ARMED;
      uint8_t e[2] = { M_EVENT, E_CANCELLED }; emit(e, 2, false);
    } else if (state_ == S_FAULT) { fault_ = F_NONE; state_ = planValid_ ? S_ARMED : S_IDLE; }
  }
  void focus(bool on, uint32_t now) {
    if (on) {
      if (state_ == S_IDLE || state_ == S_ARMED || state_ == S_FAULT) { state_ = S_FOCUS; focusEl_ = 0; fault_ = F_NONE; setLamp(true, now); }
    } else if (state_ == S_FOCUS) { setLamp(false, now); state_ = planValid_ ? S_ARMED : S_IDLE; }
  }
  void config(const uint8_t* d, size_t n) {
    if (n < 7) return;
    Config c;
    c.latencyMs = (int16_t)((uint16_t)d[1] | ((uint16_t)d[2] << 8));
    c.focusTimeoutSec = (uint16_t)((uint16_t)d[3] | ((uint16_t)d[4] << 8));
    c.volume = d[5] > 100 ? 100 : d[5];
    c.mute = (d[6] & 1) != 0; c.safeAuto = (d[6] & 2) != 0;
    if (c.latencyMs > 500) c.latencyMs = 500;
    if (c.latencyMs < -500) c.latencyMs = -500;
    bool volChanged = c.volume != cfg_.volume;
    cfg_ = c; hal_.configChanged(cfg_);
    if (volChanged) beepIf(B_TICK);
  }

  void sendStatus() {
    uint8_t f[18];
    f[0] = M_STATUS; f[1] = state_;
    f[2] = (uint8_t)((lamp_ ? 1 : 0) | (safeOut_ ? 2 : 0) | (safeUser_ ? 4 : 0));
    f[3] = idx_; f[4] = planValid_ ? plan_.n : 0;
    wr32(f + 5, rem_); wr32(f + 9, total_);
    uint16_t fr = 0xFFFF;
    if (state_ == S_FOCUS && cfg_.focusTimeoutSec) {
      uint32_t left = (uint32_t)cfg_.focusTimeoutSec * 1000UL; left = left > focusEl_ ? left - focusEl_ : 0;
      fr = (uint16_t)(left / 1000UL);
    }
    f[13] = fr & 255; f[14] = fr >> 8;
    f[15] = planValid_ ? plan_.kind : 0xFF;
    f[16] = state_ == S_PAUSED ? (uint8_t)pf_ : 0xFF;
    f[17] = fault_;
    hal_.notify(f, sizeof f);
  }

  Hal& hal_;
  Config cfg_;
  State state_ = S_IDLE, pf_ = S_EXPOSING;
  Fault fault_ = F_NONE;
  Plan plan_, stage_;
  bool planValid_ = false, stageActive_ = false;
  bool lamp_ = false, safeUser_ = true, safeOut_ = false;
  bool connected_ = false, heard_ = false, hbOk_ = true, flushNext_ = false;
  uint8_t idx_ = 0;
  uint32_t rem_ = 0, total_ = 0, sumMs_ = 0, focusEl_ = 0, lastTickSec_ = 0;
  uint32_t last_ = 0, lastRx_ = 0, lastStatus_ = 0, lampSince_ = 0;
  static constexpr uint8_t PEND = 4;
  uint8_t pend_[PEND][8]; uint8_t pendLen_[PEND]; uint8_t pendN_ = 0;
};

}  // namespace fstop
