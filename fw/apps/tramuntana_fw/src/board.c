/*
 * Tramuntana - board. See board.h.
 *
 * The DIO header pins are driven straight through the Pico SDK's
 * hardware/gpio.h (SIO registers) rather than Zephyr's GPIO API: the
 * bit-banged I2C master and the pin arbiter both need to change a pad's
 * function and direction in a handful of cycles, and the Zephyr driver
 * is just a wrapper over the same registers anyway. LEDs, the power
 * switch and the pull-up enable are ordinary Zephyr devicetree GPIOs.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/logging/log.h>
#include <cmsis_core.h>
#include <hardware/gpio.h>
#include <hardware/adc.h>
#include <string.h>

#include "board.h"

LOG_MODULE_REGISTER(board, LOG_LEVEL_INF);

const char *const pin_owner_names[OWNER_COUNT] = {
  [OWNER_GPIO] = "gpio", [OWNER_SPI] = "spi", [OWNER_I2C] = "i2c",
  [OWNER_UART] = "uart", [OWNER_PIO] = "pio", [OWNER_PWM] = "pwm",
  [OWNER_EMU] = "emu",
};

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led_r), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led_g), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led_b), gpios);
static const struct gpio_dt_spec pu_en =
  GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), dio_pu_enable_gpios, 0);
static const struct device *const target_reg = DEVICE_DT_GET(DT_ALIAS(targetpwr));

static struct {
  enum pin_owner owner[DIO_COUNT];
  bool exclusive[DIO_COUNT];
  bool pullups;
  uint32_t pullups_changes;
  enum led_color led_bg;
  void (*on_change)(void);
  struct k_work_delayable led_work;
  struct k_spinlock lock;
} board;

/* --- LED --------------------------------------------------------------- */

/* Straight to the SIO registers (the LEDs are active low): led_set() sits
 * in the path of every bus transaction, and three gpio_pin_set_dt()
 * calls cost ~20us where one masked write costs nothing. */
static void led_apply(enum led_color c)
{
  uint32_t mask = BIT(led_r.pin) | BIT(led_g.pin) | BIT(led_b.pin);
  uint32_t on = 0;

  if (c == LED_RED || c == LED_YELLOW || c == LED_MAGENTA || c == LED_WHITE) {
    on |= BIT(led_r.pin);
  }
  if (c == LED_GREEN || c == LED_YELLOW || c == LED_CYAN || c == LED_WHITE) {
    on |= BIT(led_g.pin);
  }
  if (c == LED_BLUE || c == LED_CYAN || c == LED_MAGENTA || c == LED_WHITE) {
    on |= BIT(led_b.pin);
  }
  gpio_put_masked(mask, mask & ~on);
}

static void led_work_fn(struct k_work *work)
{
  ARG_UNUSED(work);
  led_apply(board.led_bg);
}

void led_set(enum led_color color)
{
  if (k_work_delayable_is_pending(&board.led_work)) {
    k_work_cancel_delayable(&board.led_work);
  }
  led_apply(color);
}

void led_background(enum led_color color)
{
  board.led_bg = color;
  if (!k_work_delayable_is_pending(&board.led_work)) {
    led_apply(color);
  }
}

void led_idle(void)
{
  led_set(board.led_bg);
}

void led_flash(enum led_color color, int ms)
{
  led_apply(color);
  k_work_reschedule(&board.led_work, K_MSEC(ms));
}

/* --- power / pull-ups -------------------------------------------------- */

/* The regulator API is reference counted: every enable needs its own
 * disable. 'power on' twice and 'power off' once would leave the rail on,
 * so only a real change of state reaches the regulator. */
void board_on_change(void (*fn)(void))
{
  board.on_change = fn;
}

static void board_changed(void)
{
  if (board.on_change != NULL) {
    board.on_change();
  }
}

void board_power_set(bool on)
{
  if (on == regulator_is_enabled(target_reg)) {
    return;
  }
  if (on) {
    (void)regulator_enable(target_reg);
  } else {
    (void)regulator_disable(target_reg);
  }
  board_changed();
}

bool board_power_get(void)
{
  return regulator_is_enabled(target_reg);
}

