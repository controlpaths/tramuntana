/*
 * Tramuntana - shell: human-facing commands on the first CDC-ACM port.
 * Every command is a thin wrapper over the same instrument API the
 * binary protocol uses (src/proto.c), so both fronts behave the same.
 *
 *   info / pins / boot / note <text> / pinout [seconds|hold|off]
 *   led <colour|auto>        power <on|off>       pullups <on|off>
 *   dio dir <mask> <out> | dio write <mask> <val> | dio read
 *   adc <0|1>
 *   spi conf <hz> [mode] [lsb] | sel | unsel | write <hex..> | read <n>
 *       | xfer <hex..> | txn <n> <hex..> | sniff <on|off> [mode]
 *   i2c conf <hz> [pu] | scan | write <addr> <hex..> | read <addr> <n>
 *       | wrrd <addr> <n> <hex..> | start <addr_rw> | stop
 *       | raw write <hex..> | raw read <n> | sniff <on|off> [pu] | reset
 *   scope <n> [ch] [clkdiv] [trig] [level] [pre] | scope dump [off] [cnt]
 *   awg <sine|tri|saw|square|stairs|arb> <hz> [amp_v] [off_v] [param]
 *       | dc <v> | step <from> <to> <ms> | off | status
 *   pwm set <dio> <hz> <duty%> [inv] | phase <dio> <deg> | sync [mask]
 *       | stop <dio|all> | status
 *   logic <n> [clkdiv] [trig] [pin] [mask] [value] [pre]
 *       | logic dump [off] [cnt]
 *   uart <auto|on|off> [baud] | uart status | uart ctrl <off|direct|esp> [reset_dio] [boot_dio]
 *   time | watch <dio_mask|off>
 *   emu i2c <addr> [scl] [sda] [pu] | emu spi <mode> [cs sck mosi miso] [amask rmask rval status]
 *       | emu off | emu set <reg> <hex..> | emu regs [reg] [n] | emu status
 *   seq status | seq abort
 *
 * Bytes are hex ("9f 00 ff"), addresses 7-bit hex, volts decimal ("1.65").
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <pico/bootrom.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "board.h"
#include "display.h"
#include "proto.h"
#include "spi.h"
#include "i2c.h"
#include "scope.h"
#include "awg.h"
#include "pwm.h"
#include "logic.h"
#include "bridge.h"
#include "arena.h"
#include "watch.h"
#include "emu.h"
#include "seq.h"
#include "version.h"

/* --- helpers --------------------------------------------------------------- */

static int parse_hex_bytes(const struct shell *sh, char **argv, size_t n, uint8_t *out, size_t max)
{
  size_t i;

  if (n == 0 || n > max) {
    shell_error(sh, "1..%u bytes, please", (unsigned)max);
    return -EINVAL;
  }
  for (i = 0; i < n; i++) {
    char *end;
    unsigned long v = strtoul(argv[i], &end, 16);

    if (*end != '\0' || v > 0xFF) {
      shell_error(sh, "'%s' is not a hex byte", argv[i]);
      return -EINVAL;
    }
    out[i] = (uint8_t)v;
  }
  return 0;
}

/* "1.65" -> 1650 */
static int parse_mv(const char *s, int32_t *mv)
{
  char *end;
  double v = strtod(s, &end);

  if (*end != '\0' || end == s || v < 0.0 || v > 3.3) {
    return -EINVAL;
  }
  *mv = (int32_t)(v * 1000.0 + 0.5);
  return 0;
}

static bool parse_onoff(const char *s, bool *on)
{
  if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0) {
    *on = true;
    return true;
  }
  if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0) {
    *on = false;
    return true;
  }
  return false;
}

static const char *err_str(int err)
{
  switch (err) {
  case -EBUSY:
    return "busy (pins or instrument in use)";
  case -EIO:
    return "bus error / NAK";
  case -ETIMEDOUT:
    return "timeout";
  case -EPERM:
    return "not allowed in this state";
  case -EINVAL:
    return "invalid argument";
  default:
    return "failed";
  }
}

/* --- system --------------------------------------------------------------- */

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t id[8];

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  board_chip_id(id);
  shell_print(sh, "Tramuntana Lite r1a  fw %s  proto v%d", FW_VERSION, PROTO_VERSION);
  shell_print(sh, "sysclk %u Hz  chip %02x%02x%02x%02x%02x%02x%02x%02x", board_sysclk_hz(), id[0],
              id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
  shell_print(sh, "ports: ACM0 shell, ACM1 binary API, ACM2 UART bridge (DIO0 RX / DIO1 TX)");
  shell_print(sh, "power %s, pull-ups %s, arena ring: %s", board_power_get() ? "on" : "off",
              board_pullups_get() ? "on" : "off",
              arena_ring_holder() ? arena_ring_holder() : "free");
  return 0;
}

static int cmd_pins(const struct shell *sh, size_t argc, char **argv)
{
  int i;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  for (i = 0; i < DIO_COUNT; i++) {
    shell_print(sh, "DIO%d (GPIO%d): %s%s", i, DIO_GPIO(i), pin_owner_names[board_pin_owner(i)],
                board_pin_exclusive(i) ? " (exclusive)" : "");
  }
  shell_print(sh, "levels: 0x%02x", board_dio_read());
  return 0;
}

static int cmd_boot(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  shell_print(sh, "boot");
  k_sleep(K_MSEC(50));
  reset_usb_boot(0, 0);
  return 0;
}

static int cmd_note(const struct shell *sh, size_t argc, char **argv)
{
  char text[UI_LINE_MAX + 1] = "";
  size_t i;

  ARG_UNUSED(sh);
  for (i = 1; i < argc; i++) {
    strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
    if (i + 1 < argc) {
      strncat(text, " ", sizeof(text) - strlen(text) - 1);
    }
  }
  ui_set(UI_ROW_NOTE, "%s", text);
  return 0;
}

static int cmd_pinout(const struct shell *sh, size_t argc, char **argv)
{
  uint16_t seconds = 60;

  if (argc > 1) {
    if (strcmp(argv[1], "hold") == 0) {
      seconds = UI_PINOUT_HOLD;
    }
    else if (strcmp(argv[1], "off") == 0) {
      seconds = 0;
    }
    else {
      long s = strtol(argv[1], NULL, 0);

      if (s < 0 || s >= UI_PINOUT_HOLD) {
        shell_error(sh, "seconds: 0..%d, 'hold' or 'off'", UI_PINOUT_HOLD - 1);
        return -EINVAL;
      }
      seconds = (uint16_t)s;
    }
  }
  ui_pinout(seconds);
  shell_print(sh, "J1  1 3V3     2 GND     | 3 AIN0  4 GND  | 5 AIN1  6 GND  | 7 AOUT0 8 VREF");
  shell_print(sh, "    9 UART RX 10 UART TX | 11 CS  12 MISO | 13 MOSI 14 SCK | 15 SCL 16 SDA");
  return 0;
}

