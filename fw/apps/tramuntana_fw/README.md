# tramuntana_fw — Tramuntana Lite r1a firmware

Zephyr firmware for the Tramuntana Lite r1a, a bench tool for poking at,
listening to and debugging embedded systems: SPI and I2C master + sniffer,
oscilloscope, waveform generator, 8-channel logic analyzer and a USB-UART
bridge, on the RP2350 of a Raspberry Pi Pico 2.

Everything the board can do is reachable three ways, and they all drive the
same instrument code:

| Front end | Port | For |
|---|---|---|
| Shell | `/dev/ttyACM0` (interface 0) | humans in a terminal (`picocom /dev/ttyACM0`, any baud) |
| Binary API | `/dev/ttyACM1` (interface 2) | scripts: [`host/python`](../../../host/python) and [`host/webapp`](../../../host/webapp) |
| UART bridge | `/dev/ttyACM2` (interface 4) | a plain serial port wired to the target's UART on the header |

The ports enumerate with the chip's unique id as USB serial number, so
`/dev/serial/by-id/usb-controlpaths_Tramuntana_Lite_r1a_<id>-if0{0,2,4}` is
stable across reboots and boards.

## Build and flash

```bash
cd fw
./build.sh tramuntana_fw          # -> build/tramuntana_fw/zephyr/zephyr.uf2
./build.sh flash tramuntana_fw    # asks the running firmware to reboot into the
                                  # UF2 bootloader, then copies the file over
```

