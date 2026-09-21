// Unit tests for relay_core.h. Build and run with firmware/tests/run_tests.sh
#include "../relay/relay_core.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

using namespace fstop;
using Bytes = std::vector<uint8_t>;

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { failures++; printf("  FAIL  %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)
#define CHECK_NEAR(a, b, tol, msg) do { checks++; long _a = (long)(a), _b = (long)(b); if (labs(_a - _b) > (long)(tol)) { failures++; printf("  FAIL  %s: got %ld, expected %ld +/- %ld  (%s:%d)\n", msg, _a, _b, (long)(tol), __FILE__, __LINE__); } } while (0)

static uint32_t T = 1000;   // fake clock, ms

struct FakeHal : Hal {
  bool connected = true;
  bool enl = false, safe = false;
  struct Edge { uint32_t t; bool on; };
  std::vector<Edge> lamp, safeEdges;
  std::vector<Bytes> notes;
  std::vector<int> beeps;
  int configCalls = 0;
  void setEnlarger(bool on) override { enl = on; lamp.push_back({ T, on }); }
  void setSafelight(bool on) override { safe = on; safeEdges.push_back({ T, on }); }
  void beep(Beep k, uint8_t) override { beeps.push_back(k); }
  bool notify(const uint8_t* d, size_t n) override { if (!connected) return false; notes.push_back(Bytes(d, d + n)); return true; }
  void configChanged(const Config&) override { configCalls++; }

  int countEvents(uint8_t ev) const { int c = 0; for (auto& b : notes) if (b.size() >= 2 && b[0] == M_EVENT && b[1] == ev) c++; return c; }
  Bytes firstEvent(uint8_t ev) const { for (auto& b : notes) if (b.size() >= 2 && b[0] == M_EVENT && b[1] == ev) return b; return Bytes(); }
  Bytes lastEvent(uint8_t ev) const { Bytes r; for (auto& b : notes) if (b.size() >= 2 && b[0] == M_EVENT && b[1] == ev) r = b; return r; }
  int countBeeps(int k) const { int c = 0; for (int b : beeps) if (b == k) c++; return c; }
  // durations of every lamp-on interval, in ms
  std::vector<uint32_t> onWindows() const {
    std::vector<uint32_t> w; uint32_t s = 0; bool on = false;
    for (auto& e : lamp) { if (e.on && !on) { s = e.t; on = true; } else if (!e.on && on) { w.push_back(e.t - s); on = false; } }
    return w;
  }
  std::vector<uint32_t> offGaps() const {
    std::vector<uint32_t> g; uint32_t s = 0; bool off = false; bool seenOn = false;
    for (auto& e : lamp) { if (e.on) { if (off && seenOn) g.push_back(e.t - s); off = false; seenOn = true; } else { if (seenOn) { s = e.t; off = true; } } }
    return g;
  }
  void clear() { lamp.clear(); safeEdges.clear(); notes.clear(); beeps.clear(); }
};

static void run(RelayCore& c, uint32_t ms) { for (uint32_t i = 0; i < ms; i++) { T++; c.tick(T); } }
static void send(RelayCore& c, const Bytes& b) { c.onFrame(b.data(), b.size(), T); }
static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static Bytes u32(uint32_t v) { return { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; }
static void arm(RelayCore& c, uint8_t id, uint8_t kind, uint32_t gap, const std::vector<uint32_t>& ms) {
  Bytes b = { C_ARM_BEGIN, id, kind, (uint8_t)ms.size() }; Bytes g = u32(gap); b.insert(b.end(), g.begin(), g.end()); send(c, b);
  for (size_t i = 0; i < ms.size(); i += 4) {
    size_t cnt = ms.size() - i < 4 ? ms.size() - i : 4;
    Bytes s = { C_ARM_STEPS, id, (uint8_t)i, (uint8_t)cnt };
    for (size_t k = 0; k < cnt; k++) { Bytes v = u32(ms[i + k]); s.insert(s.end(), v.begin(), v.end()); }
    send(c, s);
  }
  send(c, { C_ARM_END, id });
}
static void config(RelayCore& c, int16_t lat, uint16_t focusSec, uint8_t vol, bool mute, bool safeAuto) {
  send(c, { C_CONFIG, (uint8_t)(lat & 255), (uint8_t)((lat >> 8) & 255), (uint8_t)(focusSec & 255), (uint8_t)(focusSec >> 8), vol, (uint8_t)((mute ? 1 : 0) | (safeAuto ? 2 : 0)) });
}

struct Rig {
  FakeHal hal; RelayCore core; Rig() : core(hal) { T += 5000; core.begin(T); core.onConnect(T); send(core, { C_PING }); hal.clear(); }
};

static void test_single() {
  printf("single exposure\n");
  Rig r; arm(r.core, 1, K_SINGLE, 0, { 2000 });
  CHECK(r.core.state() == S_ARMED, "armed after ARM_END");
  CHECK(r.hal.countEvents(E_ARM_OK) == 1, "ARM_OK event");
  send(r.core, { C_START }); CHECK(r.hal.enl, "lamp on immediately after START");
  run(r.core, 2100);
  auto w = r.hal.onWindows();
  CHECK(w.size() == 1, "one lamp window");
  if (w.size()) CHECK_NEAR(w[0], 2000, 1, "lamp on for 2000 ms");
  CHECK(r.core.state() == S_ARMED, "back to armed");
  CHECK(r.hal.countEvents(E_DONE) == 1, "done event");
  Bytes d = r.hal.lastEvent(E_DONE); CHECK(d.size() == 6 && rd32(&d[2]) == 2000, "done event carries total lamp time");
  CHECK(r.hal.countBeeps(B_TICK) == 1, "one tick beep (at 1 s)");
  CHECK(r.hal.countBeeps(B_DONE) == 1, "done beep");
}

static void test_strip() {
  printf("strip test with gaps\n");
  Rig r; std::vector<uint32_t> ms = { 1000, 260, 327, 413, 520, 655 };
  arm(r.core, 2, K_STRIP, 500, ms);
  send(r.core, { C_START });
  run(r.core, 1000 + 260 + 327 + 413 + 520 + 655 + 5 * 500 + 200);
  auto w = r.hal.onWindows(); auto g = r.hal.offGaps();
  CHECK(w.size() == 6, "six lamp windows");
  for (size_t i = 0; i < w.size() && i < ms.size(); i++) CHECK_NEAR(w[i], ms[i], 1, "step duration");
  CHECK(g.size() == 5, "five gaps");
  for (size_t i = 0; i < g.size(); i++) CHECK_NEAR(g[i], 500, 2, "gap length, enlarger off");
  CHECK(r.hal.countEvents(E_STEP_DONE) == 5, "five step_done events");
  CHECK(r.hal.countEvents(E_DONE) == 1, "one done event");
  CHECK(r.hal.countBeeps(B_STEP) == 5, "step beeps");
}

static void test_no_gap_and_offset() {
  printf("latency offset and zero gap\n");
  { Rig r; config(r.core, 50, 300, 60, false, false); arm(r.core, 1, K_SINGLE, 0, { 1000 }); send(r.core, { C_START }); run(r.core, 1200);
    auto w = r.hal.onWindows(); CHECK(w.size() == 1, "one window"); if (w.size()) CHECK_NEAR(w[0], 1050, 1, "+50 ms offset"); }
  { Rig r; config(r.core, -20, 300, 60, false, false); arm(r.core, 1, K_SINGLE, 0, { 1000 }); send(r.core, { C_START }); run(r.core, 1200);
    auto w = r.hal.onWindows(); if (w.size()) CHECK_NEAR(w[0], 980, 1, "-20 ms offset"); }
  { Rig r; arm(r.core, 1, K_STRIP, 0, { 300, 200, 100 }); send(r.core, { C_START }); run(r.core, 700);
    auto w = r.hal.onWindows(); CHECK(w.size() == 3, "three back-to-back steps with no gap"); }
}

static void test_pause_cancel() {
  printf("pause, resume, cancel\n");
  { Rig r; arm(r.core, 1, K_SINGLE, 0, { 2000 }); send(r.core, { C_START }); run(r.core, 500);
    send(r.core, { C_PAUSE }); CHECK(r.core.state() == S_PAUSED && !r.hal.enl, "paused, lamp off");
    run(r.core, 1000); CHECK(r.core.state() == S_PAUSED, "still paused");
    send(r.core, { C_RESUME }); run(r.core, 1600);
    uint32_t sum = 0; for (auto x : r.hal.onWindows()) sum += x;
    CHECK_NEAR(sum, 2000, 3, "total lamp time preserved across a pause"); CHECK(r.core.state() == S_ARMED, "finished"); }
  { Rig r; arm(r.core, 1, K_STRIP, 1000, { 500, 500 }); send(r.core, { C_START }); run(r.core, 700);
    CHECK(r.core.state() == S_GAP, "in gap"); send(r.core, { C_PAUSE }); run(r.core, 3000);
    CHECK(r.core.state() == S_PAUSED, "pause during a gap holds it"); send(r.core, { C_RESUME }); CHECK(r.core.state() == S_GAP, "resumes into the gap"); }
  { Rig r; arm(r.core, 1, K_SINGLE, 0, { 2000 }); send(r.core, { C_START }); run(r.core, 300);
    send(r.core, { C_CANCEL }); CHECK(!r.hal.enl && r.core.state() == S_ARMED, "cancel: lamp off, armed");
    CHECK(r.hal.countEvents(E_CANCELLED) == 1, "cancelled event"); CHECK(r.hal.countEvents(E_DONE) == 0, "no done event"); }
}

static void test_pedal_and_offline() {
  printf("foot pedal with no Bluetooth link\n");
  Rig r; arm(r.core, 1, K_SINGLE, 0, { 1500 });
  r.hal.connected = false; r.core.onDisconnect(T);
  r.core.pedal(false, T); CHECK(r.core.state() == S_EXPOSING && r.hal.enl, "pedal starts the armed exposure");
  run(r.core, 300); r.core.pedal(false, T); CHECK(r.core.state() == S_PAUSED && !r.hal.enl, "pedal pauses");
  r.core.pedal(false, T); CHECK(r.core.state() == S_EXPOSING, "pedal resumes");
  run(r.core, 100); r.core.pedal(true, T); CHECK(r.core.state() == S_ARMED && !r.hal.enl, "long press cancels");
  r.core.pedal(false, T); run(r.core, 1700);
  uint32_t sum = 0; auto w = r.hal.onWindows(); for (size_t i = w.size() >= 1 ? w.size() - 1 : 0; i < w.size(); i++) sum += w[i];
  CHECK_NEAR(sum, 1500, 2, "final run lasts the armed time"); CHECK(r.core.state() == S_ARMED, "finished");
  CHECK(r.hal.countEvents(E_DONE) == 0, "done event could not be delivered yet");
  r.hal.connected = true; r.core.onConnect(T); r.hal.clear(); send(r.core, { C_PING });
  CHECK(r.hal.countEvents(E_DONE) == 1, "done event delivered after reconnecting");
}

static void test_link_loss_during_exposure() {
  printf("link loss during an exposure\n");
  Rig r; arm(r.core, 1, K_SINGLE, 0, { 1500 }); send(r.core, { C_START }); run(r.core, 300);
  r.hal.connected = false; r.core.onDisconnect(T);
  run(r.core, 1500);
  auto w = r.hal.onWindows(); CHECK(w.size() == 1, "one window"); if (w.size()) CHECK_NEAR(w[0], 1500, 1, "exposure completes at the commanded length");
  CHECK(r.core.state() == S_ARMED, "armed again"); CHECK(r.hal.countEvents(E_DONE) == 0, "not delivered while disconnected");
  r.hal.connected = true; r.core.onConnect(T); send(r.core, { C_PING });
  CHECK(r.hal.countEvents(E_DONE) == 1, "queued done event delivered on reconnect");
}

static void test_focus() {
  printf("focus light\n");
  { Rig r; config(r.core, 0, 2, 60, false, false); send(r.core, { C_FOCUS, 1 }); CHECK(r.hal.enl && r.core.state() == S_FOCUS, "focus on");
    for (int i = 0; i < 40; i++) { run(r.core, 50); send(r.core, { C_PING }); }
    CHECK(!r.hal.enl && r.core.state() == S_IDLE, "focus times out and switches the lamp off");
    CHECK(r.hal.countEvents(E_FOCUS_TIMEOUT) == 1, "timeout event"); }
  { Rig r; send(r.core, { C_FOCUS, 1 }); run(r.core, 1700);
    CHECK(!r.hal.enl && r.core.state() == S_FAULT && r.core.fault() == F_LINK_IN_FOCUS, "silent tablet in focus mode: lamp off, fault");
    CHECK(r.hal.countEvents(E_FAULT) == 1, "fault event");
    send(r.core, { C_PING }); arm(r.core, 1, K_SINGLE, 0, { 500 });
    CHECK(r.core.state() == S_ARMED, "ARM clears the fault"); }
  { Rig r; send(r.core, { C_FOCUS, 1 }); run(r.core, 200); r.hal.connected = false; r.core.onDisconnect(T);
    CHECK(!r.hal.enl && r.core.state() == S_FAULT, "disconnect in focus mode switches the lamp off at once");
    r.hal.connected = true; r.core.onConnect(T); r.hal.clear(); send(r.core, { C_PING });
    CHECK(r.hal.countEvents(E_FAULT) == 1, "fault event delivered after reconnect"); }
  { Rig r; arm(r.core, 1, K_SINGLE, 0, { 1000 }); send(r.core, { C_FOCUS, 1 }); run(r.core, 200); send(r.core, { C_START }); run(r.core, 1200);
    CHECK(r.core.state() == S_ARMED, "START from focus runs the exposure");
    auto w = r.hal.onWindows(); CHECK(w.size() == 1, "lamp stays on from focus into the exposure"); }
}

static void test_arm_validation() {
  printf("arming validation\n");
  { Rig r; send(r.core, { C_ARM_BEGIN, 1, K_STRIP, 3, 0, 0, 0, 0 }); send(r.core, { C_ARM_STEPS, 1, 0, 1, 1, 0, 0, 0 }); send(r.core, { C_ARM_END, 1 });
    CHECK(r.core.state() != S_ARMED && !r.core.planValid(), "incomplete plan is rejected");
    Bytes e = r.hal.lastEvent(E_ARM_ERR); CHECK(e.size() == 4 && e[3] == A_INCOMPLETE, "incomplete error code"); }
  { Rig r; arm(r.core, 1, K_SINGLE, 0, { 2000 }); send(r.core, { C_START }); run(r.core, 100); r.hal.notes.clear();
    arm(r.core, 2, K_SINGLE, 0, { 500 });
    Bytes e = r.hal.firstEvent(E_ARM_ERR); CHECK(e.size() == 4 && e[3] == A_BUSY, "cannot re-arm while running"); run(r.core, 2100);
    auto w = r.hal.onWindows(); CHECK(w.size() == 1 && w[0] > 1990, "running exposure unaffected"); }
  { Rig r; send(r.core, { C_ARM_BEGIN, 1, K_SINGLE, 1, 0, 0, 0, 0 }); send(r.core, { C_ARM_STEPS, 1, 0, 1, 0x00, 0x00, 0x20, 0x00 });   // 2,097,152 ms
    Bytes e = r.hal.lastEvent(E_ARM_ERR); CHECK(e.size() == 4 && e[3] == A_TOO_LONG, "step over 30 minutes is rejected"); }
  { Rig r; send(r.core, { C_ARM_BEGIN, 1, K_SINGLE, 13, 0, 0, 0, 0 });
    Bytes e = r.hal.lastEvent(E_ARM_ERR); CHECK(e.size() == 4 && e[3] == A_TOO_MANY, "13 steps is too many"); }
  { Rig r; send(r.core, { C_ARM_BEGIN, 1, K_SINGLE, 1, 0, 0, 0, 0 }); send(r.core, { C_ARM_STEPS, 9, 0, 1, 1, 0, 0, 0 });
    Bytes e = r.hal.lastEvent(E_ARM_ERR); CHECK(e.size() == 4 && e[3] == A_NO_SESSION, "wrong plan id is rejected"); }
  { Rig r; send(r.core, { C_ARM_BEGIN, 1, K_SINGLE, 1, 0, 0, 0, 0 }); send(r.core, { C_ARM_STEPS, 1, 0, 2, 1, 0, 0, 0 });
    Bytes e = r.hal.lastEvent(E_ARM_ERR); CHECK(e.size() == 4 && e[3] == A_BAD_FRAME, "wrong frame length is rejected"); }
  { Rig r; send(r.core, { C_START }); CHECK(r.core.state() == S_IDLE && !r.hal.enl, "START without a plan does nothing"); }
  { Rig r; arm(r.core, 1, K_RECIPE, 5000, { 12000, 4100, 2900, 5000, 700, 800, 900, 1000, 1100 });
    CHECK(r.core.state() == S_ARMED, "nine-step recipe plan is accepted (chunked in fours)"); }
}

static void test_safelight_and_config() {
  printf("safelight, volume and mute\n");
  { Rig r; config(r.core, 0, 300, 60, false, true); arm(r.core, 1, K_SINGLE, 0, { 1000 }); CHECK(r.hal.safe, "safelight on by default");
    send(r.core, { C_START }); CHECK(!r.hal.safe, "safelight off while exposing (setting on)"); run(r.core, 1100); CHECK(r.hal.safe, "safelight back on after"); }
  { Rig r; send(r.core, { C_SAFELIGHT, 0 }); CHECK(!r.hal.safe, "safelight can be switched off"); send(r.core, { C_SAFELIGHT, 1 }); CHECK(r.hal.safe, "and on"); }
  { Rig r; config(r.core, 0, 300, 60, true, false); arm(r.core, 1, K_SINGLE, 0, { 2500 }); send(r.core, { C_START }); run(r.core, 2600);
    CHECK(r.hal.beeps.empty(), "mute silences the buzzer"); }
  { Rig r; config(r.core, 0, 300, 80, false, false); CHECK(r.hal.countBeeps(B_TICK) == 1, "changing the volume plays a test beep");
    CHECK(r.core.config().volume == 80 && r.hal.configCalls >= 1, "config applied and reported for saving"); }
}

static void test_lamp_cap() {
  printf("one-hour lamp cap\n");
  Rig r; config(r.core, 0, 0, 60, false, false); send(r.core, { C_FOCUS, 1 });
  for (uint32_t i = 0; i < 3600 * 1000UL + 2000; i++) { T++; r.core.tick(T); if (i % 400 == 0) send(r.core, { C_PING }); }
  CHECK(!r.hal.enl && r.core.state() == S_FAULT && r.core.fault() == F_LAMP_TIMEOUT, "lamp forced off after an hour");
}

static void test_status_frame() {
  printf("status frame\n");
  Rig r; arm(r.core, 7, K_STRIP, 500, { 1000, 300, 300, 300, 300, 300 }); r.hal.clear(); run(r.core, 250);
  Bytes s; for (auto& b : r.hal.notes) if (b[0] == M_STATUS) s = b;
  CHECK(s.size() == 18, "status frame is 18 bytes"); if (s.size() == 18) {
    CHECK(s[1] == S_ARMED, "state armed"); CHECK((s[2] & 4) != 0, "safelight-user flag"); CHECK(s[4] == 6, "six steps"); CHECK(s[15] == K_STRIP, "kind strip");
    CHECK(s[13] == 0xFF && s[14] == 0xFF, "no focus timer"); CHECK(s[16] == 0xFF, "not paused"); CHECK(s[17] == 0, "no fault"); }
  send(r.core, { C_START }); run(r.core, 350); s.clear(); for (auto& b : r.hal.notes) if (b[0] == M_STATUS) s = b;
  if (s.size() == 18) { CHECK(s[1] == S_EXPOSING && (s[2] & 1), "exposing with lamp flag"); CHECK_NEAR(rd32(&s[9]), 1000, 0, "total"); CHECK(rd32(&s[5]) < 1000 && rd32(&s[5]) > 500, "remaining counts down"); }
  int statuses = 0; for (auto& b : r.hal.notes) if (b[0] == M_STATUS) statuses++;
  CHECK(statuses >= 5 && statuses <= 8, "status about every 100 ms");
}

int main() {
  test_single(); test_strip(); test_no_gap_and_offset(); test_pause_cancel(); test_pedal_and_offline();
  test_link_loss_during_exposure(); test_focus(); test_arm_validation(); test_safelight_and_config(); test_lamp_cap(); test_status_frame();
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
