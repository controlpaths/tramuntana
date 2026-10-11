/*
 * Tramuntana - emu: the board pretends to be a device on the target's
 * bus. The target's I2C or SPI master talks to a 256-byte register file
 * that the host loads and reads back (CMD_EMU_*), so a firmware can be
 * tested against a sensor that returns whatever the test needs, or that
 * isn't there at all.
 *
 * Register model (both buses): a register pointer that auto-increments
 * on every byte, reads return regs[ptr++], writes store into regs[ptr++]
 * unless the register is marked read-only (the write is still ACKed, as
 * real devices do with read-only registers). Every completed transfer is
 * reported to the host as an EVT_EMU event.
 *
 *   I2C  one 7-bit address. First written byte = register pointer, the
 *        rest are data; a read starts at the current pointer (so the
 *        usual write-register / repeated START / read works). Any pins.
 *        The board stretches SCL for a moment after every START and
 *        every byte while the CPU decides ACK/NAK and the next data
 *        byte, so it works at any bus speed - the target's master has
 *        to allow clock stretching (practically all do).
 *   SPI  any mode and pins. The first byte of each CS-low window is the
 *        command: when (cmd & read_mask) == read_value it is a read of
 *        register (cmd & addr_mask) and the following MISO bytes are
 *        regs[reg], regs[reg+1]... (up to EMU_SPI_BURST, then the line
 *        holds); otherwise the following MOSI bytes are written from
 *        that register on. MISO during the command byte is 'status'.
 *        E.g. most sensors: read_mask 0x80, read_value 0x80, addr_mask
 *        0x7F. A table per command byte makes the response start without
 *        CPU help, but the first response bit still has to be ready
 *        half a clock after the command: keep SCK at or below about
 *        2 MHz, and CS high for >= 10 us between transactions.
 *
 * Only one emulator at a time; both use PIO1 and claim their pins
 * exclusively (owner "emu").
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define EMU_REGS        256
#define EMU_SPI_BURST   32
#define EMU_LOG_MAX     32    /* data bytes kept per EVT_EMU */

enum emu_kind {
  EMU_OFF = 0,
  EMU_I2C = 1,
  EMU_SPI = 2,
};

struct emu_spi_params {
  uint8_t mode;       /* 0..3 */
  uint8_t cs, sck, mosi, miso;   /* DIOs */
  uint8_t addr_mask;
  uint8_t read_mask;
  uint8_t read_value;
  uint8_t status;     /* MISO byte while the command byte is shifted in */
};

struct emu_stats {
  enum emu_kind kind;
  uint32_t txns;
  uint32_t reads;     /* bytes read by the target */
  uint32_t writes;    /* bytes written by the target */
  uint32_t naks;      /* I2C addresses / bytes NAKed */
  /* low-level counters, for diagnosing a bus that doesn't work */
  uint32_t starts;    /* I2C START / repeated START seen; SPI: CS rises */
  uint32_t stops;     /* I2C STOP seen */
  uint32_t bytes;     /* bytes the shifters handed to the CPU */
};

void emu_init(void);
int emu_i2c_start(uint8_t addr, uint8_t scl_dio, uint8_t sda_dio, bool pullups);
int emu_spi_start(const struct emu_spi_params *p);
void emu_stop(void);
void emu_regs_set(uint8_t offset, const uint8_t *data, size_t n);
void emu_regs_get(uint8_t offset, uint8_t *out, size_t n);
void emu_ro_set(const uint8_t mask[32]);
void emu_get_stats(struct emu_stats *out);
