#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

// USB-CDC logging.  Toggle the LOG_ENABLED build flag in platformio.ini.
//
// NOTE: SAMD21 USB is killed by LowPower.deepSleep().  When LOG_ENABLED is
// set, enterSleep() in main.cpp falls back to a polling wait so the CDC
// link stays up between brews.  Disable LOG_ENABLED for production builds
// to recover real standby current.

#ifdef LOG_ENABLED

constexpr unsigned long kLogBaud = 115200;

inline void log_printf(const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
}

// Format a float into a small fixed buffer.  Newlib-nano's printf doesn't
// always include %f, so route every float through dtostrf instead.
inline const char* log_f(char* buf, size_t buflen, float v, int width = 5, int prec = 2) {
  dtostrf(v, width, prec, buf);
  (void)buflen;
  return buf;
}

// Every line is prefixed with "[<millis>] " automatically.  Call sites
// only carry the actual message + their own args.  Uses GCC's
// ##__VA_ARGS__ to swallow the comma when no extra args are passed.
#define LOG(fmt, ...) \
  ::log_printf("[%lu] " fmt, (unsigned long)millis(), ##__VA_ARGS__)
#define LOG_INIT()                                            \
  do {                                                        \
    Serial.begin(kLogBaud);                                   \
    uint32_t _t0 = millis();                                  \
    while (!Serial && (millis() - _t0) < 1500) { delay(10); } \
  } while (0)

#else

#define LOG(...) ((void)0)
#define LOG_INIT() ((void)0)

#endif
