# espresso-timer

Automatic, Arduino-based espresso shot timer, that uses an accelerometer to detect when the espresso
machine's pump start and stops.

## Hardware

- [Seeed XIAO SAMD21](https://wiki.seeedstudio.com/Seeeduino-XIAO/).
- [LIS3DH 3-axis MEMS accelerometer](https://www.st.com/en/mems-and-sensors/lis3dh.html).
- [SSD1315 128x64 display](https://www.orientdisplay.com/wp-content/uploads/2021/02/SSD1315.pdf).

Both LIS3DH and SSD1315 share the I2C bus.

### Wiring diagram

```
  SSD1315        XIAO SAMD21         LIS3DH
    VCC ──────────── 3V3 ────────────  VIN
    GND ──────────── GND ────────────  GND
    SDA ────────────  D4 ────────────  SDA
    SCL ────────────  D5 ────────────  SCL
                      D3 ────────────  INT1
                     3V3 ────────────  SA0  (sets addr to 0x19)
```

## Algorithm

The LIS3DH's high-pass-filtered magnitude is sampled in 100 ms ticks and classified as one of three
states: idle, pump, or spike. A state machine tracks the current state, starts the time on pump
motion, freezes when the pump's vibration stops, and ignores incidental spikes.

Thresholds are set in [config.h](include/config.h).

### State machine

```
                  motion IRQ (INT1)
        ┌───────┐ ───────────────────► ┌───────────┐
        │ Sleep │                      │ Detecting │
        └───────┘ ◄─────────────────── └───────────┘
            ▲       no sustained motion    │     ▲
            │           (~4sec)            │     │ new motion
            │                  2sec motion │     │ (after 2sec grace)
            │                              ▼     │
            │  shot time < 5 sec     ┌─────────┐ │
            ├────────────────────────│ Running │ │
            │                        └─────────┘ │
            │            1.5 sec calm OR  │      │
            │           1 min spike-only  |      │
            |                             ▼      |
            │  2min timeout         ┌─────────┐  │
            └───────────────────────│ Holding │──┘
                                    └─────────┘
```

## Build & flash

This project uses [PlatformIO](https://platformio.org/) to configure and build:

```
pio run            # build
pio run -t upload  # build + flash over USB
```

## License

This software is licensed under the terms of the [MIT license](LICENSE).