`build.sh` takes the Zephyr workspace and SDK locations from `fw/.env`
(copy `fw/.env.example`; defaults `~/zephyrproject` and `~/zephyr-sdk-1.0.1`,
see the [main README](../../../readme.md#building-the-firmware)). The first flash
(or recovering from a firmware that hangs) needs BOOTSEL held while plugging
the board in; after that the `boot` shell command / `CMD_BOOTLOADER` do it.

## Header map

```
J1 pin  signal   RP2350   used by
  1     3V3      GPIO11   switched target rail (power on/off)
  3     AIN0     GPIO26   scope ch0, adc
  5     AIN1     GPIO27   scope ch1, adc
  7     AOUT0    GPIO8    waveform generator (PWM + 2k7/1nF RC)
  9     DIO0     GPIO7    UART RX,  logic ch0
 10     DIO1     GPIO6    UART TX,  logic ch1
 11     DIO2     GPIO5    SPI CS,   logic ch2
 12     DIO3     GPIO4    SPI MISO, logic ch3
 13     DIO4     GPIO3    SPI MOSI, logic ch4
 14     DIO5     GPIO2    SPI SCK,  logic ch5
 15     DIO6     GPIO1    I2C SCL,  logic ch6
 16     DIO7     GPIO0    I2C SDA,  logic ch7
```

`DIOn = GPIO(7-n)`. Every DIO is a plain GPIO plus exactly one bus
function, so the UART bridge, the SPI master and the I2C master can all run
at the same time. The logic analyzer and the sniffers only *listen*, so
they can run on top of whatever is driving the pins — including this board's
own SPI/I2C master or the UART bridge (that is how the firmware was tested
without a single wire: the analyzer captured the board's own I2C and UART
traffic). Pins that get *driven* go through a small arbiter (`board.c`): the
SPI and I2C masters and plain GPIO use hold their pins "softly" between
operations, and the UART bridge holds DIO0/DIO1 exclusively while a host has
`/dev/ttyACM2` open — driving those two pins as GPIO then gets `busy`
instead of fighting the UART.

## Instruments

### SPI master (`spi`) — SPIDriver style
Hardware SPI0 at up to 75 MHz with chip select on DIO2 driven by hand, so a
transaction can be built step by step from the host: `sel`, `write`, `read`,
`xfer`, `unsel`, or in one go with `txn` (sel + write + read + unsel). Modes
0–3, MSB/LSB first, up to 4096 bytes per command. CS is never handed to the
peripheral because the PL022 pulses its hardware CS once per *word*, which
breaks every multi-byte transaction.

### SPI sniffer
A PIO0 state machine waits for CS low and samples **both MOSI and MISO** on
every active SCK edge (2 bits per clock, autopush every 8 clocks), pushes a
marker when CS rises, and a DMA ring streams it all to RAM. Messages arrive
as `EVT_SPI_MSG` events: total length, first 256 bytes of each direction,
overrun flag. Up to ~15 MHz SCK.

### I2C master (`i2c`) — I2CDriver style
Bit-banged on DIO6/DIO7 (the bit-bang code lives in RAM), 1 kHz – 1 MHz,
open-drain, honours clock stretching. High level: `scan`, `write`, `read`,
`write_read` (write, repeated START, read — register access). Low level:
`start`, `raw write`, `raw read`, `stop`, so any sequence a datasheet asks
for can be scripted, with per-byte ACK results. `reset` clocks a stuck slave
free. The board's pull-up bias on DIO6/DIO7 (`pullups on`) is switchable
because the target may or may not have its own; the effective bus speed is
limited by the pull-ups' rise time, never faster than configured.

### I2C sniffer
PIO0 watches SCL/SDA, pushes one event per bit / START / STOP, and the
decoder emits `EVT_I2C_MSG` events: (START address+R/W)(ACK)(DATA)(ACK)…
(STOP), repeated STARTs included.

### Scope (`scope`)
AIN0 or AIN0+AIN1 (round robin), 12-bit, up to 500 kS/s (per sample: a
2-channel capture gives 250 kS/s per channel), up to 16384 samples. The ADC
free-runs into a DMA ring and the trigger (none / rising / falling on AIN0,
level 0–4095) is found by scanning the ring, so captures have **pre-trigger**
samples and never miss the edge. Arm / status / read from the API, blocking
from the shell.

### Waveform generator (`awg`)
Sine, triangle, sawtooth, square (duty), stairs (steps), DC, step, and an
**arbitrary** table of up to 4096 samples uploaded from the host — 1 Hz to
100 kHz, amplitude and offset in millivolts. A PWM on GPIO8 through the
board's RC filter, fed by a self-restarting DMA loop, so it keeps running
while everything else works.

### Logic analyzer (`logic`)
8 channels on DIO0..7 into a 128 KB ring, PIO sampled:

| Trigger | Max rate | Pre-trigger | How |
|---|---|---|---|
| none | 150 MS/s | – | linear DMA of n samples |
| rising / falling on one pin | 150 MS/s | – | `wait` instructions in the PIO program, zero latency |
| pattern: `(sample & mask)` becomes `== value` | 15 MS/s | yes | endless DMA ring, CPU scans 4 samples/word for a non-matching sample, then the first match |

Up to 131072 samples (122880 in pattern mode). The host library exports
VCD (PulseView, GTKWave) and CSV.

### UART bridge (`uart`)
UART1 on DIO1 (TX) / DIO0 (RX) bridged to the third CDC-ACM port. GPIO6/7
are UART1's CTS/RTS pins; the RP2350's UART_AUX function (F11) puts TX/RX on
them, which frees DIO6/DIO7 for I2C. Wire DIO1 to the target's RX and DIO0
to its TX. In the
default `auto` mode it is on whenever a host program holds the port open
(DTR), and the baud rate, parity and stop bits follow the host's line
coding: `picocom -b 9600 /dev/ttyACM2` just works. `on`/`off` force it,
`--baud` overrides the host's rate.

Control lines (`uart ctrl`): the host's RTS/DTR on the bridge port can drive
two more DIOs (DIO2..DIO7) as open-drain outputs, the way a USB-serial
adapter resets a target and selects its bootloader. `esp` reproduces the
two-transistor auto-reset of ESP32/ESP8266 boards (reset low only with RTS
and not DTR, boot low only with DTR and not RTS), `direct` follows the lines
as they are. While it's on the bridge stays active regardless of DTR, and
every DTR/RTS change is reported as `EVT_LINE`.

### Emulated I2C/SPI target (`emu`)
The board answers on the target's bus from a 256-byte register file (auto-
incrementing pointer, optional read-only registers), on PIO1 with its own
interrupt. I2C: two state machines, one shifting 9-bit frames and one
watching for START/STOP; the board stretches SCL after every START and
every byte while the ISR decides ACK/NAK and the next byte, so it keeps up
at any bus speed (tested 100 kHz - 1 MHz). SPI: three state machines (MOSI
capture, MISO shifter, CS-rise detector) and two DMA channels: the shifter
turns the command byte into the address of that command's row in a
256 x 32-byte response table and DMA channel A writes it into channel B's
READ_ADDR_TRIG, so the response starts without the CPU (up to 8 MHz in
modes 0/2, about 2 MHz in modes 1/3). Every transfer becomes an `EVT_EMU`.

### Sequencer (`seq`)
A byte-coded program (up to 2 KB, see `src/seq.h`) the host uploads and the
board runs on its own thread: GPIO, delays, waits on DIO levels, I2C/SPI
transfers, UART bytes, AWG/ADC, loops, arming captures. Steps follow each
other within microseconds; each one leaves a record (status, board time,
data) the host reads back. `EVT_SEQ_DONE` at the end.

### Timestamps and DIO watch (`time`, `watch`)
Every timestamped event uses one clock, the board's microseconds since boot
(`board_time_us()`, 32 bits). `watch` reports edges on chosen DIOs as
`EVT_DIO` from a GPIO interrupt (muted for 100 ms if a pin goes over 50
edges/ms). With stamps on (`TIME` with payload 1) the sniffer events carry
the board time of their START / CS edge too: the sniffer programs raise a
PIO interrupt at that edge and `src/stamp.c` records the time with the DMA
ring position, so the stamp is exact however late the decode thread runs.

### Board (`power`, `pullups`, `led`, `dio`, `adc`, `note`)
Target 3.3 V rail, I2C pull-up bias, RGB LED, DIO as plain GPIO, single-shot
ADC readings (a quick voltmeter), and a free-text line on the display for
scripts to label what they are doing.

### Display
A dashboard rather than per-command screens: one status line per instrument,
an "activity" flash in the title bar (`SPI 4B`, `I2C W 50 2B`, `UART on`…),
and a plot area showing the last scope trace or logic capture. Rendering runs
in its own low-priority thread from a snapshot, so no instrument ever waits
for the 30 ms SPI write of a frame.

`pinout` (shell) / `DISPLAY_PINOUT` (API) swaps the dashboard for a drawing
of J1 as it sits on the board — pin 1 at the top, odd pins in the inner
column — with each pin's name and function, for 60 s by default or until
dismissed, as a wiring aid. The pin table lives in `display.c` and must
match `host/python/tramuntana/pinout.py`.

## Shell

```
info | pins | boot | note <text> | pinout [seconds|hold|off]
led <off|red|green|blue|yellow|cyan|magenta|white|auto>
power [on|off]      pullups [on|off]      adc <0|1>
dio dir <mask> <out_mask> | dio write <mask> <value> | dio read
spi conf <hz> [mode] [lsb] | sel | unsel | write <hex..> | read <n>
    | xfer <hex..> | txn <read_n> <hex..> | sniff <on|off> [mode]
i2c conf <hz> [pu on|off] | scan | write <addr> <hex..> | read <addr> <n>
    | wrrd <addr> <n> <hex..> | start <addr_rw> | stop
    | raw write <hex..> | raw read <n> | sniff <on|off> [pu] | reset
scope <n> [ch] [clkdiv>=96] [trig 0|1|2] [level] [pre] | scope dump [off] [cnt]
awg <sine|tri|saw|square|stairs|arb> <hz> [amp_v] [off_v] [param]
    | dc <v> | step <from> <to> <ms> | off | status
pwm set <dio> <hz> <duty%> [inv] | phase <dio> <deg> | sync [dio_mask]
    | stop <dio|all> | status
logic <n> [clkdiv>=1] [trig 0|1|2|3] [pin] [mask] [value] [pre] | logic dump [off] [cnt]
uart <auto|on|off> [baud] | uart status | uart ctrl <off|direct|esp> [reset_dio] [boot_dio]
time | watch <dio_mask|off>
emu i2c <addr> [scl] [sda] [pu] | emu spi <mode> [cs sck mosi miso] [amask rmask rval status]
    | emu off | emu set <reg> <hex..> | emu regs [reg] [n] | emu status
seq status | seq abort
```

Bytes and addresses are hex, volts decimal (`1.65`). While a sniffer is on
from the shell, every message is echoed as one line
(`spi 4B  MOSI 9f a5 00 00  MISO 00 00 00 00`, `i2c S 50W- P`).

PWM outputs use the hardware PWM slices: the DIO pairs 7/6, 5/4, 3/2 and
1/0 each share one slice, so the two pins of a pair run at the same
frequency (each with its own duty and polarity), giving up to four
independent frequencies. `pwm sync` restarts the pairs on the same clock
edge, each delayed by its `pwm phase`.

## Binary API

`src/proto.h` is the reference; `host/python/tramuntana/proto.py` and
`host/webapp/js/proto.js` mirror it.

```
frame:  AA 55 | cmd u8 | seq u8 | len u16 | payload[len] | crc16 u16     (little-endian)
crc:    CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over cmd..payload
reply:  cmd | 0x80, same seq, payload = status u8 + data
event:  cmd 0xF0..0xFF, seq 0, unsolicited (may arrive between a request and its reply)
```

Status: 0 OK, 1 invalid argument, 2 busy, 3 timeout, 4 bus error/NAK,
5 not supported, 6 not permitted, 7 no memory, 8 unknown command, 9 wrong
state. Commands stay below 0x70 so a reply id can never look like an event.

| cmd | name | request → reply |
|---|---|---|
| 01 | PING | → `"TRMT"`, proto version |
| 02 | INFO | → fw[16], board[16], sysclk u32, chip_id[8], features u32 |
| 03 | BOOTLOADER | reboot into UF2 |
| 04 | LED | r, g, b (0/1); `FF x x` = automatic |
| 05 / 06 | POWER / PULLUPS | [on u8] → on u8 |
| 07 / 08 / 09 | DIO_DIR / DIO_WRITE / DIO_READ | mask, out_mask / mask, value / → value |
| 0A | STATUS | → pins u32 (4 bits per DIO: owner, exclusive), power, pullups, scope, logic, awg, spi_sniff, i2c_sniff, bridge, rx_dropped u32 |
| 0B | DISPLAY_TEXT | text (≤24) |
| 0D | DISPLAY_PINOUT | seconds u16: J1 drawing on the screen (0 = dashboard, FFFF = hold) |
| 0E | TIME | [stamp u8] → t_us u32; stamp = 1 makes the sniffer events carry t_us |
| 0F | DIO_WATCH | mask u8 → EVT_DIO on every edge (0 = off) |
| 0C | ADC_READ | channel → mV u16 |
| 10 | SPI_CONF | hz u32, mode u8, lsb u8 → hz u32 |
| 11 / 12 | SPI_SEL / SPI_UNSEL | |
| 13 / 14 / 15 | SPI_WRITE / SPI_READ / SPI_XFER | data / n u16, fill u8 → data / data → data |
| 16 | SPI_TXN | read_n u16, data → rx[read_n] |
| 17 | SPI_SNIFF | on u8, [mode u8] |
| 20 | I2C_CONF | hz u32, pullups u8 → hz u32 |
| 21 | I2C_SCAN | → 16-byte bitmap |
| 22 / 23 | I2C_WRITE / I2C_READ | addr, data → acked u16 / addr, n u16 → data |
| 24 | I2C_WRITE_READ | addr, wn u8, w[wn], rn u16 → data |
| 25 / 26 | I2C_START / I2C_STOP | addr_rw u8 → ack u8 / |
| 27 / 28 | I2C_RAW_WRITE / I2C_RAW_READ | data → acked u16 / n u16, nack_last u8 → data |
| 29 / 2A | I2C_SNIFF / I2C_RESET | on u8, pullups u8 / |
| 30 | SCOPE_ARM | channels u8, n u16, clkdiv u16, trig u8, level u16, pre u16, timeout_ms u16 → rate u32 |
| 31 | SCOPE_STATUS | → state u8, channels u8, n u16, rate u32, trig_index u16 |
| 32 / 33 | SCOPE_READ / SCOPE_ABORT | offset u16, count u16 (≤2048) → u16 samples / |
| 40 | AWG_SET | shape u8, hz u32, amp_mv u16, off_mv u16, param u16 → mHz u32, n u16, top u16 |
| 41 / 42 | AWG_OFF / AWG_STATUS | / → running, shape, mHz u32, amp, off |
| 43 / 44 / 45 | AWG_ARB_LOAD / AWG_DC / AWG_STEP | offset u16, u8[] / mV u16 / from, to, ms |
| 50 | LOGIC_ARM | n u32, clkdiv u16, trig u8, pin u8, mask u8, value u8, pre u32, timeout_ms u16 → rate u32 |
| 51 | LOGIC_STATUS | → state u8, n u32, rate u32, trig_index u32 |
| 52 / 53 | LOGIC_READ / LOGIC_ABORT | offset u32, count u16 (≤4096) → bytes (bit i = DIOi) / |
| 60 / 61 | UART_CONF / UART_STATUS | mode u8, baud u32 / → active, baud, rx, tx, mode, ctrl, reset_dio, boot_dio, lines |
| 62 | UART_CTRL | mode u8 (0 off, 1 direct, 2 esp), reset_dio u8, boot_dio u8 (FF = none) |
| 64 / 65 | EMU_I2C / EMU_SPI | on u8, addr, scl, sda, pullups / on u8, mode, cs, sck, mosi, miso, addr_mask, read_mask, read_value, status |
| 66 | EMU_REGS | op 0: offset u8, data[] / op 1: offset u8, n u16 → data / op 2: read-only mask[32] |
| 67 | EMU_STATUS | → kind u8, txns u32, reads u32, writes u32, naks u32 |
| 68 / 69 | SEQ_LOAD / SEQ_RUN | offset u16, code[] / len u16, timeout_ms u32 |
| 6A / 6B / 6C | SEQ_STATUS / SEQ_RESULT / SEQ_ABORT | → state, error, pc u16, result_len u16 / offset u16, n u16 → records / |
| F1 | EVT_SPI_MSG | flags (bit 7 = stamped), [t_us u32], total u16, mosi[k], miso[k] |
| F2 | EVT_I2C_MSG | flags (bit 7 = stamped), [t_us u32], (type, value)* — 1 START addr_rw, 2 ACK/NAK, 3 DATA, 4 STOP |
| F3 | EVT_CAPTURE_DONE | instrument (0 scope, 1 logic), state |
| F4 | EVT_DIO | t_us u32, levels u8, changed u8, lost u8 |
| F5 | EVT_EMU | t_us u32, kind u8 (1 i2c, 2 spi), flags u8 (0 read, 1 nak, 2 truncated), addr u8, reg u8, data[] |
| F6 | EVT_LINE | t_us u32, dtr u8, rts u8 |
| F7 | EVT_SEQ_DONE | t_us u32, state u8, error u8, pc u16, result_len u16 |

Captures are arm → poll STATUS → READ in pages, so the port never blocks;
`state` is 0 idle, 1 armed, 2 running, 3 done, 4 timeout, 5 aborted.

## Source map

```
src/
  main.c         boot order
  usb.c          composite device, three CDC-ACM functions
  port.c         irq-driven ring-buffered serial port (USB backpressure aware)
  proto.c/h      API framing, dispatch table, events         <- protocol reference
  shell_cmds.c   shell wrappers
  bridge.c       USB <-> UART1
  board.c/h      LED, power, pull-ups, DIO, ADC one-shot, cycle delays, pin arbiter
  display.c/h    ST7735R primitives + dashboard thread
  arena.c/h      capture memory (32 KB scope ring + 128 KB shared ring)
  dmaring.c/h    endless circular DMA of any size (4-channel ping-pong)
  spi.c  i2c.c  scope.c  awg.c  logic.c    the instruments
  emu.c/h        emulated I2C/SPI target on PIO1
  seq.c/h        sequencer                                    <- program format reference
  watch.c/h      timestamped DIO edges
  stamp.c/h      exact timestamps for sniffer messages (PIO0 irq flag 0)
boards/rpi_pico2_rp2350a_m33.overlay   3x CDC-ACM, capture arena region, uart1 on GPIO6/7, pio1
```

Memory: the devicetree overlay shrinks Zephyr's SRAM to 320 KB and reserves
192 KB at `0x20050000` as `CAPTURE_ARENA` for the DMA rings, at addresses
aligned to their sizes, instead of letting the linker pad `.bss`.

## Things learned the hard way (kept so nobody relearns them)

- **The RP2350 DMA's ring mode tops out at 32 KB** (`RING_SIZE` is a 4-bit
  field). Anything bigger silently wraps wrong — the old firmware's 64 KB
  ring actually disabled wrapping. `dmaring.c` builds larger rings from two
  data channels plus two control channels that rewrite the write address.