static int cmd_led(const struct shell *sh, size_t argc, char **argv)
{
  static const struct {
    const char *name;
    enum led_color c;
  } map[] = {
    { "off", LED_OFF }, { "red", LED_RED }, { "green", LED_GREEN }, { "blue", LED_BLUE },
    { "yellow", LED_YELLOW }, { "cyan", LED_CYAN }, { "magenta", LED_MAGENTA },
    { "white", LED_WHITE },
  };
  size_t i;

  ARG_UNUSED(argc);
  if (strcmp(argv[1], "auto") == 0) {
    led_idle();
    return 0;
  }
  for (i = 0; i < ARRAY_SIZE(map); i++) {
    if (strcmp(argv[1], map[i].name) == 0) {
      led_set(map[i].c);
      return 0;
    }
  }
  shell_error(sh, "colour: off red green blue yellow cyan magenta white auto");
  return -EINVAL;
}

static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
  bool on;

  if (argc > 1 && parse_onoff(argv[1], &on)) {
    board_power_set(on);
  }
  shell_print(sh, "target 3V3: %s", board_power_get() ? "on" : "off");
  return 0;
}

static int cmd_pullups(const struct shell *sh, size_t argc, char **argv)
{
  bool on;

  if (argc > 1 && parse_onoff(argv[1], &on)) {
    board_pullups_set(on);
  }
  shell_print(sh, "I2C pull-ups: %s", board_pullups_get() ? "on" : "off");
  return 0;
}

static int cmd_dio_dir(const struct shell *sh, size_t argc, char **argv)
{
  int ret;

  ARG_UNUSED(argc);
  ret = board_dio_dir((uint8_t)strtoul(argv[1], NULL, 0), (uint8_t)strtoul(argv[2], NULL, 0));
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  }
  return ret;
}

static int cmd_dio_write(const struct shell *sh, size_t argc, char **argv)
{
  int ret;

  ARG_UNUSED(argc);
  ret = board_dio_write((uint8_t)strtoul(argv[1], NULL, 0), (uint8_t)strtoul(argv[2], NULL, 0));
  if (ret != 0) {
    shell_error(sh, "%s (set the direction first: dio dir <mask> <out>)", err_str(ret));
  }
  return ret;
}

static int cmd_dio_read(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t v = board_dio_read();
  int i;
  char bits[9];

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  for (i = 0; i < 8; i++) {
    bits[i] = (v & BIT(7 - i)) ? '1' : '0';
  }
  bits[8] = '\0';
  shell_print(sh, "DIO7..0 = %s (0x%02x)", bits, v);
  return 0;
}

static int cmd_adc(const struct shell *sh, size_t argc, char **argv)
{
  int mv;

  ARG_UNUSED(argc);
  mv = board_adc_read_mv((int)strtol(argv[1], NULL, 0));
  if (mv < 0) {
    shell_error(sh, "%s", err_str(mv));
    return mv;
  }
  shell_print(sh, "AIN%s = %d.%03d V", argv[1], mv / 1000, mv % 1000);
  return 0;
}

/* --- spi --------------------------------------------------------------------- */

static int cmd_spi_conf(const struct shell *sh, size_t argc, char **argv)
{
  uint32_t actual = 0;
  int ret = spi_drv_conf((uint32_t)strtoul(argv[1], NULL, 0),
                         argc > 2 ? (uint8_t)strtoul(argv[2], NULL, 0) : 0,
                         argc > 3 && strcmp(argv[3], "lsb") == 0, &actual);

  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  shell_print(sh, "spi: %u Hz, mode %u, %s first", actual, spi_drv_mode(),
              spi_drv_lsb_first() ? "LSB" : "MSB");
  return 0;
}

static int cmd_spi_sel(const struct shell *sh, size_t argc, char **argv)
{
  int ret = spi_drv_sel();

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  }
  return ret;
}

static int cmd_spi_unsel(const struct shell *sh, size_t argc, char **argv)
{
  int ret = spi_drv_unsel();

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  }
  return ret;
}

static uint8_t sh_tx[256], sh_rx[SPI_MAX_XFER];

static int cmd_spi_write(const struct shell *sh, size_t argc, char **argv)
{
  int ret = parse_hex_bytes(sh, argv + 1, argc - 1, sh_tx, sizeof(sh_tx));

  if (ret == 0) {
    ret = spi_drv_xfer(sh_tx, NULL, argc - 1, 0);
  }
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  }
  return ret;
}

static int cmd_spi_read(const struct shell *sh, size_t argc, char **argv)
{
  unsigned long n = strtoul(argv[1], NULL, 0);
  int ret;

  ARG_UNUSED(argc);
  if (n == 0 || n > sizeof(sh_rx)) {
    shell_error(sh, "1..%u bytes", (unsigned)sizeof(sh_rx));
    return -EINVAL;
  }
  ret = spi_drv_xfer(NULL, sh_rx, n, 0);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  shell_hexdump(sh, sh_rx, n);
  return 0;
}

static int cmd_spi_xfer(const struct shell *sh, size_t argc, char **argv)
{
  int ret = parse_hex_bytes(sh, argv + 1, argc - 1, sh_tx, sizeof(sh_tx));

  if (ret == 0) {
    ret = spi_drv_xfer(sh_tx, sh_rx, argc - 1, 0);
  }
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  shell_hexdump(sh, sh_rx, argc - 1);
  return 0;
}

static int cmd_spi_txn(const struct shell *sh, size_t argc, char **argv)
{
  unsigned long n = strtoul(argv[1], NULL, 0);
  int ret;

  if (n > sizeof(sh_rx)) {
    shell_error(sh, "read up to %u bytes", (unsigned)sizeof(sh_rx));
    return -EINVAL;
  }
  ret = parse_hex_bytes(sh, argv + 2, argc - 2, sh_tx, sizeof(sh_tx));
  if (ret == 0) {
    ret = spi_drv_txn(sh_tx, argc - 2, sh_rx, n);
  }
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  if (n > 0) {
    shell_hexdump(sh, sh_rx, n);
  }
  return 0;
}

/* Event echo for the sniffers. */
static const struct shell *tap_shell;

/* One shell_print() per message: printing from the sniffer thread while
 * the shell's line editor owns the prompt interleaves badly across
 * multiple calls, so each message is formatted into one line first. */
#define TAP_SHOW_BYTES 24

static size_t tap_hex(char *out, size_t cap, const uint8_t *d, size_t n, size_t total)
{
  size_t i, pos = 0;

  for (i = 0; i < n && pos + 4 < cap; i++) {
    pos += snprintf(out + pos, cap - pos, "%02x ", d[i]);
  }
  if (total > n && pos + 4 < cap) {
    pos += snprintf(out + pos, cap - pos, "..");
  }
  return pos;
}

