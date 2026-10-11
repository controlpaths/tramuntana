# hcsr04_range

Live distance from an HC-SR04 ultrasonic sensor, printed with a bar and
shown on the board's screen, until Ctrl-C.

## Wiring

| Sensor | Tramuntana |
|---|---|
| VCC | J1.1 (3V3) |
| GND | J1.2 |
| TRIG | J1.10 (DIO1) |
| ECHO | J1.11 (DIO2) |

J1 has no 5 V pin. The classic HC-SR04 is specified for 5 V and many of
them don't answer (or range erratically) at 3.3 V; the HC-SR04P and
RCWL-1601 versions work from 3 to 5.5 V. If yours needs 5 V, power it from
an external 5 V supply with GND shared: its 5 V ECHO is fine on DIO2, the
RP2350's digital pins are 5 V tolerant while the board is powered.

DIO1 is also the UART bridge TX: close anything that has `/dev/ttyACM2`
open, or the pin stays busy.

## Usage

```bash
python3 hcsr04_range.py                 # until Ctrl-C
python3 hcsr04_range.py --temp 30       # speed of sound at 30 C
python3 hcsr04_range.py -n 20           # 20 readings
python3 hcsr04_range.py --scale 1.075   # calibrated against a ruler
```

| Option | Default | |
|---|---|---|
| `--temp` | 20 | air temperature in C, for the speed of sound |
| `--scale` | 1 | calibration: the distance is multiplied by this |
| `--offset` | 0 | calibration: cm added after scaling |
| `--period` | 0.1 | seconds between readings (the sensor wants >= 60 ms) |
| `-n` | 0 | number of readings, 0 = until Ctrl-C |

## How it works

A measurement starts on the falling edge of a TRIG pulse at least 10 us
long. The sensor sends a 40 kHz burst and raises ECHO for as long as the
sound takes to come back: `distance = width * c / 2`, with
`c = 331.3 + 0.606 * T` m/s. With nothing in range ECHO stays high for
about 38 ms.

The host cannot time a pulse that short over USB, so the logic analyzer
does it:

1. A running I2C or SPI sniffer is stopped (the webapp may leave one on):
   they share the capture memory with the logic analyzer. DIO1 is set as
   a GPIO output, DIO2 as an input.
2. The analyzer is armed on a rising edge of DIO2 at 250 kS/s (4 us per
   sample, 0.7 mm of distance) for 40 ms, longer than the "no echo" pulse.
   An edge trigger without pre-trigger runs in the PIO itself, so sample 0
   is the start of the echo.
3. TRIG is driven high and low with two `dio_write()` calls. Each one is a
   USB round trip, far more than the 10 us the sensor needs.
4. The capture is downloaded and the echo width is the index of the first
   low sample on DIO2. No rising edge within 100 ms, ECHO still high at the
   end of the window, or more than 4 m: shown as "no echo".

## Accuracy and calibration

Below about 15 cm a sensor running at 3.3 V may miss the first echo and
read several cm long (10 cm read as 16.5 cm on the unit this was tested
with). From there on it is stable to about 1 mm, but its time base can be
off by several percent. Put a flat, hard target square to the sensor at two
distances measured from the front of the transducers (e.g. 30 and 60 cm),
take the ratio real / measured, and pass it as `--scale`; the test unit
read 7-8 % short (30 cm -> 27.7 cm, 50 cm -> 46.8 cm), so `--scale 1.075`.
A constant error left after that goes in `--offset`. Powering the sensor
from 5 V removes most of both problems.
