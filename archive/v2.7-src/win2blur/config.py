"""
Config file management for Window2Clear.
Reads/writes ./config.ini (same format as original window2clear.exe v0.2.0).
"""
import configparser
from pathlib import Path

CONFIG_PATH = Path(__file__).parent.parent / "config.ini"

DEFAULTS = {
    "Settings": {
        "TransparencyStep": "5",
        "DefaultOpacity": "90",
    },
    "Hotkeys": {
        "TransparencyUpModifiers": "1",       # ALT
        "TransparencyUpKey": "37",            # VK_LEFT
        "TransparencyDownModifiers": "1",     # ALT
        "TransparencyDownKey": "39",          # VK_RIGHT
        "TransparencyToggleModifiers": "1",   # ALT
        "TransparencyToggleKey": "38",        # VK_UP
        "AcrylicToggleModifiers": "1",        # ALT
        "AcrylicToggleKey": "40",             # VK_DOWN
    },
    "Switches": {
        "EnableTransparencyUp": "1",
        "EnableTransparencyDown": "1",
        "EnableTransparencyToggle": "1",
        "EnableAcrylicToggle": "1",
    },
}


def load() -> configparser.ConfigParser:
    """Load config, creating with defaults if missing."""
    cfg = configparser.ConfigParser()
    cfg.read_dict(DEFAULTS)
    if CONFIG_PATH.exists():
        cfg.read(str(CONFIG_PATH), encoding="utf-8")
    return cfg


def save(cfg: configparser.ConfigParser):
    """Write config to disk."""
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        cfg.write(f)