static void shell_event_tap(uint8_t evt, const uint8_t *p, size_t len)
{
  const struct shell *sh = tap_shell;
  char line[256];
  size_t pos = 0;

  if (sh == NULL) {
    return;
  }
  if (evt == EVT_SPI_MSG && len >= 3) {
    size_t k = (len - 3) / 2;
    size_t show = MIN(k, (size_t)TAP_SHOW_BYTES);
    uint16_t total = rd16(p + 1);

    pos += snprintf(line + pos, sizeof(line) - pos, "spi %uB%s  MOSI ", total,
                    (p[0] & 1) ? " OVERRUN" : "");
    pos += tap_hex(line + pos, sizeof(line) - pos, p + 3, show, total);
    pos += snprintf(line + pos, sizeof(line) - pos, " MISO ");
    pos += tap_hex(line + pos, sizeof(line) - pos, p + 3 + k, show, total);
    shell_print(sh, "%s", line);
  } else if (evt == EVT_I2C_MSG && len >= 1) {
    size_t i;

    pos += snprintf(line + pos, sizeof(line) - pos, "i2c ");
    for (i = 1; i + 1 < len && pos < sizeof(line) - 8; i += 2) {
      switch (p[i]) {
      case 1:
        pos += snprintf(line + pos, sizeof(line) - pos, "S %02X%c", p[i + 1] >> 1,
                        (p[i + 1] & 1) ? 'R' : 'W');
        break;
      case 2:
        pos += snprintf(line + pos, sizeof(line) - pos, "%s ", p[i + 1] ? "+" : "-");
        break;
      case 3:
        pos += snprintf(line + pos, sizeof(line) - pos, "%02X", p[i + 1]);
        break;
      case 4:
        pos += snprintf(line + pos, sizeof(line) - pos, "P");
        break;
      default:
        break;
      }
    }
    if (p[0] & 1) {
      pos += snprintf(line + pos, sizeof(line) - pos, " OVERRUN");
    } else if (p[0] & 2) {
      pos += snprintf(line + pos, sizeof(line) - pos, " (truncated)");
    }
    shell_print(sh, "%s", line);
  }
}

static int cmd_spi_sniff(const struct shell *sh, size_t argc, char **argv)
{
  bool on;
  int ret;

  if (!parse_onoff(argv[1], &on)) {
    shell_error(sh, "spi sniff <on|off> [mode]");
    return -EINVAL;
  }
  if (on) {
    tap_shell = sh;
    proto_set_event_tap(shell_event_tap);
    ret = spi_sniff_start(argc > 2 ? (uint8_t)strtoul(argv[2], NULL, 0) : spi_drv_mode());
    if (ret == 0) {
      shell_print(sh, "sniffing SPI (DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK); 'spi sniff off' to stop");
    }
  } else {
    ret = spi_sniff_stop();
    if (!i2c_sniff_active()) {
      proto_set_event_tap(NULL);
    }
    shell_print(sh, "stopped after %u message(s)", spi_sniff_count());
  }
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  }
  return ret;
}

/* --- i2c --------------------------------------------------------------------- */

static int parse_addr(const struct shell *sh, const char *s, uint8_t *addr)
{
  unsigned long v = strtoul(s, NULL, 16);

  if (v > 0x7F) {
    shell_error(sh, "address: 7-bit hex (00..7f)");
    return -EINVAL;
  }
  *addr = (uint8_t)v;
  return 0;
}

static int cmd_i2c_conf(const struct shell *sh, size_t argc, char **argv)
{
  uint32_t actual = 0;
  bool pu = board_pullups_get();
  int ret;

  if (argc > 2) {
    parse_onoff(argv[2], &pu);
  }
  ret = i2c_drv_conf((uint32_t)strtoul(argv[1], NULL, 0), pu, &actual);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  shell_print(sh, "i2c: %u Hz, pull-ups %s", actual, board_pullups_get() ? "on" : "off");
  return 0;
}

static int cmd_i2c_scan(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t bitmap[16];
  int ret, row, col, found = 0;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  ret = i2c_drv_scan(bitmap);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  shell_print(sh, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");
  for (row = 0; row < 8; row++) {
    char line[64];
    int pos = snprintf(line, sizeof(line), "%02x:", row * 16);

    for (col = 0; col < 16; col++) {
      int addr = row * 16 + col;
      bool hit = (bitmap[addr / 8] >> (addr % 8)) & 1;

      pos += snprintf(line + pos, sizeof(line) - pos, hit ? " %02x" : " --", addr);
      found += hit;
    }
    shell_print(sh, "%s", line);
  }
  shell_print(sh, "%d device(s)", found);
  return 0;
}

static int cmd_i2c_write(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t addr;
  size_t acked = 0;
  int ret = parse_addr(sh, argv[1], &addr);

  if (ret == 0) {
    ret = parse_hex_bytes(sh, argv + 2, argc - 2, sh_tx, sizeof(sh_tx));
  }
  if (ret == 0) {
    ret = i2c_drv_write(addr, sh_tx, argc - 2, &acked);
  }
  if (ret != 0) {
    shell_error(sh, "%s (%u byte(s) acked)", err_str(ret), (unsigned)acked);
    return ret;
  }
  shell_print(sh, "ok, %u byte(s) acked", (unsigned)acked);
  return 0;
}

static int cmd_i2c_read(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t addr;
  unsigned long n = strtoul(argv[2], NULL, 0);
  int ret = parse_addr(sh, argv[1], &addr);

  ARG_UNUSED(argc);
  if (ret == 0 && (n == 0 || n > sizeof(sh_rx))) {
    shell_error(sh, "1..%u bytes", (unsigned)sizeof(sh_rx));
    ret = -EINVAL;
  }
  if (ret == 0) {
    ret = i2c_drv_read(addr, sh_rx, n);
  }
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  shell_hexdump(sh, sh_rx, n);
  return 0;
}

static int cmd_i2c_wrrd(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t addr;
  unsigned long n = strtoul(argv[2], NULL, 0);
  int ret = parse_addr(sh, argv[1], &addr);

  if (ret == 0 && (n == 0 || n > sizeof(sh_rx))) {
    shell_error(sh, "1..%u bytes", (unsigned)sizeof(sh_rx));
    ret = -EINVAL;
  }
  if (ret == 0) {
    ret = parse_hex_bytes(sh, argv + 3, argc - 3, sh_tx, sizeof(sh_tx));
  }
  if (ret == 0) {
    ret = i2c_drv_write_read(addr, sh_tx, argc - 3, sh_rx, n);
  }
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  shell_hexdump(sh, sh_rx, n);
  return 0;
}

static int cmd_i2c_start(const struct shell *sh, size_t argc, char **argv)
{
  bool ack = false;
  int ret;

  ARG_UNUSED(argc);
  ret = i2c_drv_start((uint8_t)strtoul(argv[1], NULL, 16), &ack);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  shell_print(sh, "start: %s", ack ? "ACK" : "NAK");
  return 0;
}

static int cmd_i2c_stop(const struct shell *sh, size_t argc, char **argv)
{
  int ret = i2c_drv_stop();

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  }
  return ret;
}

