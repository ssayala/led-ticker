"""Exception hierarchy for led_ticker. The library raises these; only the CLI
turns them into printed messages and process exit codes."""
from __future__ import annotations


class LedTickerError(Exception):
    """Base class for all led_ticker errors."""


class DeviceNotFoundError(LedTickerError):
    """No matching LED-Ticker device was found while scanning."""


class AuthError(LedTickerError):
    """The device rejected our PIN, or requires one we do not have."""

    def __init__(self, message: str = "", *, pin_present: bool = False):
        super().__init__(message or "device rejected authentication")
        self.pin_present = pin_present


class ValidationError(LedTickerError):
    """Input failed client-side validation before any BLE write."""


class ProtocolError(LedTickerError):
    """The device returned a response we could not parse."""
