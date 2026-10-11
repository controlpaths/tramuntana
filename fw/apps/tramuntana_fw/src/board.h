/*
 * Tramuntana - board: everything that isn't an instrument. The RGB LED,
 * the target's switched 3.3V rail, the I2C pull-up bias, the DIO header
 * as plain GPIO, single-shot ADC readings, a cycle-accurate busy wait,
 * and - the important one - the pin arbiter that decides which function
 * owns each DIO pin at any moment.
 *
 * Header/GPIO map (fw/boards/tramuntana_lite_r1a-common.dtsi):
 *
 *   DIOn = GPIO(7-n)     DIO0/1  = UART1 RX/TX (GPIO7/6, UART_AUX function)
 *                        DIO2..5 = SPI0 CSN/RX/TX/SCK (GPIO5..2)
 *                        DIO6/7  = I2C SCL/SDA (bit-banged)
 *   AIN0/1 = GPIO26/27   AOUT0 = GPIO8 (PWM 4A)
 *   PU_EN = GPIO10       PWR_EN = GPIO11 (active low)
 *   LED R/G/B = GPIO21/20/22 (active low, common anode)
 *
 * Pin arbiter: every instrument that *drives* header pins claims a DIO
 * mask with its owner id. Masters (SPI, I2C), PWM outputs and plain GPIO use are
 * "soft" owners: they keep the pins between operations (so the bus idles
 * at the right levels) but anyone else can take the pins over. The UART
 * bridge claims "exclusive": while it holds pins, any other claim on
 * them fails with -EBUSY, and the pins go back to plain GPIO inputs when
 * released. The claim also sets the pad's function (SIO / SPI / UART / PWM) so
 * callers never touch gpio_set_function() themselves.
 *
 * Passive instruments (sniffers, logic analyzer) don't claim anything:
 * the PIO reads every pad's input whatever its function select, so they
 * can watch a bus while this board's own master, the bridge, or a target
 * drives it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DIO_COUNT     8
#define DIO_MASK_ALL  0xFF
#define DIO_MASK_SPI  (BIT(2) | BIT(3) | BIT(4) | BIT(5))
#define DIO_MASK_I2C  (BIT(6) | BIT(7))
#define DIO_MASK_UART (BIT(0) | BIT(1))

/* DIO index -> RP2350 GPIO number. */
#define DIO_GPIO(n) (7 - (n))

#define GPIO_AIN0    26
#define GPIO_AIN1    27
#define GPIO_AOUT0   8
#define GPIO_PU_EN   10
#define GPIO_PWR_EN  11
#define GPIO_SPI_CS  5  /* DIO2 */
#define GPIO_SPI_SCK 2  /* DIO5 */
#define GPIO_SPI_TX  3  /* DIO4, MOSI */
#define GPIO_SPI_RX  4  /* DIO3, MISO */
#define GPIO_I2C_SCL 1  /* DIO6 */
#define GPIO_I2C_SDA 0  /* DIO7 */
#define GPIO_UART_TX 6  /* DIO1, UART1 TX via UART_AUX */
#define GPIO_UART_RX 7  /* DIO0, UART1 RX via UART_AUX */

#define BOARD_VREF_MV 3300

enum pin_owner {
  OWNER_GPIO = 0,
  OWNER_SPI,
  OWNER_I2C,
  OWNER_UART,
  OWNER_PIO,     /* sniffers, logic analyzer */
  OWNER_PWM,     /* PWM outputs (src/pwm.c) */
  OWNER_EMU,     /* emulated I2C/SPI target on PIO1 (src/emu.c) */
  OWNER_COUNT,
};

extern const char *const pin_owner_names[OWNER_COUNT];

/* Configures LEDs/power/pull-ups (all off), parks every DIO as a plain
 * GPIO input and enables the cycle counter. Call once at boot. */
void board_init(void);

/* --- pin arbiter --- */
int board_pins_claim(uint8_t dio_mask, enum pin_owner who, bool exclusive);
void board_pins_release(uint8_t dio_mask, enum pin_owner who);
enum pin_owner board_pin_owner(int dio);
bool board_pin_exclusive(int dio);
/* Owner id of each DIO packed for status reports: 4 bits per pin,
 * bit 3 = exclusive, bits 2:0 = owner. */
uint32_t board_pins_snapshot(void);

/* --- LED --- */
enum led_color {
  LED_OFF, LED_RED, LED_GREEN, LED_BLUE, LED_YELLOW, LED_CYAN, LED_MAGENTA, LED_WHITE,
};
void led_set(enum led_color color);          /* immediate */
void led_background(enum led_color color);   /* what 'idle' means (e.g. AWG running) */
void led_idle(void);                         /* back to the background colour */
void led_flash(enum led_color color, int ms); /* non-blocking: colour, then idle */

/* --- target power / pull-ups --- */
void board_power_set(bool on);
bool board_power_get(void);
void board_pullups_set(bool on);
bool board_pullups_get(void);
/* Counts every board_pullups_set(). An instrument that sets the pull-ups
 * for its own use (sniffer, emulator) and puts them back when it stops
 * only does so if this hasn't moved since: a change made meanwhile by the
 * user wins. */
uint32_t board_pullups_changes(void);
/* Called (from the caller's thread) after the target power or the pull-ups
 * change, whoever changed them - the dashboard redraws its lines. */
void board_on_change(void (*fn)(void));

/* --- DIO as GPIO (claims the pins as OWNER_GPIO, soft) --- */
int board_dio_dir(uint8_t mask, uint8_t out_mask);
int board_dio_write(uint8_t mask, uint8_t value);
uint8_t board_dio_read(void);

/* --- misc --- */
/* Single-shot reading of AIN0/AIN1 in millivolts, or a negative errno
 * (e.g. -EBUSY while the scope owns the ADC). */
int board_adc_read_mv(int channel);
uint32_t board_sysclk_hz(void);
/* The board clock in microseconds since boot, truncated to 32 bits (wraps
 * after ~71 minutes; hosts unwrap it). Every timestamped event uses it. */
uint32_t board_time_us(void);
/* Busy waits on the Cortex-M33 cycle counter (DWT), good down to a few
 * tens of ns - for the bit-banged I2C. board_delay_ns() does a 64-bit
 * division to convert, so hot loops precompute cycles with
 * board_ns_to_cycles() and use board_delay_cycles(). */
void board_delay_ns(uint32_t ns);
uint32_t board_ns_to_cycles(uint32_t ns);
static inline uint32_t board_cycles(void);
static inline void board_delay_cycles(uint32_t cycles);
/* Unique chip id (from hwinfo), 8 bytes. */
void board_chip_id(uint8_t out[8]);

#include <cmsis_core.h>

static inline uint32_t board_cycles(void)
{
  return DWT->CYCCNT;
}

static inline void board_delay_cycles(uint32_t cycles)
{
  uint32_t start = DWT->CYCCNT;

  while ((uint32_t)(DWT->CYCCNT - start) < cycles) {
    /* spin */
  }
}