- **PIO `in pins, 8` with autopush stores the four samples of a word in
  memory order only with the ISR shifting *right*.** Shifting left reverses
  them and every transition looks like a 3-sample glitch.
- **Scan behind the DMA.** The write pointer moves a few cycles before the
  data lands, and the ring still holds the previous capture underneath; a
  trigger scanner that reads the newest word can match stale data.
- **Command ids stay below 0x70**: `cmd | 0x80` for 0x60+ collides with an
  event range at 0xE0.
- **CDC-ACM backpressure**: when the RX ring is full the callback must
  *disable* RX interrupts, not just stop reading — the class driver
  resubmits its work item for as long as data is pending, and on a
  cooperative workqueue that spins the whole system.
- **Bit-banging from flash is slow the first time**: XIP cache misses added
  6–15 µs at the start of every I2C byte. The bit-bang path is `__ramfunc`,
  and its delays are precomputed in cycles (a 64-bit divide per delay cost
  ~1 µs on the M33).
- **The PIO reads every pad regardless of FUNCSEL**, so passive instruments
  don't need to own pins — and can watch this board's own traffic.
- The Zephyr PL022 driver in polling mode leaves ~1.5 µs between bytes, so
  bulk SPI throughput is ~150 kB/s regardless of clock; DMA mode
  (`CONFIG_SPI_PL022_DMA`) is the obvious next step if it matters.