static int cmd_i2c_raw_write(const struct shell *sh, size_t argc, char **argv)
{
  size_t acked = 0;
  int ret = parse_hex_bytes(sh, argv + 1, argc - 1, sh_tx, sizeof(sh_tx));

  if (ret == 0) {
    ret = i2c_drv_raw_write(sh_tx, argc - 1, &acked);
  }
  if (ret != 0) {
    shell_error(sh, "%s (%u acked)", err_str(ret), (unsigned)acked);
    return ret;
  }
  shell_print(sh, "ok, %u byte(s) acked", (unsigned)acked);
  return 0;
}

static int cmd_i2c_raw_read(const struct shell *sh, size_t argc, char **argv)
{
  unsigned long n = strtoul(argv[1], NULL, 0);
  int ret;

  ARG_UNUSED(argc);
  if (n == 0 || n > sizeof(sh_rx)) {
    shell_error(sh, "1..%u bytes", (unsigned)sizeof(sh_rx));
    return -EINVAL;
  }
  ret = i2c_drv_raw_read(sh_rx, n, true);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  shell_hexdump(sh, sh_rx, n);
  return 0;
}

static int cmd_i2c_sniff(const struct shell *sh, size_t argc, char **argv)
{
  bool on, pu = false;
  int ret;

  if (!parse_onoff(argv[1], &on)) {
    shell_error(sh, "i2c sniff <on|off> [pu]");
    return -EINVAL;
  }
  if (on) {
    if (argc > 2) {
      parse_onoff(argv[2], &pu);
      pu = pu || strcmp(argv[2], "pu") == 0;
    }
    tap_shell = sh;
    proto_set_event_tap(shell_event_tap);
    ret = i2c_sniff_start(pu);
    if (ret == 0) {
      shell_print(sh, "sniffing I2C (DIO6 SCL, DIO7 SDA); 'i2c sniff off' to stop");
    }
  } else {
    ret = i2c_sniff_stop();
    if (!spi_sniff_active()) {
      proto_set_event_tap(NULL);
    }
    shell_print(sh, "stopped after %u message(s)", i2c_sniff_count());
  }
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  }
  return ret;
}

static int cmd_i2c_reset(const struct shell *sh, size_t argc, char **argv)
{
  int ret = i2c_drv_reset();

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  }
  return ret;
}

/* --- scope --------------------------------------------------------------------- */

static int cmd_scope(const struct shell *sh, size_t argc, char **argv)
{
  struct scope_params p = { .channels = 1, .clkdiv = SCOPE_MIN_CLKDIV, .trig_mode = 0 };
  uint32_t rate = 0;
  const uint16_t *s;
  uint32_t i, n, sum = 0, vmin = 4095, vmax = 0;
  int ret;

  p.n = (uint16_t)strtoul(argv[1], NULL, 0);
  if (argc > 2) {
    p.channels = (uint8_t)strtoul(argv[2], NULL, 0);
  }
  if (argc > 3) {
    p.clkdiv = (uint16_t)strtoul(argv[3], NULL, 0);
  }
  if (argc > 4) {
    p.trig_mode = (uint8_t)strtoul(argv[4], NULL, 0);
  }
  if (argc > 5) {
    p.trig_level = (uint16_t)strtoul(argv[5], NULL, 0);
  }
  if (argc > 6) {
    p.pre = (uint16_t)strtoul(argv[6], NULL, 0);
  }
  ret = scope_capture_blocking(&p, &rate);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  s = scope_samples();
  n = p.n * p.channels;
  for (i = 0; i < n; i += p.channels) {
    sum += s[i];
    vmin = MIN(vmin, s[i]);
    vmax = MAX(vmax, s[i]);
  }
  shell_print(sh, "scope: %u sample(s) x %u ch @ %u S/s (per channel %u), trigger at %u", p.n,
              p.channels, rate, rate / p.channels, p.pre);
  shell_print(sh, "AIN0: min %u.%03u V  max %u.%03u V  mean %u.%03u V",
              vmin * 3300 / 4095 / 1000, vmin * 3300 / 4095 % 1000, vmax * 3300 / 4095 / 1000,
              vmax * 3300 / 4095 % 1000, (sum / p.n) * 3300 / 4095 / 1000,
              (sum / p.n) * 3300 / 4095 % 1000);
  shell_print(sh, "'scope dump [offset] [count]' prints raw samples");
  return 0;
}

static int cmd_scope_dump(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t ch;
  uint16_t n, ti;
  uint32_t rate, offset = 0, count = 64, i, total;
  const uint16_t *s = scope_samples();

  if (scope_state() != CAP_DONE) {
    shell_error(sh, "no capture");
    return -EPERM;
  }
  scope_status(&ch, &n, &rate, &ti);
  total = (uint32_t)n * ch;
  if (argc > 1) {
    offset = strtoul(argv[1], NULL, 0);
  }
  if (argc > 2) {
    count = strtoul(argv[2], NULL, 0);
  }
  for (i = offset; i < total && i < offset + count; i++) {
    shell_print(sh, "%u,%u", i, s[i]);
  }
  return 0;
}

/* --- awg ------------------------------------------------------------------------- */

static int cmd_awg_shape(const struct shell *sh, size_t argc, char **argv, enum awg_shape shape)
{
  int32_t amp = 1000, off = 1650;
  uint16_t param = 0;
  struct awg_status st;
  int ret;

  if (argc > 2 && parse_mv(argv[2], &amp) != 0) {
    shell_error(sh, "amplitude: volts 0..3.3");
    return -EINVAL;
  }
  if (argc > 3 && parse_mv(argv[3], &off) != 0) {
    shell_error(sh, "offset: volts 0..3.3");
    return -EINVAL;
  }
  if (argc > 4) {
    param = (uint16_t)strtoul(argv[4], NULL, 0);
  }
  ret = awg_set(shape, (uint32_t)strtoul(argv[1], NULL, 0), (uint16_t)amp, (uint16_t)off, param,
                &st);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  shell_print(sh, "awg: %s %u.%03u Hz (table %u, top %u), amp %d mV, offset %d mV",
              awg_shape_name(shape), st.freq_mhz / 1000, st.freq_mhz % 1000, st.n, st.top, amp,
              off);
  return 0;
}

