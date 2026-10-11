# 20. Examples

[← AI tools](19-ai-tools.md) · [Contents](README.md) · [Next: Troubleshooting →](21-troubleshooting.md)

[`host/python/examples/`](../host/python/examples) has ready-made scripts,
one folder each, with a README covering wiring, usage and how it works.
Run them with the package installed (or from `host/python` with
`source env.sh` done first):

```bash
cd host/python/examples/spi_flash_id
python3 spi_flash_id.py
```

| Example | What it does | Instruments it shows |
|---|---|---|
| [`board_test`](../host/python/examples/board_test) | functional test of a board: flashes the firmware, asks for six jumpers on J1 and tests every instrument | everything; flashing through the API |
| [`spi_flash_id`](../host/python/examples/spi_flash_id) | JEDEC ID and first page of a SPI NOR flash | SPI `txn` |
| [`nrf24_ble_scan`](../host/python/examples/nrf24_ble_scan) | passive BLE scanner with an nRF24L01: lists devices advertising nearby | SPI, GPIO (CE), IRQ polling |
| [`i2c_regs`](../host/python/examples/i2c_regs) | scan the I2C bus and dump the first 32 registers of every device | I2C scan, register reads |
| [`eeprom`](../host/python/examples/eeprom) | read a 24xx I2C EEPROM to a `.bin`, or write one and verify | I2C paged writes, ACK polling |
| [`ds3231m_rtc`](../host/python/examples/ds3231m_rtc) | DS3231M RTC: read, set from the PC clock, live view, check the 1 Hz output | I2C, display, logic analyzer |
| [`vl53l0x_range`](../host/python/examples/vl53l0x_range) | VL53L0X time-of-flight distance, with ST's init sequence | I2C register sequences |
| [`ssd1306_oled`](../host/python/examples/ssd1306_oled) | 128×32 SSD1306 OLED: frame buffer, font, live AIN0 readout | I2C bulk writes, ADC |
| [`hcsr04_range`](../host/python/examples/hcsr04_range) | HC-SR04 ultrasonic distance, echo timed with the logic analyzer | GPIO trigger, logic analyzer timing |
| [`scope_plot`](../host/python/examples/scope_plot) | AWG sine captured by the scope and plotted (one wire: AOUT0 → AIN0) | AWG, scope, matplotlib |
| [`logic_to_vcd`](../host/python/examples/logic_to_vcd) | logic capture triggered on an I2C START, saved for PulseView | logic pattern trigger, VCD |
| [`sniff_log`](../host/python/examples/sniff_log) | log every I2C and SPI message on the header, timestamped | sniffers, events |

## The board test

`board_test` is also the regression test for the firmware. It needs six
wires on J1, and nothing else connected:

| From | To | Tests |
|---|---|---|
| J1.7 AOUT0 | J1.3 AIN0 | waveform generator → ADC / scope ch0 |
| J1.1 3V3 | J1.5 AIN1 | target power rail → ADC / scope ch1 |
| J1.9 DIO0 | J1.10 DIO1 | GPIO, UART bridge loopback, PWM |
| J1.12 DIO3 | J1.13 DIO4 | GPIO, SPI MISO ← MOSI loopback |
| J1.11 DIO2 | J1.15 DIO6 | GPIO, SPI CS / I2C SCL |
| J1.14 DIO5 | J1.16 DIO7 | GPIO, SPI SCK / I2C SDA |

```bash
python3 board_test.py                  # flash fw/build, then test
python3 board_test.py --no-flash       # test the firmware already on the board
python3 board_test.py --no-visual      # skip the LED / screen questions
```

## A self-test for the debugging features

The emulator, the sequencer and the DIO watch can be checked with the
board's own masters and four jumpers:
**DIO2↔DIO6, DIO5↔DIO7, DIO4↔DIO0, DIO3↔DIO1**.

```python
from tramuntana import Tramuntana, Sequence, Timeline

with Tramuntana() as t:
    tl = Timeline(t); tl.start(watch=1 << 0)

    # I2C: emulated device on DIO2 (SCL) / DIO5 (SDA), board master on DIO6/DIO7
    t.emu.set_regs(0xD0, b"\x60")
    t.emu.i2c(0x76, scl=2, sda=5, pullups=True)
    t.i2c.config(400_000, True)
    print(t.i2c.reg_read(0x76, 0xD0).hex())          # 60
    t.emu.stop()

    # sequencer + DIO watch: a pulse on DIO4 shows up on DIO0
    r = t.seq.run(Sequence().dio_dir(0x10, 0x10).dio_write(0x10, 0)
                  .delay_us(500).dio_write(0x10, 0x10).dio_dir(0x10, 0))
    print(r.state)

    # SPI: emulated device on DIO6/7/0/1, board master on DIO2..5
    # (DIO0 now carries MOSI: too fast for the watch, which mutes it)
    tl.watch(0)
    t.dio_dir(0x3C, 0)                               # release the SPI pins first
    t.emu.set_regs(0x00, b"\xe5\x01\x02")
    t.emu.spi(mode=0, cs=6, sck=7, mosi=0, miso=1)
    print(t.spi.txn(b"\x80", 3).hex())               # e50102
    t.emu.stop()

    print(tl.format())
    tl.stop()
```

[← AI tools](19-ai-tools.md) · [Contents](README.md) · [Next: Troubleshooting →](21-troubleshooting.md)