- `k_msgq_purge()` wakes a thread blocked in `k_msgq_get()` with
  `-ENOMSG`: check the return value, or the thread sends whatever stale
  message is in its local buffer (emu.c did, once, on every stop).
- PIO autopull refills the OSR in the background as soon as it is empty and
  the TX FIFO has data, not only on the next `out`: a program that must
  take one specific word first (the SPI target's status byte) uses
  explicit `pull`s instead.
- Two state machines writing the same pin direction (the I2C target's SCL
  stretch) is fine as long as they never disagree at the same time: the
  last write wins, so a side-set on every instruction would undo the other
  machine's stretch. Only explicit `set pindirs` touch SCL there.
- The Zephyr GPIO driver's callback list is shared: a callback registered
  for all DIOs is called for edges another module enabled, so watch.c
  filters by its own mask.
- The PL022 is only reprogrammed by the next transfer, so after a mode
  change SCK sits at the old CPOL until then - which used to be after CS
  went low, one spurious clock edge for the target. `spi_drv_sel()` now
  settles it with a dummy byte while CS is still high. And CS is configured
  as a high SIO output before its function switch (board.c), never passing
  through SPI0's own CS output: that was a 300 ns select glitch.
- The SPI master re-claims its pins if any of the four was taken (it used to
  check only CS and SCK, and a GPIO left on MOSI silenced it).
- The sequencer's busy waits run above most threads, so the UART bridge
  thread sits above it (priority 1): otherwise a long run held up the
  target's bytes, a new baud rate and DTR/RTS changes until it ended.
- The I2C sniffer and the emulator restore the pull-ups when they stop,
  which used to undo a `pullups on` given while they ran. They now only
  restore if `board_pullups_changes()` hasn't moved since their own set.
- The I2C emulator failed with an Arduino at ~470 kHz (fine with this
  board's slower master): the byte machine released SCL on every bit,
  cancelling the stretch the condition machine puts on SCL after a START,
  so the master clocked the address in before the CPU had restarted the
  byte machine (it received 0xF6 for 0xEC). And after that stretch the
  condition machine mistook the first address bit, already on SDA, for a
  STOP. Both fixed; the low-level counters (`starts`, `stops`, `bytes`)
  and the NAK events are what found them.

