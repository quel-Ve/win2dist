"""
Window2Clear — Windows window transparency controller.

Core:
    transparency.py    — SetLayeredWindowAttributes wrapper (proven, stable)
    config.py          — config.ini read/write
    tray_app.py        — System tray + global hotkeys + settings dialog

Experimental (DWM backdrop — limited window support):
    blur.py            — SetWindowCompositionAttribute CLI
    blur_test.py       — Interactive blur mode test on real windows
    frosted_demo.py    — Pure Win32 frosted glass demo window
    acrylic_service.py — Background blur-on-settle service

Utilities:
    window_utils.py    — Window enumeration (find_by_title, list_all, get_title)
"""
