#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_LIS3DH.h>
#include <U8g2lib.h>
#include <ArduinoLowPower.h>
#include <math.h>

#include "config.h"
#include "log.h"

namespace {

  // ----- LIS3DH registers we touch directly (the Adafruit driver doesn't
  // expose them all, and motion-interrupt config needs raw writes). -----
  constexpr uint8_t REG_CTRL1     = 0x20;
  constexpr uint8_t REG_CTRL2     = 0x21;
  constexpr uint8_t REG_CTRL3     = 0x22;
  constexpr uint8_t REG_CTRL5     = 0x24;
  constexpr uint8_t REG_REFERENCE = 0x26;
  constexpr uint8_t REG_INT1_CFG  = 0x30;
  constexpr uint8_t REG_INT1_SRC  = 0x31;
  constexpr uint8_t REG_INT1_THS  = 0x32;
  constexpr uint8_t REG_INT1_DUR  = 0x33;

  Adafruit_LIS3DH lis;
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

  enum class State : uint8_t {
    Sleep,        // MCU in standby, OLED off; LIS3DH watching for any motion
    Detecting,    // Motion present, but not yet confirmed as a brew
    Running,      // Brew in progress, timer counting
    Holding,      // Brew finished, displaying final time
  };

  State state = State::Sleep;
  volatile bool wake_flag = false;

  // i=idle, p=pump-like, x=spike.  Takes the squared mean magnitude so the
  // hot path never calls sqrtf.
  [[maybe_unused]] inline char motionChar(const float msq) {
    if (msq <= cfg::kThActiveSq) return 'i';
    if (msq <= cfg::kThSpikeSq)  return 'p';
    return 'x';
  }

  uint32_t shot_start_ms    = 0;
  uint32_t last_active_ms   = 0;  // last tick classified as motion (any kind)
  uint32_t last_pumplike_ms = 0;  // last tick classified as pump (not spike)
  uint32_t hold_start_ms    = 0;
  uint32_t final_elapsed_ms = 0;

  // ----- LIS3DH I2C helpers -----
  void lisWrite(const uint8_t reg, const uint8_t val) {
    Wire.beginTransmission(cfg::kLis3dhAddr);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
  }

  uint8_t lisRead(const uint8_t reg) {
    Wire.beginTransmission(cfg::kLis3dhAddr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(cfg::kLis3dhAddr, (uint8_t)1);
    return Wire.read();
  }

  void onWakeIsr() {
    wake_flag = true;
  }

  void configureLis3dh() {
    lis.setRange(LIS3DH_RANGE_2_G);

    // CTRL_REG2: HPF routed to OUT (FDS=1) and to AOI1 (HPIS1=1).
    //   HPM=00 (normal/auto-reset), HPCF=01 (~ODR/50 cutoff),
    //   FDS=1, HPCLICK=0, HPIS2=0, HPIS1=1
    lisWrite(REG_CTRL2, 0b00011001);

    // CTRL_REG3: I1_IA1 -> route AOI1 to physical INT1 pin
    lisWrite(REG_CTRL3, 0x40);

    // CTRL_REG5: non-latched interrupt, no FIFO
    lisWrite(REG_CTRL5, 0x00);

    // INT1_CFG: AOI=0 (OR), enable X-high | Y-high | Z-high
    lisWrite(REG_INT1_CFG, 0b00101010);
    lisWrite(REG_INT1_THS, cfg::kWakeThresholdLsb);
    lisWrite(REG_INT1_DUR, 0x00);

    (void)lisRead(REG_REFERENCE);  // reset HPF reference
    (void)lisRead(REG_INT1_SRC);   // clear pending interrupt
  }

  void setLisActive() {
    lisWrite(REG_CTRL1, cfg::kActiveOdrReg1);
    (void)lisRead(REG_REFERENCE);
    delay(20);  // let HPF settle so the first sample isn't a transient
  }

  void setLisSleep() {
    lisWrite(REG_CTRL1, cfg::kSleepOdrReg1);
    (void)lisRead(REG_REFERENCE);
    (void)lisRead(REG_INT1_SRC);
  }

  // Read one HP-filtered sample, return |a|^2 in (m/s^2)^2.  Squared so the
  // classifier can threshold without sqrtf.
  float readMagnitudeSq() {
    lis.read();
    const float ax = lis.x_g * SENSORS_GRAVITY_EARTH;
    const float ay = lis.y_g * SENSORS_GRAVITY_EARTH;
    const float az = lis.z_g * SENSORS_GRAVITY_EARTH;
    return ax * ax + ay * ay + az * az;
  }

  // One classifier tick: collect samples for kTickMs and return the mean
  // squared magnitude (RMS^2).
  float sampleWindowMeanSq() {
    constexpr uint8_t kSamplesPerTick = 10;
    constexpr uint32_t kInterSampleDelayMs = cfg::kTickMs / kSamplesPerTick;
    static_assert(kInterSampleDelayMs * kSamplesPerTick == cfg::kTickMs,
                  "kTickMs must divide evenly into kSamplesPerTick");
    float sum = 0.0f;
    for (uint8_t i = 0; i < kSamplesPerTick; ++i) {
      sum += readMagnitudeSq();
      delay(kInterSampleDelayMs);
    }
    return sum / kSamplesPerTick;
  }

  // ----- Display rendering -----
  // Layout: top 16 px (yellow) = label, bottom 48 px (blue) = big number.
  void drawTimer(const uint32_t elapsed_ms, const char* const label) {
    display.clearBuffer();

    display.setFont(u8g2_font_6x12_tr);
    const int label_w = display.getStrWidth(label);
    display.drawStr((128 - label_w) / 2, 12, label);

    char buf[8];
    const float secs = elapsed_ms / 1000.0f;
    snprintf(buf, sizeof(buf), "%4.1f", secs);
    display.setFont(u8g2_font_logisoso38_tn);
    const int w = display.getStrWidth(buf);
    display.drawStr((128 - w) / 2, 62, buf);

    display.sendBuffer();
  }

  void error(const char* const message) {
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tr);
    display.drawStr(0, 32, message);
    display.sendBuffer();
    while (true) { delay(1000); }
  }

