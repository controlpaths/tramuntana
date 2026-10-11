/*
 * Tramuntana - i2c: I2CDriver-style master on DIO6 (SCL) / DIO7 (SDA),
 * bit-banged so arbitrary START / bytes / repeated START / STOP sequences
 * can be scripted from the host, plus a passive PIO sniffer. See i2c.c.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define I2C_MIN_HZ      1000
#define I2C_MAX_HZ      1000000
#define I2C_MAX_XFER    4096

void i2c_drv_init(void);

int i2c_drv_conf(uint32_t hz, bool pullups, uint32_t *actual_hz);
uint32_t i2c_drv_hz(void);

/* High level: each is a complete START..STOP transaction. -EIO = NAK. */
int i2c_drv_scan(uint8_t bitmap[16]);
int i2c_drv_write(uint8_t addr, const uint8_t *data, size_t n, size_t *acked);
int i2c_drv_read(uint8_t addr, uint8_t *data, size_t n);
int i2c_drv_write_read(uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn);

/* Low level: the bus stays open between calls until i2c_drv_stop(). */
int i2c_drv_start(uint8_t addr_rw, bool *ack);   /* repeated START if already open */
int i2c_drv_stop(void);
int i2c_drv_raw_write(const uint8_t *data, size_t n, size_t *acked);
int i2c_drv_raw_read(uint8_t *data, size_t n, bool nack_last);
bool i2c_drv_is_open(void);
/* Bus recovery: up to 9 SCL pulses until SDA is released, then STOP. */
int i2c_drv_reset(void);

/* Sniffer: one EVT_I2C_MSG per START..STOP. */
int i2c_sniff_start(bool pullups);
int i2c_sniff_stop(void);
bool i2c_sniff_active(void);
/* Redraws the I2C dashboard line (clock, pull-ups) unless the sniffer owns it. */
void i2c_ui_refresh(void);
uint32_t i2c_sniff_count(void);
