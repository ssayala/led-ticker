from led_ticker.errors import (
    AuthError, DeviceNotFoundError, LedTickerError, ProtocolError, ValidationError,
)


def test_all_errors_subclass_base():
    for cls in (DeviceNotFoundError, AuthError, ValidationError, ProtocolError):
        assert issubclass(cls, LedTickerError)


def test_auth_error_carries_pin_present_flag():
    assert AuthError(pin_present=True).pin_present is True
    assert AuthError().pin_present is False
