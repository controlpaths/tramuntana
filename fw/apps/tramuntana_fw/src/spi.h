/*
 * Tramuntana - spi: SPIDriver-style master on SPI0 (DIO2 CS, DIO3 MISO,
 * DIO4 MOSI, DIO5 SCK) with chip select under the host's control, plus a
 * passive PIO sniffer of the same pins. See spi.c.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SPI_MIN_FREQ_HZ 1000
#define SPI_MAX_XFER    4096

void spi_drv_init(void);

/* Master. All of these claim DIO2..5 (soft) the first time they run;
 * -EBUSY if a capture holds them. */
int spi_drv_conf(uint32_t freq_hz, uint8_t mode, bool lsb_first, uint32_t *actual_hz);
int spi_drv_sel(void);
int spi_drv_unsel(void);
/* tx and/or rx may be NULL (rx: discard; tx: send 'fill'). CS untouched. */
int spi_drv_xfer(const uint8_t *tx, uint8_t *rx, size_t n, uint8_t fill);
/* sel, write tx, read rn bytes, unsel - one atomic transaction. */
int spi_drv_txn(const uint8_t *tx, size_t tn, uint8_t *rx, size_t rn);

uint32_t spi_drv_freq(void);
uint8_t spi_drv_mode(void);
bool spi_drv_lsb_first(void);
bool spi_drv_selected(void);

/* Sniffer: watches CS/SCK/MOSI, one EVT_SPI_MSG per CS-low burst. */
int spi_sniff_start(uint8_t mode);
int spi_sniff_stop(void);
bool spi_sniff_active(void);
uint32_t spi_sniff_count(void);
