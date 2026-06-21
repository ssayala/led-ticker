import led_ticker.cli as cli
import led_ticker.protocol as P
from led_ticker.protocol import DeviceInfo
from led_ticker.errors import AmbiguousDeviceError


class FakeDevice:
    def __init__(self, recorder):
        self.r = recorder

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def set_tickers(self, symbols):
        self.r["tickers"] = symbols

    def get_status(self):
        return P.Status(text="BUSY", seconds=1800)


def test_unknown_command_prints_help_and_returns_1(capsys):
    assert cli.main(["bogus"]) == 1
    assert "Usage:" in capsys.readouterr().out


def test_tickers_dispatch_calls_library(monkeypatch):
    rec = {}
    monkeypatch.setattr(cli, "LedTicker", lambda **kw: FakeDevice(rec))
    assert cli.main(["tickers", "aapl", "msft"]) == 0
    assert rec["tickers"] == ["aapl", "msft"]


def test_validation_error_maps_to_exit_1(monkeypatch, capsys):
    # Empty ticker list is rejected by protocol.encode_tickers inside the device.
    class BadDevice(FakeDevice):
        def set_tickers(self, symbols):
            P.encode_tickers([])  # raises ValidationError

    monkeypatch.setattr(cli, "LedTicker", lambda **kw: BadDevice({}))
    assert cli.main(["tickers", "x"]) == 1
    assert "ERROR:" in capsys.readouterr().out


def test_status_get_formats_remaining(monkeypatch, capsys):
    monkeypatch.setattr(cli, "LedTicker", lambda **kw: FakeDevice({}))
    assert cli.main(["get", "status"]) == 0
    assert "30m 0s remaining" in capsys.readouterr().out


