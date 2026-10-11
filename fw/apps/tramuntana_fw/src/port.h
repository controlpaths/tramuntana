/*
 * Tramuntana - port: a Zephyr UART (CDC-ACM or hardware) wrapped in
 * interrupt-driven RX/TX ring buffers with blocking read/write on top.
 * Used for the API port (src/proto.c) and both ends of the UART bridge
 * (src/bridge.c), so none of them has to deal with uart_fifo_* directly.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/ring_buffer.h>

struct port {
  const struct device *dev;
  struct ring_buf rx;
  struct ring_buf tx;
  struct k_sem rx_sem;   /* given by the ISR whenever bytes land in rx */
  struct k_sem tx_sem;   /* given by the ISR whenever tx space frees up */
  uint32_t rx_dropped;   /* bytes lost to a full rx ring (hardware UARTs only) */
  uint32_t rx_total;
  uint32_t tx_total;
  bool backpressure;     /* CDC-ACM: leave data in the driver when the ring is full */
  volatile bool rx_stalled;
};

/* Wires 'dev' to the rings (caller-provided storage) and enables RX
 * interrupts. With 'backpressure' (CDC-ACM ports) a full RX ring makes
 * the ISR stop reading so the USB stack NAKs the host instead of bytes
 * being dropped; a hardware UART can't do that, so those ports count
 * drops. Returns 0 or a negative errno if the device isn't ready. */
int port_init(struct port *p, const struct device *dev, uint8_t *rx_buf, size_t rx_size,
              uint8_t *tx_buf, size_t tx_size, bool backpressure);

/* Reads up to 'n' bytes, blocking until at least one is available or
 * 'timeout' passes. Returns the number of bytes read (0 on timeout). */
size_t port_read(struct port *p, uint8_t *buf, size_t n, k_timeout_t timeout);

/* Queues 'n' bytes for transmission, waiting up to 'timeout' for ring
 * space when it's full (e.g. nobody has the CDC port open). Returns the
 * number of bytes actually queued. */
size_t port_write(struct port *p, const uint8_t *buf, size_t n, k_timeout_t timeout);

/* Bytes waiting in the RX ring. */
size_t port_rx_pending(struct port *p);

/* Throws away everything in the RX ring. */
void port_rx_flush(struct port *p);
