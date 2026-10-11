/*
 * Tramuntana - seq: a small program the host uploads and the board runs
 * on its own, so a sequence like "reset low, 10 ms, reset high, wait for
 * the IRQ line, read the sensor" happens with microsecond timing instead
 * of a USB round trip (and a host's scheduling) between every step.
 *
 * Program: a byte string of instructions, opcode first, little-endian
 * arguments. Setting SEQ_CONTINUE (0x80) on an opcode records a failure
 * of that step instead of stopping there.
 *
 *   op    name             arguments                                  record data
 *   0x01  DELAY_US         us u32 (busy wait below 2 ms)              -
 *   0x02  POWER            on u8                                      -
 *   0x03  PULLUPS          on u8                                      -
 *   0x04  DIO_DIR          mask u8, out_mask u8                       -
 *   0x05  DIO_WRITE        mask u8, value u8                          -
 *   0x06  DIO_READ         -                                          levels u8
 *   0x07  WAIT_DIO         mask u8, value u8, timeout_us u32          waited_us u32
 *   0x08  I2C_WRITE        addr u8, n u8, data[n]                     acked u8
 *   0x09  I2C_READ         addr u8, n u8                              data[n]
 *   0x0A  I2C_WRITE_READ   addr u8, wn u8, w[wn], rn u8               data[rn]
 *   0x0B  SPI_TXN          wn u8, w[wn], rn u8                        data[rn]
 *   0x0C  UART_SEND        n u8, data[n] (bridge must be active)      -
 *   0x0D  AWG_DC           mv u16                                     -
 *   0x0E  ADC_READ         channel u8                                 mv u16
 *   0x0F  MARK             id u8                                      -
 *   0x10  LOOP             count u16 (one level, up to ENDLOOP)       -
 *   0x11  ENDLOOP          -                                          -
 *   0x12  LOGIC_ARM        as CMD_LOGIC_ARM (16 bytes)                rate u32
 *   0x13  SCOPE_ARM        as CMD_SCOPE_ARM (12 bytes)                rate u32
 *   0x14  WATCH            mask u8 (as CMD_DIO_WATCH)                 -
 *   0x15  DELAY_MS         ms u32 (sleeps)                            -
 *
 * Every executed step appends a record to the result buffer:
 *
 *   pc u16 | op u8 | status u8 (proto_status) | t_us u32 | len u8 | data[len]
 *
 * t_us is the board clock when the step finished (the same clock as the
 * EVT_* timestamps), so the host can line results up with the timeline.
 * LOOP/ENDLOOP don't record; steps inside a loop record once per pass.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define SEQ_MAX_CODE    2048
#define SEQ_MAX_RESULT  4096
#define SEQ_CONTINUE    0x80

enum seq_op {
  SEQ_DELAY_US = 0x01,
  SEQ_POWER = 0x02,
  SEQ_PULLUPS = 0x03,
  SEQ_DIO_DIR = 0x04,
  SEQ_DIO_WRITE = 0x05,
  SEQ_DIO_READ = 0x06,
  SEQ_WAIT_DIO = 0x07,
  SEQ_I2C_WRITE = 0x08,
  SEQ_I2C_READ = 0x09,
  SEQ_I2C_WRITE_READ = 0x0A,
  SEQ_SPI_TXN = 0x0B,
  SEQ_UART_SEND = 0x0C,
  SEQ_AWG_DC = 0x0D,
  SEQ_ADC_READ = 0x0E,
  SEQ_MARK = 0x0F,
  SEQ_LOOP = 0x10,
  SEQ_ENDLOOP = 0x11,
  SEQ_LOGIC_ARM = 0x12,
  SEQ_SCOPE_ARM = 0x13,
  SEQ_WATCH = 0x14,
  SEQ_DELAY_MS = 0x15,
};

enum seq_state {
  SEQ_IDLE = 0,
  SEQ_RUNNING = 1,
  SEQ_DONE = 2,      /* every step ran (failures of SEQ_CONTINUE steps are in the records) */
  SEQ_FAILED = 3,    /* stopped at 'pc' with 'error' */
  SEQ_ABORTED = 4,
  SEQ_TIMEOUT = 5,
  SEQ_INVALID = 6,   /* malformed program: nothing ran past 'pc' */
};

struct seq_status {
  enum seq_state state;
  uint8_t error;       /* proto_status of the failing step */
  uint16_t pc;
  uint16_t result_len;
};

void seq_init(void);
int seq_load(uint16_t offset, const uint8_t *code, size_t n);
int seq_run(uint16_t len, uint32_t timeout_ms);
int seq_abort(void);
void seq_get_status(struct seq_status *out);
int seq_result(uint16_t offset, uint16_t n, uint8_t *out);
