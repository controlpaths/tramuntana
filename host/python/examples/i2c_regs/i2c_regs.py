#!/usr/bin/env python3
"""Dump the first 32 registers of every I2C device on the bus."""
from tramuntana import Tramuntana, ProtoError

with Tramuntana() as t:
    t.power(True)
    t.i2c.config(400_000, pullups=True)
    for addr in t.i2c.scan():
        try:
            regs = t.i2c.reg_read(addr, 0x00, 32)
            print(f"0x{addr:02x}: {regs.hex(' ')}")
        except ProtoError as e:
            print(f"0x{addr:02x}: {e}")
