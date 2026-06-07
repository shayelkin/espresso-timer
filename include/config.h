#pragma once

#include <Arduino.h>

namespace cfg {

// ----- I2C addresses -----
// LIS3DH SA0/SDO pin tied HIGH -> 0x19. Tie LOW for 0x18 (avoid that:
// 0x18 collides with no common OLED but some breakouts default to it).
constexpr uint8_t kLis3dhAddr = 0x19;
constexpr uint8_t kOledAddr   = 0x3C;

// ----- Wiring -----
// XIAO D3 = PA11 = EXTINT[11]; connected to LIS3DH INT1.
constexpr uint8_t kLis3dhInt1Pin = 3;

// ----- Motion classification (m/s^2, RMS over one tick window) -----
// Sample magnitude is HP-filtered, so gravity is removed and only the AC
  // component is measured.
constexpr float kThActive = 0.30f;  // > idle (0.15) + margin, well below pump (1.3)
constexpr float kThSpike  = 2.50f;  // > pump (1.6) + margin for real knocks

// Squared variants: the hot path compares against |a|^2 so we never need
// sqrtf in the classifier.  sqrtf is only called at log sites.
constexpr float kThActiveSq = kThActive * kThActive;
constexpr float kThSpikeSq  = kThSpike  * kThSpike;

// ----- State-machine timings (ms) -----
constexpr uint32_t kTickMs         = 100;     // classifier window
constexpr uint32_t kConfirmMs      = 2000;    // motion sustained this long -> Running
constexpr uint32_t kDetectAbortMs  = 4000;    // give up if no commit within this
constexpr uint32_t kCalmDebounceMs = 1500;    // calm this long -> end of brew
constexpr uint32_t kMinShotMs      = 5000;    // brews shorter than this are noise
constexpr uint32_t kMaxSpikeOnlyMs = 60000;   // spike-only this long -> end brew
constexpr uint32_t kHoldDisplayMs  = 120000;  // keep result on-screen for 2 min
constexpr uint32_t kHoldRearmMs    = 2000;    // ignore motion right after a brew ends

// ----- LIS3DH CTRL_REG1 presets -----
// Bits: ODR[7:4] LPen[3] Zen[2] Yen[1] Xen[0]
constexpr uint8_t kActiveOdrReg1 = 0b01010111;  // 100 Hz, normal mode, XYZ on
constexpr uint8_t kSleepOdrReg1  = 0b00101111;  // 10 Hz, low-power, XYZ on

// ----- LIS3DH motion-wake threshold -----
// At +/-2 g range, 1 LSB ~ 16 mg. 3 LSBs ~ 0.05 g ~ 0.49 m/s^2 peak --
// just at the edge of the pump signal, so any vibration triggers wake.
constexpr uint8_t kWakeThresholdLsb = 0x03;

}  // namespace cfg
