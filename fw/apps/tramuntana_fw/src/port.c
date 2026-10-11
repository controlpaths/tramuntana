/*
 * Tramuntana - port. See port.h.
 */

#include <zephyr/drivers/uart.h>
#include <string.h>

#include "port.h"

static void port_isr(const struct device *dev, void *user_data)
{
  struct port *p = user_data;

  for (;;) {
    bool progressed = false;

    uart_irq_update(dev);
    if (!uart_irq_is_pending(dev)) {
      break;
    }

    if (uart_irq_rx_ready(dev)) {
      uint8_t *dst;
      uint32_t space = ring_buf_put_claim(&p->rx, &dst, 64);

      if (space == 0) {
        if (p->backpressure) {
          /* Ring full: stop RX interrupts and leave the bytes in the
           * driver (the USB stack stops accepting packets once its own
           * buffers fill up, so the host just waits); port_read()
           * re-enables RX as soon as there's room. Disabling matters:
           * the CDC-ACM class re-submits its callback work for as long
           * as RX is enabled and data is pending, and that work runs on
           * a cooperative workqueue - leaving it enabled here would spin
           * the whole system. */
          uart_irq_rx_disable(dev);
          p->rx_stalled = true;
          break;
        }
        /* Hardware UART: drain the FIFO into the void so the IRQ clears
         * rather than storming, and count what was lost. */
        {
          uint8_t scratch[16];
          int n = uart_fifo_read(dev, scratch, sizeof(scratch));

          if (n > 0) {
            p->rx_dropped += (uint32_t)n;
            progressed = true;
          }
        }
      } else {
        int n = uart_fifo_read(dev, dst, space);

        if (n < 0) {
          n = 0;
        }
        ring_buf_put_finish(&p->rx, (uint32_t)n);
        if (n > 0) {
          p->rx_total += (uint32_t)n;
          k_sem_give(&p->rx_sem);
          progressed = true;
        }
      }
    }

    if (uart_irq_tx_ready(dev)) {
      uint8_t *src;
      uint32_t avail = ring_buf_get_claim(&p->tx, &src, 256);

      if (avail == 0) {
        uart_irq_tx_disable(dev);
      } else {
        int n = uart_fifo_fill(dev, src, avail);

        if (n < 0) {
          n = 0;
        }
        ring_buf_get_finish(&p->tx, (uint32_t)n);
        if (n > 0) {
          p->tx_total += (uint32_t)n;
          k_sem_give(&p->tx_sem);
          progressed = true;
        }
      }
    }

    if (!progressed) {
      break;
    }
  }
}

int port_init(struct port *p, const struct device *dev, uint8_t *rx_buf, size_t rx_size,
              uint8_t *tx_buf, size_t tx_size, bool backpressure)
{
  if (!device_is_ready(dev)) {
    return -ENODEV;
  }
  memset(p, 0, sizeof(*p));
  p->dev = dev;
  p->backpressure = backpressure;
  ring_buf_init(&p->rx, rx_size, rx_buf);
  ring_buf_init(&p->tx, tx_size, tx_buf);
  k_sem_init(&p->rx_sem, 0, 1);
  k_sem_init(&p->tx_sem, 0, 1);
  uart_irq_callback_user_data_set(dev, port_isr, p);
  uart_irq_rx_enable(dev);
  return 0;
}

static void port_rx_unstall(struct port *p)
{
  if (p->rx_stalled) {
    p->rx_stalled = false;
    uart_irq_rx_enable(p->dev); /* CDC-ACM re-runs the callback if data waits */
  }
}

size_t port_read(struct port *p, uint8_t *buf, size_t n, k_timeout_t timeout)
{
  uint32_t got = ring_buf_get(&p->rx, buf, n);

  if (got > 0) {
    port_rx_unstall(p);
    return got;
  }
  if (k_sem_take(&p->rx_sem, timeout) != 0) {
    port_rx_unstall(p);
    return 0;
  }
  got = ring_buf_get(&p->rx, buf, n);
  port_rx_unstall(p);
  return got;
}

size_t port_write(struct port *p, const uint8_t *buf, size_t n, k_timeout_t timeout)
{
  size_t done = 0;
  int64_t deadline = K_TIMEOUT_EQ(timeout, K_FOREVER) ? INT64_MAX
                                                      : k_uptime_get() + k_ticks_to_ms_ceil64(timeout.ticks);

  while (done < n) {
    uint32_t put = ring_buf_put(&p->tx, buf + done, n - done);

    done += put;
    if (put > 0) {
      uart_irq_tx_enable(p->dev);
    }
    if (done < n) {
      int64_t now = k_uptime_get();

      if (now >= deadline) {
        break;
      }
      k_sem_take(&p->tx_sem, K_MSEC(MIN(deadline - now, 50)));
    }
  }
  return done;
}

size_t port_rx_pending(struct port *p)
{
  return ring_buf_size_get(&p->rx);
}

void port_rx_flush(struct port *p)
{
  ring_buf_reset(&p->rx);
  port_rx_unstall(p);
}
