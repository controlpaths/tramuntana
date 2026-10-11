/*
 * Tramuntana - proto: the binary API spoken on the second CDC-ACM port.
 * This header is the protocol's single source of truth; host/python/
 * tramuntana/proto.py and host/webapp/js/proto.js mirror it.
 *
 * Frame (both directions, little-endian):
 *
 *   0xAA 0x55 | cmd u8 | seq u8 | len u16 | payload[len] | crc u16
 *
 *   crc: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over cmd..payload.
 *   Requests carry cmd < 0x80. The reply echoes seq, sets cmd | 0x80 and
 *   starts its payload with a status byte (PROTO_OK or an error code),
 *   followed by the command's own data. Events (0xF0..0xFF) are sent by
 *   the board on its own with seq = 0; hosts must be ready for one to
 *   arrive between a request and its reply. Commands stay below 0x70 so
 *   no reply id (cmd | 0x80) can ever look like an event.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PROTO_VERSION      1
#define PROTO_SOF0         0xAA
#define PROTO_SOF1         0x55
#define PROTO_MAX_PAYLOAD  4104   /* 4096 of data + a small header */
#define PROTO_REPLY_FLAG   0x80
#define PROTO_CMD_MAX      0x6F
#define PROTO_EVT_MIN      0xF0
#define PROTO_MAGIC        "TRMT"

/* Status codes (first byte of every reply). */
enum proto_status {
  PROTO_OK = 0,
  PROTO_EINVAL = 1,     /* bad argument / payload length */
  PROTO_EBUSY = 2,      /* resource (pins, DMA, instrument) in use */
  PROTO_ETIMEDOUT = 3,
  PROTO_EIO = 4,        /* bus error: NAK, overrun... */
  PROTO_ENOTSUP = 5,
  PROTO_EPERM = 6,      /* pins held by another instrument */
  PROTO_ENOMEM = 7,
  PROTO_ENOCMD = 8,     /* unknown command */
  PROTO_ESTATE = 9,     /* wrong state (e.g. read before capture done) */
};

/* --- commands --------------------------------------------------------- */

/* System (0x0x) */
#define CMD_PING          0x01  /* -> "TRMT", proto_version u8 */
#define CMD_INFO          0x02  /* -> fw_version[16], board[16], sysclk u32, chip_id[8], features u32 */
#define CMD_BOOTLOADER    0x03  /* reset into the ROM's UF2 bootloader */
#define CMD_LED           0x04  /* r u8, g u8, b u8 (0/1); 0xFF,x,x -> back to auto */
#define CMD_POWER         0x05  /* on u8 -> on u8 (no payload: query) */
#define CMD_PULLUPS       0x06  /* on u8 -> on u8 (no payload: query) */
#define CMD_DIO_DIR       0x07  /* mask u8, out_mask u8 */
#define CMD_DIO_WRITE     0x08  /* mask u8, value u8 */
#define CMD_DIO_READ      0x09  /* -> value u8 */
#define CMD_STATUS        0x0A  /* -> pins u32, power u8, pullups u8, scope u8, logic u8, awg u8, spi_sniff u8, i2c_sniff u8, bridge u8, rx_dropped u32 */
#define CMD_DISPLAY_TEXT  0x0B  /* text (<= 24 chars) shown on the dashboard's note row */
#define CMD_ADC_READ      0x0C  /* channel u8 -> mv u16 */
#define CMD_DISPLAY_PINOUT 0x0D /* seconds u16: J1 pinout on the display (0 = back to the dashboard, 0xFFFF = until told otherwise) */
#define CMD_TIME          0x0E  /* [stamp u8] -> t_us u32: the board clock every event timestamp uses (wraps
                                 * after ~71 min). stamp = 1 also stamps the sniffer events: EVT_SPI_MSG /
                                 * EVT_I2C_MSG then set flags bit 7 and carry t_us u32 right after the flags */
#define CMD_DIO_WATCH     0x0F  /* mask u8 -> events EVT_DIO on every edge of those DIOs (0 = off) */

/* SPI (0x1x) - DIO2 CS, DIO3 MISO, DIO4 MOSI, DIO5 SCK */
#define CMD_SPI_CONF      0x10  /* freq u32, mode u8, lsb_first u8 -> actual freq u32 */
#define CMD_SPI_SEL       0x11  /* CS low */
#define CMD_SPI_UNSEL     0x12  /* CS high */
#define CMD_SPI_WRITE     0x13  /* data[] */
#define CMD_SPI_READ      0x14  /* n u16, fill u8 -> data[n] */
#define CMD_SPI_XFER      0x15  /* data[] -> rx[] */
#define CMD_SPI_TXN       0x16  /* read_n u16, data[] -> rx[read_n]: sel, write, read, unsel */
#define CMD_SPI_SNIFF     0x17  /* on u8, mode u8 -> events EVT_SPI_MSG */