  // Forward declaration so enterSleep() can reset state on wake.
  void resetForDetecting(const uint32_t now);

  void enterSleep() {
    LOG("state=S enter sleep\n");
    display.setPowerSave(1);
    setLisSleep();
    state = State::Sleep;

    // Pause to ensure the LIS3DH has settled and any IRQ edge has cleared
    // before we arm the EXTI-wake.
    delay(50);
    (void)lisRead(REG_INT1_SRC);
    wake_flag = false;

#ifdef LOG_ENABLED
    // SAMD21 STANDBY shuts down USB, which would sever the CDC link.  For
    // dev builds we poll instead so logs keep flowing.
    while (!wake_flag) { delay(50); }
#else
    LowPower.deepSleep();
#endif

    // ---- Execution resumes here on motion-wake. ----
    wake_flag = false;
    setLisActive();
    display.setPowerSave(0);
    LOG("WAKE\n");
    resetForDetecting(millis());
  }

  void resetForDetecting(const uint32_t now) {
    state = State::Detecting;
    shot_start_ms    = now;
    last_active_ms   = now;
    last_pumplike_ms = now;
    drawTimer(0, "DETECTING");
    LOG("state=D start\n");
  }

  void tickDetecting() {
    const uint32_t now = millis();
    const float msq = sampleWindowMeanSq();
    const bool motion   = msq > cfg::kThActiveSq;
    const bool pumplike = motion && msq <= cfg::kThSpikeSq;

    if (motion)   last_active_ms   = now;
    if (pumplike) last_pumplike_ms = now;

#ifdef LOG_ENABLED
    char rmsbuf[10];
    LOG("tick state=D rms=%s cls=%c\n",
        log_f(rmsbuf, sizeof(rmsbuf), sqrtf(msq)), motionChar(msq));
#endif

    const uint32_t since_start  = now - shot_start_ms;
    const uint32_t since_motion = now - last_active_ms;

    // Commit to a brew once we've seen sustained motion.
    if (since_start >= cfg::kConfirmMs && motion) {
      state = State::Running;
      LOG("state=R confirm\n");
      return;
    }
    // Give up if motion didn't sustain.
    if (since_motion > cfg::kCalmDebounceMs && since_start > cfg::kDetectAbortMs) {
      LOG("state=D abort (no sustained motion)\n");
      enterSleep();
    }
  }