static int cmd_awg_sine(const struct shell *sh, size_t argc, char **argv)
{
  return cmd_awg_shape(sh, argc, argv, AWG_SINE);
}

static int cmd_awg_tri(const struct shell *sh, size_t argc, char **argv)
{
  return cmd_awg_shape(sh, argc, argv, AWG_TRI);
}

static int cmd_awg_saw(const struct shell *sh, size_t argc, char **argv)
{
  return cmd_awg_shape(sh, argc, argv, AWG_SAW);
}

static int cmd_awg_square(const struct shell *sh, size_t argc, char **argv)
{
  return cmd_awg_shape(sh, argc, argv, AWG_SQUARE);
}

static int cmd_awg_stairs(const struct shell *sh, size_t argc, char **argv)
{
  return cmd_awg_shape(sh, argc, argv, AWG_STAIRS);
}

static int cmd_awg_arb(const struct shell *sh, size_t argc, char **argv)
{
  if (awg_arb_len() == 0) {
    shell_error(sh, "no arbitrary table loaded (upload one over the API)");
    return -EINVAL;
  }
  return cmd_awg_shape(sh, argc, argv, AWG_ARB);
}

static int cmd_awg_dc(const struct shell *sh, size_t argc, char **argv)
{
  int32_t mv;

  ARG_UNUSED(argc);
  if (parse_mv(argv[1], &mv) != 0) {
    shell_error(sh, "volts 0..3.3");
    return -EINVAL;
  }
  return awg_dc((uint16_t)mv);
}

static int cmd_awg_step(const struct shell *sh, size_t argc, char **argv)
{
  int32_t from, to;

  ARG_UNUSED(argc);
  if (parse_mv(argv[1], &from) != 0 || parse_mv(argv[2], &to) != 0) {
    shell_error(sh, "volts 0..3.3");
    return -EINVAL;
  }
  return awg_step((uint16_t)from, (uint16_t)to, (uint16_t)strtoul(argv[3], NULL, 0));
}

static int cmd_awg_off(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(sh);
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  awg_off();
  return 0;
}

static int cmd_awg_status(const struct shell *sh, size_t argc, char **argv)
{
  struct awg_status st;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  awg_get_status(&st);
  shell_print(sh, "awg: %s%s %u.%03u Hz, amp %u mV, offset %u mV, table %u, top %u, arb %u samples",
              awg_shape_name(st.shape), st.running ? "" : " (stopped)", st.freq_mhz / 1000,
              st.freq_mhz % 1000, st.amp_mv, st.off_mv, st.n, st.top, awg_arb_len());
  return 0;
}

/* --- pwm ------------------------------------------------------------------------- */

static int parse_dio(const struct shell *sh, const char *s, int *dio)
{
  char *end;
  long v = strtol(s, &end, 0);

  if (*end != '\0' || end == s || v < 0 || v >= DIO_COUNT) {
    shell_error(sh, "DIO 0..7, please");
    return -EINVAL;
  }
  *dio = (int)v;
  return 0;
}

static void pwm_print(const struct shell *sh, int dio, const struct pwm_out_status *st)
{
  shell_print(sh, "DIO%d: %u.%03u Hz, duty %u.%02u %% (level %u/%u, div %u), phase %u deg%s%s", dio,
              st->freq_mhz / 1000, st->freq_mhz % 1000, st->duty / 100, st->duty % 100, st->level,
              st->top + 1, st->div, st->phase_deg, st->invert ? ", inverted" : "",
              st->lost ? " - pin taken by another instrument" : "");
}

static int cmd_pwm_set(const struct shell *sh, size_t argc, char **argv)
{
  struct pwm_out_status st;
  bool invert = false;
  uint32_t freq;
  double duty;
  char *end;
  int dio, ret;

  if (parse_dio(sh, argv[1], &dio) != 0) {
    return -EINVAL;
  }
  duty = strtod(argv[3], &end);
  if (*end != '\0' || end == argv[3] || duty < 0.0 || duty > 100.0) {
    shell_error(sh, "duty: percent 0..100");
    return -EINVAL;
  }
  if (argc > 4) {
    if (strcmp(argv[4], "inv") != 0) {
      shell_error(sh, "'%s': only 'inv' is accepted", argv[4]);
      return -EINVAL;
    }
    invert = true;
  }
  freq = (uint32_t)strtoul(argv[2], NULL, 0);
  ret = pwm_out_set(dio, freq, (uint16_t)(duty * 100.0 + 0.5), invert, &st);
  if (ret == -EBUSY) {
    pwm_out_get_status(PWM_OUT_PARTNER(dio), &st);
    if (st.active && !st.lost && st.freq_hz != freq) {
      shell_error(sh, "DIO%d shares PWM slice %d with DIO%d, which runs at %u Hz (stop it or use the same frequency)",
                  dio, PWM_OUT_SLICE(dio), PWM_OUT_PARTNER(dio), st.freq_hz);
      return ret;
    }
  }
  if (ret == -EINVAL) {
    shell_error(sh, "frequency: %u..%u Hz", PWM_OUT_MIN_FREQ_HZ, board_sysclk_hz() / 2);
    return ret;
  }
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  pwm_print(sh, dio, &st);
  return 0;
}

static int cmd_pwm_phase(const struct shell *sh, size_t argc, char **argv)
{
  unsigned long deg;
  int dio, ret;

  ARG_UNUSED(argc);
  if (parse_dio(sh, argv[1], &dio) != 0) {
    return -EINVAL;
  }
  deg = strtoul(argv[2], NULL, 0);
  ret = pwm_out_phase(dio, (uint16_t)MIN(deg, 360UL));
  if (ret != 0) {
    shell_error(sh, "phase: degrees 0..359");
    return ret;
  }
  shell_print(sh, "slice %d (DIO%d/DIO%d): phase %lu deg, applied on 'pwm sync'", PWM_OUT_SLICE(dio),
              dio | 1, dio & ~1, deg);
  return 0;
}

static int cmd_pwm_sync(const struct shell *sh, size_t argc, char **argv)
{
  int ret = pwm_out_sync(argc > 1 ? (uint8_t)strtoul(argv[1], NULL, 0) : 0);

  if (ret != 0) {
    shell_error(sh, "no live PWM output in the mask");
  }
  return ret;
}

static int cmd_pwm_stop(const struct shell *sh, size_t argc, char **argv)
{
  int dio;

  ARG_UNUSED(argc);
  if (strcmp(argv[1], "all") == 0) {
    pwm_out_stop(DIO_MASK_ALL);
    return 0;
  }
  if (parse_dio(sh, argv[1], &dio) != 0) {
    return -EINVAL;
  }
  pwm_out_stop(BIT(dio));
  return 0;
}

