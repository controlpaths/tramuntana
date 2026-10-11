# logic_to_vcd

Captures the eight DIO pins with the logic analyzer, triggered on an I2C
START, and saves the capture as `capture.vcd` for PulseView or GTKWave.

## Wiring

Nothing to wire on the Tramuntana side: the analyzer listens on DIO0..DIO7
(J1.9..J1.16). Connect the bus you want to watch to DIO6 (SCL) and DIO7
(SDA), with GND shared.

## Usage

```bash
python3 logic_to_vcd.py
pulseview capture.vcd          # add an I2C decoder on DIO6/DIO7
```

## How it works

`t.logic.capture()` takes 100 000 samples at 10 MS/s (10 ms of signal) with
a pattern trigger: mask `0x80`, value `0x00`, so it fires when DIO7 (SDA) is
low. The first time that happens after arming is normally a START condition.
2000 samples before the trigger are kept so the START itself is visible.
The capture waits up to 10 s for the trigger.

The script prints the number of transitions on each pin and writes the VCD
with `lc.to_vcd()`.
