/*
 * Tramuntana - arena: the 192KB capture region reserved at 0x20050000 by
 * the devicetree overlay (boards/rpi_pico2_rp2350a_m33.overlay). Split in two:
 *
 *   0x20050000  32KB   scope ring (ADC samples, DMA address-wrapped)
 *   0x20060000  128KB  shared ring for the logic analyzer and the SPI/I2C
 *                      sniffers - one of them at a time, hence the claim.
 *
 * Both starts are aligned to their own size, which is what the RP2350
 * DMA's ring mode (channel_config_set_ring()) needs.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ARENA_SCOPE_BITS  15
#define ARENA_SCOPE_BYTES (1u << ARENA_SCOPE_BITS)
#define ARENA_RING_BITS   17
#define ARENA_RING_BYTES  (1u << ARENA_RING_BITS)

uint8_t *arena_scope(void);
uint8_t *arena_ring(void);

/* The shared ring: -EBUSY if another instrument holds it - unless that
 * holder registered itself as evictable (the logic analyzer keeps a
 * finished capture in the ring for the host to read, but gives it up
 * the moment someone else needs the memory: its evict callback runs
 * first, then the claim succeeds). */
typedef void (*arena_evict_cb_t)(void);
int arena_ring_claim(const char *who);
void arena_ring_release(const char *who);
void arena_ring_set_evictable(const char *who, arena_evict_cb_t cb);
const char *arena_ring_holder(void);
