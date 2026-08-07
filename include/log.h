#pragma once

#ifdef LOG_ENABLED

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

constexpr unsigned long kLogBaud = 115200;

inline void log_printf(const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
}

inline const char* log_f(char* buf, size_t buflen, float v, int width = 5, int prec = 2) {
  dtostrf(v, width, prec, buf);
  (void)buflen;
  return buf;
}

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