/* R12/R13 (4k7) hang from GPIO10 on the connector side of DIO6/DIO7.
 * On = GPIO10 driven high. Off = GPIO10 disconnected (output driver off,
 * input isolated), never driven low: that would turn the two 4k7 into
 * pull-downs on the target's I2C bus. GPIO_DISCONNECTED is 0 and the
 * RP2xxx driver tests flags == GPIO_DISCONNECTED, so it goes through
 * gpio_pin_configure() without the devicetree flags. */
static void pullups_apply(bool on)
{
  if (on) {
    gpio_pin_configure_dt(&pu_en, GPIO_OUTPUT_ACTIVE);
  } else {
    gpio_pin_configure(pu_en.port, pu_en.pin, GPIO_DISCONNECTED);
  }
}

void board_pullups_set(bool on)
{
  pullups_apply(on);
  board.pullups = on;
  board.pullups_changes++;
  board_changed();
}

uint32_t board_pullups_changes(void)
{
  return board.pullups_changes;
}

bool board_pullups_get(void)
{
  return board.pullups;
}

/* --- pin arbiter -------------------------------------------------------- */

static void pin_apply_owner(int dio, enum pin_owner who)
{
  uint gpio = DIO_GPIO(dio);

  switch (who) {
  case OWNER_SPI:
    if (gpio == GPIO_SPI_CS) {
      /* CS is a GPIO the SPI driver toggles by hand (see spi.c): idle high,
       * level and direction set before the function switch so the pin
       * never glitches low on the way (a target would see a select). */
      gpio_put(gpio, 1);
      gpio_set_dir(gpio, GPIO_OUT);
      gpio_set_function(gpio, GPIO_FUNC_SIO);
    } else {
      gpio_set_function(gpio, GPIO_FUNC_SPI);
    }
    break;
  case OWNER_UART:
    /* GPIO6/7 are UART1's CTS/RTS; TX/RX reach them through UART_AUX. */
    gpio_set_function(gpio, GPIO_FUNC_UART_AUX);
    break;
  case OWNER_PIO:
    gpio_set_function(gpio, GPIO_FUNC_PIO0);
    break;
  case OWNER_PWM:
    gpio_set_function(gpio, GPIO_FUNC_PWM);
    break;
  case OWNER_EMU:
    /* Released (input) in PIO1's latch before emu.c sets directions. */
    gpio_set_function(gpio, GPIO_FUNC_PIO1);
    break;
  case OWNER_GPIO:
  case OWNER_I2C:
  default:
    /* Plain SIO input, no internal pulls: the target's own levels rule.
     * The bit-banged I2C drives the pad low by flipping it to output.
     * Direction first, then the function switch, so the pad never sees
     * SIO's (low) output value on the way to becoming an input. */
    gpio_set_dir(gpio, GPIO_IN);
    gpio_put(gpio, 0);
    gpio_set_function(gpio, GPIO_FUNC_SIO);
    break;
  }
  gpio_set_pulls(gpio, false, false);
  gpio_set_input_enabled(gpio, true);
}

int board_pins_claim(uint8_t dio_mask, enum pin_owner who, bool exclusive)
{
  k_spinlock_key_t key = k_spin_lock(&board.lock);
  int i;

  for (i = 0; i < DIO_COUNT; i++) {
    if ((dio_mask & BIT(i)) && board.exclusive[i] && board.owner[i] != who) {
      k_spin_unlock(&board.lock, key);
      LOG_DBG("DIO%d is held by %s", i, pin_owner_names[board.owner[i]]);
      return -EBUSY;
    }
  }
  for (i = 0; i < DIO_COUNT; i++) {
    if (dio_mask & BIT(i)) {
      if (board.owner[i] != who) {
        pin_apply_owner(i, who);
      }
      board.owner[i] = who;
      board.exclusive[i] = exclusive;
    }
  }
  k_spin_unlock(&board.lock, key);
  return 0;
}