  void tickRunning() {
    const uint32_t now  = millis();
    const float msq     = sampleWindowMeanSq();
    const bool motion   = msq > cfg::kThActiveSq;
    const bool pumplike = motion && msq <= cfg::kThSpikeSq;
    const bool spike    = motion && !pumplike;

    if (motion)   last_active_ms   = now;
    if (pumplike) last_pumplike_ms = now;

    const uint32_t elapsed = now - shot_start_ms;
    drawTimer(elapsed, "BREWING");

#ifdef LOG_ENABLED
    char rmsbuf[10];
    LOG("tick state=R rms=%s cls=%c elapsed=%lu\n",
        log_f(rmsbuf, sizeof(rmsbuf), sqrtf(msq)),
        motionChar(msq), (unsigned long)elapsed);
#endif

    const uint32_t since_motion   = now - last_active_ms;
    const uint32_t since_pumplike = now - last_pumplike_ms;

    // End-of-brew triggers:
    //   1) sustained calm (vibration really stopped), or
    //   2) sustained "spike-only" -- the device is being moved around but no
    //      pump-like vibration has returned for a long time.
    const bool calm_end  = since_motion   > cfg::kCalmDebounceMs;
    const bool spike_end = spike && since_pumplike > cfg::kMaxSpikeOnlyMs;

    if (!(calm_end || spike_end)) return;

    // Final elapsed = up to the last pump-like sample, so a long tail of
    // shaking the cup doesn't inflate the reported shot time.
    final_elapsed_ms = (last_pumplike_ms > shot_start_ms)
                       ? (last_pumplike_ms - shot_start_ms) : 0;

    LOG("state=R end reason=%s final=%lu\n",
        calm_end ? "calm" : "spike-only", (unsigned long)final_elapsed_ms);

    if (final_elapsed_ms < cfg::kMinShotMs) {
      LOG("discard (shot < kMinShotMs)\n");
      enterSleep();
      return;
    }
    state = State::Holding;
    hold_start_ms = now;
    drawTimer(final_elapsed_ms, "DONE");
    LOG("state=H final=%lu\n", (unsigned long)final_elapsed_ms);
  }

  void tickHolding() {
    const uint32_t now = millis();
    const uint32_t since_hold = now - hold_start_ms;

    if (since_hold > cfg::kHoldDisplayMs) {
      LOG("state=H timeout\n");
      enterSleep();  // screen-off implicitly resets the timer
      return;
    }
    if (since_hold < cfg::kHoldRearmMs) {
      delay(cfg::kTickMs);
      return;
    }

    // Watch for the next brew. Any pump-like vibration restarts the timer.
    const float msq = sampleWindowMeanSq();
    if (msq > cfg::kThActiveSq) {
      LOG("state=H rearm (rms above kThActive)\n");
      resetForDetecting(now);
    }
  }

}  // namespace

void setup() {
  LOG_INIT();
  LOG("=== setup ===\n");

  Wire.begin();
  Wire.setClock(400000);

  display.begin();
  display.setContrast(255);

  if (!lis.begin(cfg::kLis3dhAddr)) {
    LOG("LIS3DH not found at 0x%02X -- halting\n", cfg::kLis3dhAddr);
    error("LIS3DH not found");
  }
  configureLis3dh();
  LOG("LIS3DH ok @0x%02X, OLED @0x%02X\n", cfg::kLis3dhAddr, cfg::kOledAddr);

  pinMode(cfg::kLis3dhInt1Pin, INPUT);
  LowPower.attachInterruptWakeup(cfg::kLis3dhInt1Pin, onWakeIsr, RISING);

  // Splash for half a second so the user knows we're alive, then sleep.
  drawTimer(0, "SETUP");
  delay(500);

  enterSleep();  // Returns only after the first motion-wake.
}

void loop() {
  switch (state) {
  case State::Sleep:
    LOG("state=S unexpected in loop()");
    // Safety net: should be unreachable, but if we ever land here just
    // re-enter sleep rather than spin.
    enterSleep();
    break;
  case State::Detecting: tickDetecting(); break;
  case State::Running:   tickRunning();   break;
  case State::Holding:   tickHolding();   break;
  }
}