def test_pin_save_is_local_only(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(cli.auth, "DEFAULT_PIN_PATH", tmp_path / "pin")
    assert cli.main(["pin", "482913"]) == 0
    assert (tmp_path / "pin").read_text().strip() == "482913"


def _boom(**kw):
    raise AssertionError("LedTicker must not be constructed on a validation error")


def test_validation_happens_before_connecting(monkeypatch, capsys):
    monkeypatch.setattr(cli, "LedTicker", _boom)
    assert cli.main(["status", "a|b"]) == 1
    assert "cannot contain '|'" in capsys.readouterr().out


def test_mode_bad_token_validates_before_connecting(monkeypatch, capsys):
    monkeypatch.setattr(cli, "LedTicker", _boom)
    assert cli.main(["mode", "badtoken"]) == 1
    assert "ERROR:" in capsys.readouterr().out


def test_timer_zero_validates_before_connecting(monkeypatch, capsys):
    monkeypatch.setattr(cli, "LedTicker", _boom)
    assert cli.main(["timer", "0"]) == 1
    assert "ERROR:" in capsys.readouterr().out


# -- Fix D: --pin prefix wiring and missing-value guard ----------------------
def test_pin_prefix_passes_to_ledticker(monkeypatch):
    captured = {}

    def fake_factory(**kw):
        captured.update(kw)
        return FakeDevice(captured)

    monkeypatch.setattr(cli, "LedTicker", fake_factory)
    assert cli.main(["--pin", "123456", "tickers", "AAPL"]) == 0
    assert captured.get("pin") == "123456"


def test_pin_flag_missing_value_returns_1(capsys):
    assert cli.main(["--pin"]) == 1
    assert "ERROR:" in capsys.readouterr().out


def test_devices_lists_found(monkeypatch, capsys):
    monkeypatch.setattr(cli, "scan", lambda *a, **k: [
        DeviceInfo("LED-Ticker-A1B2", "AA:BB:CC:DD:EE:01", -54),
        DeviceInfo("LED-Ticker-9F3C", "AA:BB:CC:DD:EE:02", -71),
    ])
    assert cli.main(["devices"]) == 0
    out = capsys.readouterr().out
    assert "Found 2 LED-Ticker device(s):" in out
    assert "LED-Ticker-A1B2" in out and "-54 dBm" in out


def test_devices_none_found(monkeypatch, capsys):
    monkeypatch.setattr(cli, "scan", lambda *a, **k: [])
    assert cli.main(["devices"]) == 0
    assert "No LED-Ticker devices found." in capsys.readouterr().out


def test_device_flag_forwarded_as_select(monkeypatch):
    seen = {}

    class Dev:
        def __enter__(self_):
            return self_
        def __exit__(self_, *a):
            return False
        def set_power(self_, on):
            seen["power"] = on

    monkeypatch.setattr(cli, "LedTicker", lambda **kw: seen.update({"kw": kw}) or Dev())
    assert cli.main(["--device", "A1B2", "power", "on"]) == 0
    assert seen["kw"]["select"] == "A1B2"


def test_device_flag_requires_value(capsys):
    assert cli.main(["--device"]) == 1
    assert "--device requires a value" in capsys.readouterr().out


def test_ambiguous_non_interactive_lists_and_exits(monkeypatch, capsys):
    cands = [DeviceInfo("LED-Ticker-A1B2", "AA:BB:CC:DD:EE:01", -50),
             DeviceInfo("LED-Ticker-9F3C", "AA:BB:CC:DD:EE:02", -70)]

    class Boom:
        def __enter__(self_):
            raise AmbiguousDeviceError(candidates=cands)
        def __exit__(self_, *a):
            return False

    monkeypatch.setattr(cli, "LedTicker", lambda **kw: Boom())
    monkeypatch.setattr(cli, "_interactive", lambda: False)
    assert cli.main(["power", "on"]) == 1
    err = capsys.readouterr().err
    assert "multiple" in err.lower()
    assert "LED-Ticker-A1B2" in err and "LED-Ticker-9F3C" in err


def test_ambiguous_interactive_prompts_then_connects(monkeypatch, capsys):
    cands = [DeviceInfo("LED-Ticker-A1B2", "AA:BB:CC:DD:EE:01", -50),
             DeviceInfo("LED-Ticker-9F3C", "AA:BB:CC:DD:EE:02", -70)]
    calls = []
    done = {}

    class Boom:
        def __enter__(self_):
            raise AmbiguousDeviceError(candidates=cands)
        def __exit__(self_, *a):
            return False

    class Dev:
        def __enter__(self_):
            return self_
        def __exit__(self_, *a):
            return False
        def set_power(self_, on):
            done["power"] = on

    def factory(**kw):
        calls.append(kw.get("select"))
        return Boom() if len(calls) == 1 else Dev()

    monkeypatch.setattr(cli, "LedTicker", factory)
    monkeypatch.setattr(cli, "_interactive", lambda: True)
    monkeypatch.setattr("builtins.input", lambda *a: "1")
    assert cli.main(["power", "on"]) == 0
    assert calls[0] is None                     # first attempt: no selector
    assert calls[1] == "AA:BB:CC:DD:EE:01"      # retry targets chosen address
    assert done["power"] is True


def test_ambiguous_interactive_bad_choice_exits(monkeypatch, capsys):
    cands = [DeviceInfo("LED-Ticker-A1B2", "AA:BB:CC:DD:EE:01", -50),
             DeviceInfo("LED-Ticker-9F3C", "AA:BB:CC:DD:EE:02", -70)]

    class Boom:
        def __enter__(self_):
            raise AmbiguousDeviceError(candidates=cands)
        def __exit__(self_, *a):
            return False

    monkeypatch.setattr(cli, "LedTicker", lambda **kw: Boom())
    monkeypatch.setattr(cli, "_interactive", lambda: True)
    monkeypatch.setattr("builtins.input", lambda *a: "9")
    assert cli.main(["power", "on"]) == 1
    assert "invalid selection" in capsys.readouterr().out.lower()


def test_reset_confirm_asked_once_on_ambiguous_interactive_path(monkeypatch):
    """Regression: confirmation prompt fires exactly once even when device selection
    triggers the ambiguity path and re-runs the handler."""
    cands = [DeviceInfo("LED-Ticker-A1B2", "AA:BB:CC:DD:EE:01", -50),
             DeviceInfo("LED-Ticker-9F3C", "AA:BB:CC:DD:EE:02", -70)]
    calls = []
    done = {}
    prompts = []

    class Boom:
        def __enter__(self_):
            raise AmbiguousDeviceError(candidates=cands)
        def __exit__(self_, *a):
            return False

    class Dev:
        def __enter__(self_):
            return self_
        def __exit__(self_, *a):
            return False
        def reset(self_):
            done["reset"] = True

    def factory(**kw):
        calls.append(kw.get("select"))
        return Boom() if len(calls) == 1 else Dev()

    def fake_input(prompt=""):
        prompts.append(prompt)
        if "Which device?" in prompt:
            return "1"
        return "y"

    monkeypatch.setattr(cli, "LedTicker", factory)
    monkeypatch.setattr(cli, "_interactive", lambda: True)
    monkeypatch.setattr("builtins.input", fake_input)
    assert cli.main(["reset"]) == 0

    reset_prompts = [p for p in prompts if "Reset all NVS" in p]
    assert len(reset_prompts) == 1, f"confirm asked {len(reset_prompts)} time(s), expected 1"
    assert done.get("reset") is True