static int cmd_pwm_status(const struct shell *sh, size_t argc, char **argv)
{
  struct pwm_out_status st;
  bool any = false;
  int dio;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  for (dio = DIO_COUNT - 1; dio >= 0; dio--) {
    pwm_out_get_status(dio, &st);
    if (st.active) {
      pwm_print(sh, dio, &st);
      any = true;
    }
  }
  if (!any) {
    shell_print(sh, "no PWM outputs");
  }
  return 0;
}

/* --- logic ----------------------------------------------------------------------- */

static int cmd_logic(const struct shell *sh, size_t argc, char **argv)
{
  struct logic_params p = { .clkdiv = 1 };
  uint32_t rate = 0;
  int ret;

  p.n = strtoul(argv[1], NULL, 0);
  if (argc > 2) {
    p.clkdiv = (uint16_t)strtoul(argv[2], NULL, 0);
  }
  if (argc > 3) {
    p.trig_mode = (uint8_t)strtoul(argv[3], NULL, 0);
  }
  if (argc > 4) {
    p.trig_pin = (uint8_t)strtoul(argv[4], NULL, 0);
  }
  if (argc > 5) {
    p.trig_mask = (uint8_t)strtoul(argv[5], NULL, 0);
  }
  if (argc > 6) {
    p.trig_value = (uint8_t)strtoul(argv[6], NULL, 0);
  }
  if (argc > 7) {
    p.pre = strtoul(argv[7], NULL, 0);
  }
  ret = logic_capture_blocking(&p, &rate);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
    return ret;
  }
  {
    uint32_t n, ti, i, changes = 0;
    uint8_t buf[256], prev = 0;

    logic_status(&n, &rate, &ti);
    for (i = 0; i < n; i += sizeof(buf)) {
      int got = logic_read(i, (uint16_t)MIN(n - i, sizeof(buf)), buf);
      int j;

      for (j = 0; j < got; j++) {
        if (i + j > 0 && buf[j] != prev) {
          changes++;
        }
        prev = buf[j];
      }
    }
    shell_print(sh, "logic: %u sample(s) @ %u S/s (%u us), trigger at %u, %u transition(s)", n,
                rate, (uint32_t)(((uint64_t)n * 1000000) / rate), ti, changes);
    shell_print(sh, "'logic dump [offset] [count]' prints samples (bit i = DIOi)");
  }
  return 0;
}

static int cmd_logic_dump(const struct shell *sh, size_t argc, char **argv)
{
  uint32_t n, rate, ti, offset = 0, count = 64, i;
  uint8_t buf[64];

  if (logic_state() != CAP_DONE) {
    shell_error(sh, "no capture");
    return -EPERM;
  }
  logic_status(&n, &rate, &ti);
  if (argc > 1) {
    offset = strtoul(argv[1], NULL, 0);
  }
  if (argc > 2) {
    count = strtoul(argv[2], NULL, 0);
  }
  for (i = offset; i < n && i < offset + count; i += sizeof(buf)) {
    int got = logic_read(i, (uint16_t)MIN(MIN(n - i, offset + count - i), sizeof(buf)), buf);
    int j;

    for (j = 0; j < got; j++) {
      char bits[9];
      int b;

      for (b = 0; b < 8; b++) {
        bits[b] = (buf[j] & BIT(7 - b)) ? '1' : '0';
      }
      bits[8] = '\0';
      shell_print(sh, "%u,%s", i + j, bits);
    }
  }
  return 0;
}

/* --- uart ------------------------------------------------------------------------ */

static int cmd_uart(const struct shell *sh, size_t argc, char **argv)
{
  enum bridge_mode mode;
  int ret;

  if (strcmp(argv[1], "auto") == 0) {
    mode = BRIDGE_AUTO;
  } else if (strcmp(argv[1], "on") == 0) {
    mode = BRIDGE_ON;
  } else if (strcmp(argv[1], "off") == 0) {
    mode = BRIDGE_OFF;
  } else {
    shell_error(sh, "uart <auto|on|off> [baud]");
    return -EINVAL;
  }
  ret = bridge_conf(mode, argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  }
  return ret;
}

static int cmd_uart_status(const struct shell *sh, size_t argc, char **argv)
{
  bool active;
  uint32_t baud, rx, tx;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  bridge_status(&active, &baud, &rx, &tx);
  shell_print(sh, "bridge: %s, mode %s, %u baud, %u rx / %u tx bytes (ACM2 <-> DIO0 RX, DIO1 TX)",
              active ? "active" : "idle",
              bridge_get_mode() == BRIDGE_AUTO ? "auto" : bridge_get_mode() == BRIDGE_ON ? "on" : "off",
              baud, rx, tx);
  return 0;
}

static int cmd_uart_ctrl(const struct shell *sh, size_t argc, char **argv)
{
  enum bridge_ctrl_mode mode;
  uint8_t rst = BRIDGE_CTRL_NO_PIN, boot = BRIDGE_CTRL_NO_PIN;
  int ret;

  if (strcmp(argv[1], "off") == 0) {
    mode = BRIDGE_CTRL_OFF;
  } else if (strcmp(argv[1], "direct") == 0) {
    mode = BRIDGE_CTRL_DIRECT;
  } else if (strcmp(argv[1], "esp") == 0) {
    mode = BRIDGE_CTRL_ESP;
  } else {
    shell_error(sh, "uart ctrl <off|direct|esp> [reset_dio] [boot_dio]");
    return -EINVAL;
  }
  if (argc > 2) {
    rst = (uint8_t)strtoul(argv[2], NULL, 0);
  }
  if (argc > 3) {
    boot = (uint8_t)strtoul(argv[3], NULL, 0);
  }
  ret = bridge_ctrl(mode, rst, boot);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  }
  return ret;
}

/* --- timeline helpers ------------------------------------------------------------ */

static int cmd_time(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  shell_print(sh, "%u us", board_time_us());
  return 0;
}

static int cmd_watch(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t mask = strcmp(argv[1], "off") == 0 ? 0 : (uint8_t)strtoul(argv[1], NULL, 0);
  int ret = watch_set(mask);

  ARG_UNUSED(argc);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  } else {
    shell_print(sh, "watching 0x%02x (EVT_DIO on the API port)", mask);
  }
  return ret;
}

/* --- emulated targets ------------------------------------------------------------ */

static int cmd_emu_i2c(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t addr = (uint8_t)strtoul(argv[1], NULL, 16);
  uint8_t scl = argc > 2 ? (uint8_t)strtoul(argv[2], NULL, 0) : 6;
  uint8_t sda = argc > 3 ? (uint8_t)strtoul(argv[3], NULL, 0) : 7;
  bool pu = argc > 4 && strcmp(argv[4], "pu") == 0;
  int ret = emu_i2c_start(addr, scl, sda, pu);

  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  } else {
    shell_print(sh, "I2C target 0x%02x on SCL DIO%u / SDA DIO%u", addr, scl, sda);
  }
  return ret;
}