void board_pins_release(uint8_t dio_mask, enum pin_owner who)
{
  k_spinlock_key_t key = k_spin_lock(&board.lock);
  int i;

  for (i = 0; i < DIO_COUNT; i++) {
    if ((dio_mask & BIT(i)) && board.owner[i] == who) {
      board.owner[i] = OWNER_GPIO;
      board.exclusive[i] = false;
      pin_apply_owner(i, OWNER_GPIO);
    }
  }
  k_spin_unlock(&board.lock, key);
}

enum pin_owner board_pin_owner(int dio)
{
  return board.owner[dio];
}

bool board_pin_exclusive(int dio)
{
  return board.exclusive[dio];
}

uint32_t board_pins_snapshot(void)
{
  uint32_t v = 0;
  int i;

  for (i = 0; i < DIO_COUNT; i++) {
    v |= ((uint32_t)(board.owner[i] & 0x7) | (board.exclusive[i] ? 0x8 : 0)) << (4 * i);
  }
  return v;
}

/* --- DIO as GPIO --------------------------------------------------------- */

int board_dio_dir(uint8_t mask, uint8_t out_mask)
{
  int ret = board_pins_claim(mask, OWNER_GPIO, false);
  int i;

  if (ret != 0) {
    return ret;
  }
  for (i = 0; i < DIO_COUNT; i++) {
    if (mask & BIT(i)) {
      gpio_set_dir(DIO_GPIO(i), (out_mask & BIT(i)) ? GPIO_OUT : GPIO_IN);
    }
  }
  return 0;
}

int board_dio_write(uint8_t mask, uint8_t value)
{
  int i;

  for (i = 0; i < DIO_COUNT; i++) {
    if (mask & BIT(i)) {
      if (board.owner[i] != OWNER_GPIO) {
        return -EPERM;
      }
      gpio_put(DIO_GPIO(i), (value >> i) & 1);
    }
  }
  return 0;
}

uint8_t board_dio_read(void)
{
  uint32_t all = gpio_get_all();
  uint8_t v = 0;
  int i;

  for (i = 0; i < DIO_COUNT; i++) {
    if (all & BIT(DIO_GPIO(i))) {
      v |= BIT(i);
    }
  }
  return v;
}

/* --- misc ----------------------------------------------------------------- */

K_MUTEX_DEFINE(board_adc_mutex);

int board_adc_read_mv(int channel)
{
  uint16_t raw;

  if (channel < 0 || channel > 1) {
    return -EINVAL;
  }
  if (k_mutex_lock(&board_adc_mutex, K_NO_WAIT) != 0) {
    return -EBUSY;
  }
  adc_select_input((uint)channel);
  raw = pico_adc_read();
  k_mutex_unlock(&board_adc_mutex);
  return (int)(((uint32_t)raw * BOARD_VREF_MV + 2047) / 4095);
}

uint32_t board_sysclk_hz(void)
{
  return CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;
}

uint32_t board_time_us(void)
{
  return (uint32_t)k_cyc_to_us_floor64(k_cycle_get_64());
}

uint32_t board_ns_to_cycles(uint32_t ns)
{
  return (uint32_t)(((uint64_t)ns * board_sysclk_hz()) / 1000000000ULL);
}

void board_delay_ns(uint32_t ns)
{
  board_delay_cycles(board_ns_to_cycles(ns));
}

void board_chip_id(uint8_t out[8])
{
  memset(out, 0, 8);
  hwinfo_get_device_id(out, 8);
}

void board_init(void)
{
  int i;

  gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
  gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
  gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);
  pullups_apply(false);
  k_work_init_delayable(&board.led_work, led_work_fn);
  board.led_bg = LED_OFF;

  if (!device_is_ready(target_reg)) {
    LOG_ERR("target regulator not ready");
  }

  /* Cycle counter for board_delay_ns(). */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  /* Whatever the devicetree pinctrl groups muxed at boot (SPI0 on
   * DIO2..5, UART1 on DIO1/0), start with every header pin as a plain
   * input: instruments claim what they need when they need it. */
  for (i = 0; i < DIO_COUNT; i++) {
    board.owner[i] = OWNER_GPIO;
    board.exclusive[i] = false;
    pin_apply_owner(i, OWNER_GPIO);
  }
}
