/*
 * Tramuntana - dmaring: an endless circular DMA capture into a buffer of
 * any size (power of two not required).
 *
 * The RP2350 DMA's own address-wrapping mode (RING_SIZE) tops out at
 * 32KB, so bigger rings are built the way the datasheet suggests: two
 * data channels, one per half of the buffer, chained to each other
 * through two one-word control channels that rewrite the next data
 * channel's write address (via its WRITE_ADDR_TRIG alias) and so
 * restart it. A -> ctlB -> B -> ctlA -> A -> ... forever, with the
 * peripheral's DREQ pacing whichever data channel is running.
 *
 * Consumers track the write position with dma_ring_write_offset() (bytes
 * from the start of the buffer, wrapping at 'size') and keep their own
 * running total, like scope.c does with its hardware ring.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct dma_ring {
  int data[2];
  int ctl[2];
  uint8_t *buf;
  uint32_t size;
  uint32_t half;
  uint32_t item_size;
  uint32_t reload[2];   /* write addresses the control channels feed back */
  bool running;
};

/* Claims four channels and starts streaming 'item_size' (1/2/4) byte
 * items from 'src' (a FIFO register) into buf[0..size), paced by 'dreq'.
 * 'size' must be a multiple of 2 * item_size. Returns 0 or -EBUSY when
 * no channels are free. */
int dma_ring_start(struct dma_ring *r, uint8_t *buf, uint32_t size, uint32_t item_size,
                   uint32_t dreq, const volatile void *src);

/* Where the next item will be written, as a byte offset into buf. */
uint32_t dma_ring_write_offset(const struct dma_ring *r);

/* Aborts everything and releases the channels. */
void dma_ring_stop(struct dma_ring *r);