static int cmd_emu_spi(const struct shell *sh, size_t argc, char **argv)
{
  struct emu_spi_params p = {
    .mode = (uint8_t)strtoul(argv[1], NULL, 0), .cs = 2, .sck = 5, .mosi = 4, .miso = 3,
    .addr_mask = 0x7F, .read_mask = 0x80, .read_value = 0x80, .status = 0,
  };
  int ret;

  if (argc >= 6) {
    p.cs = (uint8_t)strtoul(argv[2], NULL, 0);
    p.sck = (uint8_t)strtoul(argv[3], NULL, 0);
    p.mosi = (uint8_t)strtoul(argv[4], NULL, 0);
    p.miso = (uint8_t)strtoul(argv[5], NULL, 0);
  }
  if (argc >= 10) {
    p.addr_mask = (uint8_t)strtoul(argv[6], NULL, 16);
    p.read_mask = (uint8_t)strtoul(argv[7], NULL, 16);
    p.read_value = (uint8_t)strtoul(argv[8], NULL, 16);
    p.status = (uint8_t)strtoul(argv[9], NULL, 16);
  }
  ret = emu_spi_start(&p);
  if (ret != 0) {
    shell_error(sh, "%s", err_str(ret));
  } else {
    shell_print(sh, "SPI target mode %u, CS DIO%u SCK DIO%u MOSI DIO%u MISO DIO%u", p.mode, p.cs,
                p.sck, p.mosi, p.miso);
  }
  return ret;
}

static int cmd_emu_off(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(sh);
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  emu_stop();
  return 0;
}

static int cmd_emu_set(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t data[EMU_REGS];
  uint8_t reg = (uint8_t)strtoul(argv[1], NULL, 16);
  int ret = parse_hex_bytes(sh, argv + 2, argc - 2, data, sizeof(data));

  if (ret == 0) {
    emu_regs_set(reg, data, argc - 2);
  }
  return ret;
}

static int cmd_emu_regs(const struct shell *sh, size_t argc, char **argv)
{
  uint8_t reg = argc > 1 ? (uint8_t)strtoul(argv[1], NULL, 16) : 0;
  size_t n = argc > 2 ? MIN(strtoul(argv[2], NULL, 0), (unsigned long)EMU_REGS) : 16;
  uint8_t data[EMU_REGS];

  emu_regs_get(reg, data, n);
  shell_hexdump(sh, data, n);
  return 0;
}

static int cmd_emu_status(const struct shell *sh, size_t argc, char **argv)
{
  struct emu_stats st;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  emu_get_stats(&st);
  shell_print(sh, "%s: %u transfers, %u bytes read, %u written, %u NAKed",
              st.kind == EMU_I2C ? "i2c" : st.kind == EMU_SPI ? "spi" : "off", st.txns, st.reads,
              st.writes, st.naks);
  return 0;
}

/* --- sequencer ------------------------------------------------------------------- */

static int cmd_seq_status(const struct shell *sh, size_t argc, char **argv)
{
  static const char *const names[] = {"idle", "running", "done", "failed", "aborted", "timeout",
                                      "invalid"};
  struct seq_status st;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  seq_get_status(&st);
  shell_print(sh, "%s, pc %u, error %u, %u result bytes", names[st.state], st.pc, st.error,
              st.result_len);
  return 0;
}

static int cmd_seq_abort(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(sh);
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  return seq_abort();
}

/* --- registration ------------------------------------------------------------------- */

SHELL_CMD_REGISTER(info, NULL, "Board / firmware information", cmd_info);
SHELL_CMD_REGISTER(pins, NULL, "Who owns each DIO pin", cmd_pins);
SHELL_CMD_REGISTER(boot, NULL, "Reset into the USB (UF2) bootloader", cmd_boot);
SHELL_CMD_ARG_REGISTER(note, NULL, "<text> - show a note on the display", cmd_note, 1, 10);
SHELL_CMD_ARG_REGISTER(pinout, NULL, "[seconds|hold|off] - draw the J1 pinout on the display (default 60 s)",
                       cmd_pinout, 1, 1);
