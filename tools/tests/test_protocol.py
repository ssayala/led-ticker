import pytest
import led_ticker.protocol as P
from led_ticker.errors import ProtocolError, ValidationError


def test_encode_tickers_uppercases_and_joins():
    assert P.encode_tickers([" aapl ", "Msft"]) == b"AAPL,MSFT"


def test_encode_tickers_empty_rejected():
    with pytest.raises(ValidationError):
        P.encode_tickers([" ", ""])


def test_encode_status_minutes_to_seconds():
    assert P.encode_status("BUSY", 30) == b"BUSY|1800"
    assert P.encode_status("ON AIR") == b"ON AIR|0"


def test_encode_status_rejects_pipe_and_negative():
    with pytest.raises(ValidationError):
        P.encode_status("a|b")
    with pytest.raises(ValidationError):
        P.encode_status("x", -1)


def test_encode_status_rejects_bool_minutes():
    # bool is an int subclass; must be explicitly rejected.
    with pytest.raises(ValidationError):
        P.encode_status("x", True)


def test_parse_status_variants():
    assert P.parse_status("") is None
    s = P.parse_status("BUSY|1800")
    assert (s.text, s.seconds, s.indefinite) == ("BUSY", 1800, False)
    assert P.parse_status("ON AIR|0").indefinite is True
    assert P.parse_status("legacy").text == "legacy"
    with pytest.raises(ProtocolError):
        P.parse_status("BUSY|notanint")


def test_display_round_trip_and_ranges():
    assert P.encode_display(8, 50) == b"8|50"
    d = P.parse_display("8|50")
    assert (d.brightness, d.scroll_ms) == (8, 50)
    assert P.parse_display("") is None
    with pytest.raises(ProtocolError):
        P.parse_display("garbage")
    with pytest.raises(ValidationError):
        P.encode_display(16, 50)
    with pytest.raises(ValidationError):
        P.encode_display(8, 19)


def test_encode_wifi_and_pipe_guard():
    assert P.encode_wifi("My Net", "pw|x") == b"My Net|pw|x"
    with pytest.raises(ValidationError):
        P.encode_wifi("ssid|bad", "pw")


def test_validate_location_good_and_bad():
    assert P.validate_location("47.61,-122.33,Seattle") == "47.61,-122.33,Seattle"
    for bad in ["47.61,Seattle", "abc,1,Here", "91,0,TooNorth", "1,2,"]:
        with pytest.raises(ValidationError):
            P.validate_location(bad)


def test_encode_locations_join_and_caps():
    assert P.encode_locations(["47.61,-122.33,Seattle", " "]) == b"47.61,-122.33,Seattle"
    with pytest.raises(ValidationError):
        P.encode_locations([])
    too_many = ["1,1,LocationLabelHere"] * 20
    with pytest.raises(ValidationError):
        P.encode_locations(too_many)


def test_parse_locations():
    assert P.parse_locations("") == []
    assert P.parse_locations("a|b") == ["a", "b"]


def test_location_length_exact_boundary():
    # MAX_LOCATION_LEN = 48; guard is >= 48, so 47 passes and 48 raises.
    prefix = "1,1,"  # 4 bytes
    label_43 = "A" * 43  # 4 + 43 = 47 bytes exactly
    label_44 = "A" * 44  # 4 + 44 = 48 bytes exactly
    entry_47 = prefix + label_43
    entry_48 = prefix + label_44
    assert len(entry_47.encode()) == 47
    assert len(entry_48.encode()) == 48
    assert P.validate_location(entry_47) == entry_47  # passes
    with pytest.raises(ValidationError):
        P.validate_location(entry_48)  # raises at boundary


def test_locations_payload_exact_boundary():
    # MAX_LOCATIONS_PAYLOAD = 245; guard is >= 245, so 244 passes and 245 raises.
    # 35 entries of '1,2,XX' (6 bytes each), joined by | -> 35*6 + 34 = 244 bytes.
    entries_244 = ["1,2,XX"] * 35
    payload_244 = "|".join(entries_244)
    assert len(payload_244.encode()) == 244
    P.encode_locations(entries_244)  # should not raise
    # 34 entries of '1,2,XX' + 1 of '1,2,XXX' -> 34*6 + 7 + 34 = 245 bytes.
    entries_245 = ["1,2,XX"] * 34 + ["1,2,XXX"]
    payload_245 = "|".join(entries_245)
    assert len(payload_245.encode()) == 245
    with pytest.raises(ValidationError):
        P.encode_locations(entries_245)  # raises at boundary


def test_normalize_mode():
    assert P.normalize_mode(["all"]) == "all"
    assert P.normalize_mode(["none"]) == "none"
    assert P.normalize_mode(["stocks", "stocks", "clock"]) == "stocks,clock"
    with pytest.raises(ValidationError):
        P.normalize_mode(["bogus"])
    with pytest.raises(ValidationError):
        P.normalize_mode([])


def test_validators():
    assert P.validate_pin(" 482913 ") == "482913"
    for bad in ["12345", "abcdef", "1234567"]:
        with pytest.raises(ValidationError):
            P.validate_pin(bad)
    assert P.validate_timezone("PST8PDT,M3.2.0,M11.1.0").startswith("PST")
    with pytest.raises(ValidationError):
        P.validate_timezone("5IST")  # must start with a letter
    assert P.validate_timer_minutes(10) == 10
    with pytest.raises(ValidationError):
        P.validate_timer_minutes(0)
    with pytest.raises(ValidationError):
        P.validate_timer_minutes(100)