/* I2C (0x2x) - DIO6 SCL, DIO7 SDA, bit-banged master */
#define CMD_I2C_CONF      0x20  /* hz u32, pullups u8 -> actual hz u32 */
#define CMD_I2C_SCAN      0x21  /* -> bitmap[16] (bit set = ACKed) */
#define CMD_I2C_WRITE     0x22  /* addr u8, data[] -> acked u16 */
#define CMD_I2C_READ      0x23  /* addr u8, n u16 -> data[n] */
#define CMD_I2C_WRITE_READ 0x24 /* addr u8, wn u8, w[wn], rn u16 -> data[rn] */
#define CMD_I2C_START     0x25  /* addr_rw u8 -> ack u8 (repeated start if the bus is open) */
#define CMD_I2C_STOP      0x26
#define CMD_I2C_RAW_WRITE 0x27  /* data[] -> acked u16 (bus must be open) */
#define CMD_I2C_RAW_READ  0x28  /* n u16, nack_last u8 -> data[n] */
#define CMD_I2C_SNIFF     0x29  /* on u8, pullups u8 -> events EVT_I2C_MSG */
#define CMD_I2C_RESET     0x2A  /* 9 SCL pulses + STOP to unstick a slave */

/* Scope (0x3x) - AIN0/AIN1 */
#define CMD_SCOPE_ARM     0x30  /* channels u8, n u16, clkdiv u16, trig_mode u8, trig_level u16, pre u16, timeout_ms u16 -> rate u32 */
#define CMD_SCOPE_STATUS  0x31  /* -> state u8, channels u8, n u16, rate u32, trig_index u16 */
#define CMD_SCOPE_READ    0x32  /* offset u16, count u16 -> samples u16[count] (interleaved when 2ch) */
#define CMD_SCOPE_ABORT   0x33

/* Waveform generator (0x4x) - AOUT0 */
#define CMD_AWG_SET       0x40  /* shape u8, freq_hz u32, amp_mv u16, off_mv u16, param u16 -> achieved freq_mhz u32, n u16, top u16 */
#define CMD_AWG_OFF       0x41
#define CMD_AWG_STATUS    0x42  /* -> running u8, shape u8, freq_mhz u32, amp_mv u16, off_mv u16 */
#define CMD_AWG_ARB_LOAD  0x43  /* offset u16, samples u8[] (0..255 = 0..3.3V) */
#define CMD_AWG_DC        0x44  /* mv u16 */
#define CMD_AWG_STEP      0x45  /* from_mv u16, to_mv u16, delay_ms u16 */

/* PWM outputs (0x48..) - DIO header, pairs 7/6, 5/4, 3/2, 1/0 share a frequency */
#define CMD_PWM_SET       0x48  /* dio u8, freq_hz u32, duty u16 (0.01 %), invert u8 -> freq_mhz u32, level u16, top u16, div u8 */
#define CMD_PWM_PHASE     0x49  /* dio u8, phase_deg u16 (pair-wide, applied on sync) */
#define CMD_PWM_SYNC      0x4A  /* dio_mask u8 (0 = all live pairs) */
#define CMD_PWM_STOP      0x4B  /* dio_mask u8 */
#define CMD_PWM_STATUS    0x4C  /* -> 8 x (flags u8 [0 active, 1 lost, 2 invert], freq_hz u32, freq_mhz u32, duty u16, phase_deg u16), DIO0 first */

/* Logic analyzer (0x5x) - DIO0..DIO7 */
#define CMD_LOGIC_ARM     0x50  /* n u32, clkdiv u16, trig_mode u8, trig_pin u8, trig_mask u8, trig_value u8, pre u32, timeout_ms u16 -> rate u32 */
#define CMD_LOGIC_STATUS  0x51  /* -> state u8, n u32, rate u32, trig_index u32 */
#define CMD_LOGIC_READ    0x52  /* offset u32, count u16 -> samples u8[count] (bit i = DIOi) */
#define CMD_LOGIC_ABORT   0x53

/* UART bridge (0x6x) - DIO0 RX, DIO1 TX, data flows on the third CDC port */
#define CMD_UART_CONF     0x60  /* mode u8 (0 auto/DTR, 1 on, 2 off), baud u32 (0 = follow host) */
#define CMD_UART_STATUS   0x61  /* -> active u8, baud u32, rx u32, tx u32 */
#define CMD_UART_CTRL     0x62  /* mode u8 (0 off, 1 direct, 2 esp), reset_dio u8, boot_dio u8 (0xFF = none):
                                 * the bridge port's RTS drives reset_dio and DTR boot_dio (see bridge.h) */

