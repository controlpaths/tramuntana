/*
 * Tramuntana - watch. See watch.h.
 *
 * The edge interrupt only takes the timestamp and the levels and queues
 * them; a thread turns the queue into EVT_DIO events, since sending a
 * frame takes the API port's mutex and can't happen in an ISR.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "watch.h"
#include "board.h"
#include "proto.h"

LOG_MODULE_REGISTER(watch, LOG_LEVEL_INF);

static const struct device *const gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

struct edge {
  uint32_t t_us;
  uint8_t levels;
  uint8_t prev;
};

K_MSGQ_DEFINE(edge_q, sizeof(struct edge), 64, 4);

static struct {
  uint8_t mask;          /* watched DIOs */
  uint8_t muted;         /* watched, but silenced for a while */
  uint8_t last;          /* levels at the previous edge */
  uint32_t lost;         /* edges not reported (queue full or muted) */
  uint32_t bucket_ms;
  uint16_t bucket_edges;
  int64_t muted_at;
  struct gpio_callback cb;
} w;

K_THREAD_STACK_DEFINE(watch_stack, 1024);
static struct k_thread watch_thread_data;

static uint32_t dio_to_gpio_mask(uint8_t dio_mask)
{
  uint32_t m = 0;
  int i;

  for (i = 0; i < DIO_COUNT; i++) {
    if (dio_mask & BIT(i)) {
      m |= BIT(DIO_GPIO(i));
    }
  }
  return m;
}

static void edge_irqs(uint8_t dio_mask, bool on)
{
  int i;

  for (i = 0; i < DIO_COUNT; i++) {
    if (dio_mask & BIT(i)) {
      gpio_pin_interrupt_configure(gpio_dev, DIO_GPIO(i), on ? GPIO_INT_EDGE_BOTH : GPIO_INT_DISABLE);
    }
  }
}

static void edge_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
  struct edge e;
  uint32_t now_ms = k_uptime_get_32();

  ARG_UNUSED(dev);
  ARG_UNUSED(cb);

  /* The callback covers every DIO; only watched pins count. */
  if ((pins & dio_to_gpio_mask(w.mask)) == 0) {
    return;
  }
  if (now_ms != w.bucket_ms) {
    w.bucket_ms = now_ms;
    w.bucket_edges = 0;
  }
  if (++w.bucket_edges > WATCH_MAX_EDGES_PER_MS) {
    /* Too fast to be a control line: mute everything for a while. */
    w.muted = w.mask;
    w.muted_at = k_uptime_get();
    edge_irqs(w.mask, false);
    w.lost++;
    return;
  }
  e.t_us = board_time_us();
  e.levels = board_dio_read();
  e.prev = w.last;
  w.last = e.levels;
  if (k_msgq_put(&edge_q, &e, K_NO_WAIT) != 0) {
    w.lost++;
  }
}

static void watch_thread(void *a, void *b, void *c)
{
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  for (;;) {
    struct edge e;

    if (k_msgq_get(&edge_q, &e, K_MSEC(WATCH_MUTE_MS)) == 0) {
      uint8_t evt[7];
      uint32_t lost = w.lost;

      w.lost = 0;
      wr32(evt, e.t_us);
      evt[4] = e.levels;
      evt[5] = (uint8_t)((e.levels ^ e.prev) & w.mask);
      evt[6] = (uint8_t)MIN(lost, 255u);
      proto_send_event(EVT_DIO, evt, sizeof(evt));
    }
    if (w.muted != 0 && k_uptime_get() - w.muted_at > WATCH_MUTE_MS) {
      unsigned int key = irq_lock();

      w.last = board_dio_read();
      edge_irqs(w.muted & w.mask, true);
      w.muted = 0;
      irq_unlock(key);
    }
  }
}

int watch_set(uint8_t mask)
{
  unsigned int key;

  if (!device_is_ready(gpio_dev)) {
    return -ENODEV;
  }
  key = irq_lock();
  edge_irqs(w.mask, false);
  w.mask = mask;
  w.muted = 0;
  w.lost = 0;
  w.last = board_dio_read();
  edge_irqs(mask, true);
  irq_unlock(key);
  if (mask == 0) {
    k_msgq_purge(&edge_q);
  }
  return 0;
}

uint8_t watch_get(void)
{
  return w.mask;
}

void watch_init(void)
{
  gpio_init_callback(&w.cb, edge_isr, dio_to_gpio_mask(DIO_MASK_ALL));
  gpio_add_callback(gpio_dev, &w.cb);
  k_thread_create(&watch_thread_data, watch_stack, K_THREAD_STACK_SIZEOF(watch_stack), watch_thread,
                  NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
  k_thread_name_set(&watch_thread_data, "watch");
}
