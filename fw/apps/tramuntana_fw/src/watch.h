/*
 * Tramuntana - watch: timestamped DIO edges for the host's timeline.
 * Every edge on a watched DIO becomes an EVT_DIO event (board time in
 * microseconds, all eight levels, which pins changed). It only listens:
 * the GPIO interrupt sees the pad's input whatever drives the pin, so a
 * reset line, an IRQ or a status LED can be watched while the target, or
 * one of this board's own instruments, owns it.
 *
 * Meant for slow control signals, not for buses: a pin that toggles more
 * than WATCH_MAX_EDGES_PER_MS times in a millisecond is muted for
 * WATCH_MUTE_MS (the lost edges are counted in the next event) so a fast
 * clock can't starve the CPU. Use the logic analyzer for those.
 */
#pragma once

#include <stdint.h>

#define WATCH_MAX_EDGES_PER_MS 50
#define WATCH_MUTE_MS          100

void watch_init(void);
/* Starts watching the DIOs in 'mask' (bit i = DIOi), 0 stops. */
int watch_set(uint8_t mask);
uint8_t watch_get(void);
