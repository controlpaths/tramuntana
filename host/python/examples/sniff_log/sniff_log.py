#!/usr/bin/env python3
"""Log every I2C and SPI message on the header to stdout with a timestamp."""
import time
from tramuntana import Tramuntana

with Tramuntana() as t:
    t.i2c.sniff(True, pullups=False)
    t.spi.sniff(True)
    print("listening (Ctrl-C to stop)")
    try:
        while True:
            for msg in t.events(timeout=0.5):
                print(time.strftime("%H:%M:%S"), msg)
    except KeyboardInterrupt:
        pass
    finally:
        t.i2c.sniff(False)
        t.spi.sniff(False)
