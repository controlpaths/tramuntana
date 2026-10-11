"""
tramuntana - host library for the Tramuntana Lite hardware hacking tool.

    from tramuntana import Tramuntana
    t = Tramuntana()
    t.i2c.scan()
"""
from .device import (Tramuntana, find_ports, find_api_port, ScopeCapture, LogicCapture,
                     SpiMessage, I2cMessage, I2cFrame, DioEdge, EmuTransfer, LineChange, SeqDone,
                     decode_spi_event, decode_i2c_event, decode_event)
from .proto import ProtoError, LinkError
from .seq import Sequence, SeqResult, SeqStep
from .timeline import Timeline, UartTap, Entry
from . import decode

__version__ = "2.2.0"
__all__ = ["Tramuntana", "find_ports", "find_api_port", "ScopeCapture", "LogicCapture",
           "SpiMessage", "I2cMessage", "I2cFrame", "DioEdge", "EmuTransfer", "LineChange", "SeqDone",
           "decode_spi_event", "decode_i2c_event", "decode_event", "ProtoError", "LinkError",
           "Sequence", "SeqResult", "SeqStep", "Timeline", "UartTap", "Entry", "decode"]