SHELL_CMD_ARG_REGISTER(led, NULL, "<off|red|green|blue|yellow|cyan|magenta|white|auto>", cmd_led, 2, 0);
SHELL_CMD_ARG_REGISTER(power, NULL, "[on|off] - target 3.3V rail", cmd_power, 1, 1);
SHELL_CMD_ARG_REGISTER(pullups, NULL, "[on|off] - I2C pull-up bias on DIO6/DIO7", cmd_pullups, 1, 1);
SHELL_CMD_ARG_REGISTER(adc, NULL, "<0|1> - read AIN0/AIN1 once", cmd_adc, 2, 0);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_dio,
  SHELL_CMD_ARG(dir, NULL, "<mask> <out_mask> - set DIO directions (bit i = DIOi)", cmd_dio_dir, 3, 0),
  SHELL_CMD_ARG(write, NULL, "<mask> <value>", cmd_dio_write, 3, 0),
  SHELL_CMD_ARG(read, NULL, "read all DIO levels", cmd_dio_read, 1, 0),
  SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(dio, &sub_dio, "DIO header as plain GPIO", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_spi,
  SHELL_CMD_ARG(conf, NULL, "<hz> [mode 0-3] [lsb]", cmd_spi_conf, 2, 2),
  SHELL_CMD_ARG(sel, NULL, "CS low", cmd_spi_sel, 1, 0),
  SHELL_CMD_ARG(unsel, NULL, "CS high", cmd_spi_unsel, 1, 0),
  SHELL_CMD_ARG(write, NULL, "<hex..> - send bytes (CS untouched)", cmd_spi_write, 2, 255),
  SHELL_CMD_ARG(read, NULL, "<n> - read n bytes (CS untouched)", cmd_spi_read, 2, 0),
  SHELL_CMD_ARG(xfer, NULL, "<hex..> - full duplex (CS untouched)", cmd_spi_xfer, 2, 255),
  SHELL_CMD_ARG(txn, NULL, "<read_n> <hex..> - sel, write, read, unsel", cmd_spi_txn, 2, 255),
  SHELL_CMD_ARG(sniff, NULL, "<on|off> [mode] - passive bus tap (MOSI+MISO)", cmd_spi_sniff, 2, 1),
  SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(spi, &sub_spi, "SPI master (DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK) + sniffer", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_i2c_raw,
  SHELL_CMD_ARG(write, NULL, "<hex..> - bytes on an open bus", cmd_i2c_raw_write, 2, 255),
  SHELL_CMD_ARG(read, NULL, "<n> - bytes from an open bus (NAK on the last)", cmd_i2c_raw_read, 2, 0),
  SHELL_SUBCMD_SET_END
);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_i2c,
  SHELL_CMD_ARG(conf, NULL, "<hz> [pu on|off]", cmd_i2c_conf, 2, 1),
  SHELL_CMD_ARG(scan, NULL, "probe 08..77", cmd_i2c_scan, 1, 0),
  SHELL_CMD_ARG(write, NULL, "<addr> <hex..>", cmd_i2c_write, 3, 255),
  SHELL_CMD_ARG(read, NULL, "<addr> <n>", cmd_i2c_read, 3, 0),
  SHELL_CMD_ARG(wrrd, NULL, "<addr> <n> <hex..> - write, repeated start, read n", cmd_i2c_wrrd, 4, 255),
  SHELL_CMD_ARG(start, NULL, "<addr_rw hex> - START + address byte", cmd_i2c_start, 2, 0),
  SHELL_CMD_ARG(stop, NULL, "STOP", cmd_i2c_stop, 1, 0),
  SHELL_CMD(raw, &sub_i2c_raw, "bytes on an already open bus", NULL),
  SHELL_CMD_ARG(sniff, NULL, "<on|off> [pu] - passive bus tap", cmd_i2c_sniff, 2, 1),
  SHELL_CMD_ARG(reset, NULL, "bus recovery (9 clocks + STOP)", cmd_i2c_reset, 1, 0),
  SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(i2c, &sub_i2c, "I2C master (DIO6 SCL, DIO7 SDA, bit-banged) + sniffer", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_scope,
  SHELL_CMD_ARG(dump, NULL, "[offset] [count] - raw samples of the last capture", cmd_scope_dump, 1, 2),
  SHELL_SUBCMD_SET_END
);
SHELL_CMD_ARG_REGISTER(scope, &sub_scope,
                       "<n> [ch 1|2] [clkdiv>=96] [trig 0|1|2] [level 0-4095] [pre] - capture AIN0(/AIN1)",
                       cmd_scope, 2, 5);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_awg,
  SHELL_CMD_ARG(sine, NULL, "<hz> [amp_v] [off_v]", cmd_awg_sine, 2, 3),
  SHELL_CMD_ARG(tri, NULL, "<hz> [amp_v] [off_v]", cmd_awg_tri, 2, 3),
  SHELL_CMD_ARG(saw, NULL, "<hz> [amp_v] [off_v]", cmd_awg_saw, 2, 3),
  SHELL_CMD_ARG(square, NULL, "<hz> [amp_v] [off_v] [duty%]", cmd_awg_square, 2, 3),
  SHELL_CMD_ARG(stairs, NULL, "<hz> [amp_v] [off_v] [steps]", cmd_awg_stairs, 2, 3),
  SHELL_CMD_ARG(arb, NULL, "<hz> [amp_v] [off_v] - play the uploaded table", cmd_awg_arb, 2, 3),
  SHELL_CMD_ARG(dc, NULL, "<v>", cmd_awg_dc, 2, 0),
  SHELL_CMD_ARG(step, NULL, "<from_v> <to_v> <delay_ms>", cmd_awg_step, 4, 0),
  SHELL_CMD_ARG(off, NULL, "stop", cmd_awg_off, 1, 0),
  SHELL_CMD_ARG(status, NULL, "", cmd_awg_status, 1, 0),
  SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(awg, &sub_awg, "Waveform generator on AOUT0", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_pwm,
  SHELL_CMD_ARG(set, NULL, "<dio> <hz> <duty%> [inv] - pairs 7/6, 5/4, 3/2, 1/0 share the frequency",
                cmd_pwm_set, 4, 1),
  SHELL_CMD_ARG(phase, NULL, "<dio> <deg> - phase delay of the pin's pair, applied on sync", cmd_pwm_phase,
                3, 0),
  SHELL_CMD_ARG(sync, NULL, "[dio_mask] - restart the pairs together (default: all)", cmd_pwm_sync, 1, 1),
  SHELL_CMD_ARG(stop, NULL, "<dio|all>", cmd_pwm_stop, 2, 0),
  SHELL_CMD_ARG(status, NULL, "", cmd_pwm_status, 1, 0),
  SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(pwm, &sub_pwm, "PWM outputs on the DIO header", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_logic,
  SHELL_CMD_ARG(dump, NULL, "[offset] [count] - samples of the last capture", cmd_logic_dump, 1, 2),
  SHELL_SUBCMD_SET_END
);
SHELL_CMD_ARG_REGISTER(logic, &sub_logic,
                       "<n> [clkdiv>=1] [trig 0 none|1 rise|2 fall|3 pattern] [pin] [mask] [value] [pre]",
                       cmd_logic, 2, 6);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_uart,
  SHELL_CMD_ARG(status, NULL, "", cmd_uart_status, 1, 0),
  SHELL_CMD_ARG(ctrl, NULL, "<off|direct|esp> [reset_dio] [boot_dio] - RTS/DTR drive reset/boot pins",
                cmd_uart_ctrl, 2, 2),
  SHELL_SUBCMD_SET_END
);
SHELL_CMD_ARG_REGISTER(uart, &sub_uart, "<auto|on|off> [baud] - USB<->UART bridge on ACM2", cmd_uart, 2, 1);

SHELL_CMD_REGISTER(time, NULL, "Board clock (us) used by every event timestamp", cmd_time);
SHELL_CMD_ARG_REGISTER(watch, NULL, "<dio_mask|off> - report edges on these DIOs (EVT_DIO)", cmd_watch, 2, 0);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_emu,
  SHELL_CMD_ARG(i2c, NULL, "<addr hex> [scl] [sda] [pu] - answer as an I2C device", cmd_emu_i2c, 2, 3),
  SHELL_CMD_ARG(spi, NULL, "<mode> [cs sck mosi miso] [amask rmask rval status] - answer as a SPI device",
                cmd_emu_spi, 2, 8),
  SHELL_CMD_ARG(off, NULL, "stop emulating", cmd_emu_off, 1, 0),
  SHELL_CMD_ARG(set, NULL, "<reg hex> <hex..> - load registers", cmd_emu_set, 3, 255),
  SHELL_CMD_ARG(regs, NULL, "[reg hex] [n] - dump registers", cmd_emu_regs, 1, 2),
  SHELL_CMD_ARG(status, NULL, "", cmd_emu_status, 1, 0),
  SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(emu, &sub_emu, "Emulated I2C/SPI target (256-byte register file)", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_seq,
  SHELL_CMD_ARG(status, NULL, "", cmd_seq_status, 1, 0),
  SHELL_CMD_ARG(abort, NULL, "", cmd_seq_abort, 1, 0),
  SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(seq, &sub_seq, "Sequencer (programs come from the API)", NULL);
