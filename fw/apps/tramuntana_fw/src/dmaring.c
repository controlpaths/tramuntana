/*
 * Tramuntana - dmaring. See dmaring.h.
 */

#include <zephyr/kernel.h>
#include <hardware/dma.h>
#include <string.h>

#include "dmaring.h"

static void release_all(struct dma_ring *r)
{
  int i;

  for (i = 0; i < 2; i++) {
    if (r->data[i] >= 0) {
      dma_channel_unclaim(r->data[i]);
      r->data[i] = -1;
    }
    if (r->ctl[i] >= 0) {
      dma_channel_unclaim(r->ctl[i]);
      r->ctl[i] = -1;
    }
  }
}

int dma_ring_start(struct dma_ring *r, uint8_t *buf, uint32_t size, uint32_t item_size,
                   uint32_t dreq, const volatile void *src)
{
  enum dma_channel_transfer_size ts;
  int i;

  memset(r, 0, sizeof(*r));
  r->data[0] = r->data[1] = r->ctl[0] = r->ctl[1] = -1;
  if (size == 0 || (size % (2 * item_size)) != 0) {
    return -EINVAL;
  }
  switch (item_size) {
  case 1:
    ts = DMA_SIZE_8;
    break;
  case 2:
    ts = DMA_SIZE_16;
    break;
  case 4:
    ts = DMA_SIZE_32;
    break;
  default:
    return -EINVAL;
  }
  r->buf = buf;
  r->size = size;
  r->half = size / 2;
  r->item_size = item_size;
  r->reload[0] = (uint32_t)buf;
  r->reload[1] = (uint32_t)buf + r->half;

  for (i = 0; i < 2; i++) {
    r->data[i] = dma_claim_unused_channel(false);
    r->ctl[i] = dma_claim_unused_channel(false);
  }
  if (r->data[0] < 0 || r->data[1] < 0 || r->ctl[0] < 0 || r->ctl[1] < 0) {
    release_all(r);
    return -EBUSY;
  }

  for (i = 0; i < 2; i++) {
    dma_channel_config dc = dma_channel_get_default_config(r->data[i]);
    dma_channel_config cc = dma_channel_get_default_config(r->ctl[i]);

    /* Data channel i fills half i, then chains to the control channel
     * that restarts the *other* data channel. */
    channel_config_set_transfer_data_size(&dc, ts);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, dreq);
    channel_config_set_chain_to(&dc, r->ctl[i ^ 1]);
    dma_channel_configure(r->data[i], &dc, buf + i * r->half, src, r->half / item_size, false);

    /* Control channel i: one word, reload[i] -> data[i].WRITE_ADDR_TRIG.
     * TRANS_COUNT is reloaded from its last programmed value on every
     * trigger, so nothing else needs rewriting. */
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, false);
    channel_config_set_write_increment(&cc, false);
    dma_channel_configure(r->ctl[i], &cc, &dma_hw->ch[r->data[i]].al2_write_addr_trig,
                          &r->reload[i], 1, false);
  }

  r->running = true;
  dma_channel_start(r->data[0]);
  return 0;
}

uint32_t dma_ring_write_offset(const struct dma_ring *r)
{
  int spins;

  for (spins = 0; spins < 1000; spins++) {
    if (dma_channel_is_busy(r->data[0])) {
      return dma_channel_hw_addr(r->data[0])->write_addr - (uint32_t)r->buf;
    }
    if (dma_channel_is_busy(r->data[1])) {
      return dma_channel_hw_addr(r->data[1])->write_addr - (uint32_t)r->buf;
    }
    /* In between: a control channel is handing over. */
  }
  return 0;
}

void dma_ring_stop(struct dma_ring *r)
{
  if (!r->running) {
    return;
  }
  dma_hw->abort = BIT(r->data[0]) | BIT(r->data[1]) | BIT(r->ctl[0]) | BIT(r->ctl[1]);
  while (dma_hw->abort) {
    /* wait for the aborts to land */
  }
  dma_channel_cleanup(r->data[0]);
  dma_channel_cleanup(r->data[1]);
  dma_channel_cleanup(r->ctl[0]);
  dma_channel_cleanup(r->ctl[1]);
  release_all(r);
  r->running = false;
}
