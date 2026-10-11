/*
 * Tramuntana - arena. See arena.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/devicetree.h>
#include <string.h>

#include "arena.h"

#define ARENA_NODE DT_NODELABEL(capture_arena)

static uint8_t arena_mem[ARENA_SCOPE_BYTES + ARENA_SCOPE_BYTES + ARENA_RING_BYTES]
  Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(ARENA_NODE));

BUILD_ASSERT(DT_REG_ADDR(ARENA_NODE) % ARENA_SCOPE_BYTES == 0,
             "scope ring must be 32KB aligned");
BUILD_ASSERT((DT_REG_ADDR(ARENA_NODE) + 2 * ARENA_SCOPE_BYTES) % ARENA_RING_BYTES == 0,
             "shared ring must be 128KB aligned");
BUILD_ASSERT(DT_REG_SIZE(ARENA_NODE) >= sizeof(arena_mem), "capture arena too small");

static const char *ring_holder;
static arena_evict_cb_t ring_evict;
static struct k_spinlock ring_lock;

uint8_t *arena_scope(void)
{
  /* First 32KB: aligned to 32KB because the region itself is. */
  return arena_mem;
}

uint8_t *arena_ring(void)
{
  /* Skip 64KB so the ring lands on the 128KB boundary at +0x10000. */
  return arena_mem + 2 * ARENA_SCOPE_BYTES;
}

int arena_ring_claim(const char *who)
{
  k_spinlock_key_t key = k_spin_lock(&ring_lock);

  if (ring_holder != NULL && ring_holder != who) {
    arena_evict_cb_t cb = ring_evict;

    if (cb == NULL) {
      k_spin_unlock(&ring_lock, key);
      return -EBUSY;
    }
    ring_evict = NULL;
    ring_holder = NULL;
    k_spin_unlock(&ring_lock, key);
    cb();
    key = k_spin_lock(&ring_lock);
  }
  ring_holder = who;
  ring_evict = NULL;
  k_spin_unlock(&ring_lock, key);
  return 0;
}

void arena_ring_set_evictable(const char *who, arena_evict_cb_t cb)
{
  k_spinlock_key_t key = k_spin_lock(&ring_lock);

  if (ring_holder == who) {
    ring_evict = cb;
  }
  k_spin_unlock(&ring_lock, key);
}

void arena_ring_release(const char *who)
{
  k_spinlock_key_t key = k_spin_lock(&ring_lock);

  if (ring_holder == who) {
    ring_holder = NULL;
    ring_evict = NULL;
  }
  k_spin_unlock(&ring_lock, key);
}

const char *arena_ring_holder(void)
{
  return ring_holder;
}
