/*
 * Tramuntana - stamp. See stamp.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>

#undef pio0
#undef pio1

#include "stamp.h"
#include "board.h"

#define STAMP_IRQ      PIO0_IRQ_0
#define STAMP_IRQ_PRIO 1
#define STAMP_QUEUE    64

struct stamp {
  uint32_t t_us;
  uint32_t word;   /* ring write position (in words) when the event fired */
};

static struct {
  PIO pio;
  const struct dma_ring *ring;
  uint32_t ring_words;
  struct stamp q[STAMP_QUEUE];
  volatile uint32_t head;
  uint32_t tail;
} st;

static void stamp_isr(const void *arg)
{
  struct stamp *s;

  ARG_UNUSED(arg);
  pio_interrupt_clear(st.pio, STAMP_PIO_FLAG);
  s = &st.q[st.head % STAMP_QUEUE];
  s->t_us = board_time_us();
  s->word = dma_ring_write_offset(st.ring) / 4;
  st.head++;
}

/* Is ring index 'a' at or before 'b' (modulo the ring, within half of it)? */
static bool at_or_before(uint32_t a, uint32_t b)
{
  return ((b - a + st.ring_words) % st.ring_words) < st.ring_words / 2;
}

bool stamp_for(uint32_t word, uint32_t *t_us)
{
  bool found = false;
  uint32_t head = st.head;

  if (head - st.tail > STAMP_QUEUE) {
    st.tail = head - STAMP_QUEUE;   /* overwritten while we were away */
  }
  /* The event fires just before its word is pushed, so the ring position
   * it saw can be the word itself or the one after. */
  while (st.tail != head && at_or_before(st.q[st.tail % STAMP_QUEUE].word, (word + 1) % st.ring_words)) {
    *t_us = st.q[st.tail % STAMP_QUEUE].t_us;
    st.tail++;
    found = true;
  }
  return found;
}

void stamp_start(PIO pio, const struct dma_ring *ring, uint32_t ring_words)
{
  st.pio = pio;
  st.ring = ring;
  st.ring_words = ring_words;
  st.head = 0;
  st.tail = 0;
  pio_interrupt_clear(pio, STAMP_PIO_FLAG);
  pio_set_irq0_source_enabled(pio, (pio_interrupt_source_t)(pis_interrupt0 + STAMP_PIO_FLAG), true);
  irq_enable(STAMP_IRQ);
}

void stamp_stop(PIO pio)
{
  irq_disable(STAMP_IRQ);
  pio_set_irq0_source_enabled(pio, (pio_interrupt_source_t)(pis_interrupt0 + STAMP_PIO_FLAG), false);
  pio_interrupt_clear(pio, STAMP_PIO_FLAG);
}

void stamp_init(void)
{
  IRQ_CONNECT(STAMP_IRQ, STAMP_IRQ_PRIO, stamp_isr, NULL, 0);
}
