# XIAO EE02 and EE04, MicroPython

A MicroPython program for the [XIAO EE02](https://www.seeedstudio.com/XIAO-ePaper-Display-Board-ESP32-S3-EE02-p-6639.html) and the XIAO EE04, which run from a battery and can hang anywhere in Wi-Fi range. It wakes for the interval the page's header carries in its **Panel refresh** setting. So a 10 minute setting has it fetch every 10 minutes, and it never fetches more often than its own 5 minute floor, even when **Panel refresh** is off. It downloads the page only if it has changed, and redraws the panel only once the download checks out. If something goes wrong, it writes an error message on the panel instead.

Contributed by [Lorenz Schmid](https://github.com/lorenzschmid) and extended by [Grégoire Seux](https://github.com/kamaradclimber). EE04 support contributed by [tcole333](https://github.com/tcole333).

## Hardware

- [XIAO EE02](https://www.seeedstudio.com/XIAO-ePaper-Display-Board-ESP32-S3-EE02-p-6639.html)
- [Seeed Studio 13.3" Spectra 6 panel](https://www.seeedstudio.com/13-3inch-Six-Color-eInk-ePaper-Display-with-1200x1600-Pixels-p-6569.html)

or

- XIAO EE04
- Seeed Studio 7.3" Spectra 6 panel

## Install and configure

On Fugleramme's admin page, enable **External e-ink panel** under **Frame** and set **External panel size** to the panel's size. Then:

1. Install MicroPython on the board (e.g. [for the XIAO ESP32S3](https://micropython.org/download/SEEED_XIAO_ESP32S3/))
2. Set your Wi-Fi credentials, Fugleramme host and panel size (`WIDTH, HEIGHT`) in `config.py`. A frame served over HTTPS on port 443 is fetched over TLS.
3. Upload `main.py`, `boot.py` and `config.py` to the board (e.g. with [mpremote](https://docs.micropython.org/en/latest/reference/mpremote.html))

## Update

`boot.py` runs on every boot before `main.py`. It is what lets the panel update its own program: set `FIRMWARE` in `config.py` to a URL serving the panel's latest program (as plain file, no sign-in or redirect), and the panel checks it on its wakes. A new program is staged, and after a restart it gets one trial wake; if the panel reaches the frame cleanly on it, the update is kept, otherwise the board goes back to the program it had before. Leave `FIRMWARE` at `None` and nothing is downloaded.

To follow this folder, set `FIRMWARE` to `https://raw.githubusercontent.com/fugleramme/clients/main/xiao-micropython/main.py`.

## Tested on

A 13.3" panel with a XIAO EE02. The 7.3" panel with a XIAO EE04 is not yet tested with this shared program.