/* Emulated targets (0x64..) - the board answers as an I2C or SPI slave from a 256-byte register file */
#define CMD_EMU_I2C       0x64  /* on u8, addr u8, scl_dio u8, sda_dio u8, pullups u8 */
#define CMD_EMU_SPI       0x65  /* on u8, mode u8, cs u8, sck u8, mosi u8, miso u8, addr_mask u8, read_mask u8,
                                 * read_value u8, status u8 (MISO byte during the command byte) */
#define CMD_EMU_REGS      0x66  /* op u8: 0 set (offset u8, data[]), 1 get (offset u8, n u16 -> data[n]),
                                 * 2 read-only mask (mask[32], bit r = register r ignores writes) */
#define CMD_EMU_STATUS    0x67  /* -> kind u8 (0 off, 1 i2c, 2 spi), txns u32, reads u32, writes u32, naks u32,
                                 *    starts u32, stops u32, bytes u32 (low-level counters) */

/* Sequencer (0x68..) - a small program run on the board with microsecond timing, see seq.h */
#define CMD_SEQ_LOAD      0x68  /* offset u16, code[] */
#define CMD_SEQ_RUN       0x69  /* len u16, timeout_ms u32 -> runs async, EVT_SEQ_DONE at the end */
#define CMD_SEQ_STATUS    0x6A  /* -> state u8, error u8, pc u16, result_len u16 */
#define CMD_SEQ_RESULT    0x6B  /* offset u16, n u16 -> result[] (records, see seq.h) */
#define CMD_SEQ_ABORT     0x6C

/* Events (board -> host, seq 0) */
#define EVT_LOG           0xF0  /* text */
#define EVT_SPI_MSG       0xF1  /* flags u8 (bit0 overrun, bit7 stamped), [t_us u32], total u16, mosi[k], miso[k] */
#define EVT_I2C_MSG       0xF2  /* flags u8 (bit7 stamped), [t_us u32], records: (type u8, value u8)*: 1 START addr_rw,
                                 * 2 ACK(1)/NAK(0), 3 DATA, 4 STOP */
#define EVT_FLAG_STAMPED  0x80
#define EVT_CAPTURE_DONE  0xF3  /* instrument u8 (0 scope, 1 logic), state u8 */
#define EVT_DIO           0xF4  /* t_us u32, levels u8, changed u8, lost u8 (edges dropped before this one) */
#define EVT_EMU           0xF5  /* t_us u32, kind u8 (1 i2c, 2 spi), flags u8 (bit0 read, bit1 nak, bit2 truncated,
                                 * bit3 no START), addr u8, reg u8, data[] (bytes written to / read from the
                                 * register file). A NAK reports an address byte not answered: addr = byte >> 1,
                                 * reg = the raw byte, no data */
#define EVT_LINE          0xF6  /* t_us u32, dtr u8, rts u8 (host control lines on the bridge port) */
#define EVT_SEQ_DONE      0xF7  /* t_us u32, state u8, error u8, pc u16, result_len u16 */

/* Capture states (scope / logic). */
enum capture_state {
  CAP_IDLE = 0,
  CAP_ARMED = 1,      /* waiting for the trigger */
  CAP_RUNNING = 2,    /* triggered, filling post-trigger samples */
  CAP_DONE = 3,
  CAP_TIMEOUT = 4,
  CAP_ABORTED = 5,
};

/* Set by CMD_TIME: the sniffers stamp their events (EVT_FLAG_STAMPED). */
extern volatile bool proto_event_stamps;

/* Maps a negative errno from the instruments to a reply status. */
enum proto_status proto_errno_to_status(int err);

/* Sends an event frame to the host (drops it if the API port isn't being
 * drained). Safe from any thread. */
void proto_send_event(uint8_t evt, const uint8_t *payload, size_t len);

/* Starts the API thread on the second CDC-ACM port. */
void proto_init(void);

/* Optional tap: called (from the sending thread) with every event, so
 * the shell can echo sniffer messages to a human as well. NULL clears. */
typedef void (*proto_event_tap_t)(uint8_t evt, const uint8_t *payload, size_t len);
void proto_set_event_tap(proto_event_tap_t tap);

/* Small LE helpers shared by the command handlers. */
static inline uint16_t rd16(const uint8_t *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static inline void wr32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
