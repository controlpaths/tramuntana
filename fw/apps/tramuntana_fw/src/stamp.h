/*
 * Tramuntana - stamp: exact board timestamps for the sniffers' messages.
 *
 * The sniffers' PIO programs raise PIO0 interrupt flag 0 at the bus event
 * that starts a message (I2C START, SPI CS falling). The ISR here takes
 * board_time_us() and the DMA ring's write position at that moment; the
 * decode thread, which may run milliseconds later, asks for the stamp of
 * the ring word it is decoding and gets the time the event happened, not
 * the time it got the CPU. Only one sniffer runs at a time (they share
 * the arena ring), so one queue serves both.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <hardware/pio.h>

#include "dmaring.h"

#define STAMP_PIO_FLAG 0

void stamp_init(void);
/* 'ring_words' = size of the ring in 32-bit words. */
void stamp_start(PIO pio, const struct dma_ring *ring, uint32_t ring_words);
void stamp_stop(PIO pio);
/* The stamp of the event behind the ring word at index 'word' (the latest
 * stamp taken at or just before it); false if there is none. Consumes the
 * stamps up to it, so call it for every message-start word in order. */
bool stamp_for(uint32_t word, uint32_t *t_us);
