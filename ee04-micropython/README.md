# XIAO EE04, MicroPython

The [`ee02-micropython`](../ee02-micropython/) program on a XIAO EE04 with a Seeed Studio 7.3" Spectra 6 panel. Both boards run the same `main.py` and `boot.py`. Only this folder's `config.py` differs: its `WIDTH, HEIGHT = 800, 480` tells the program it drives the 7.3". Wakes, updates, buttons, the LED and error messages work as described there.

Contributed by [tcole333](https://github.com/tcole333).

## Hardware

- XIAO EE04
- Seeed Studio 7.3" Spectra 6 panel

## Install and configure

On Fugleramme's admin page, enable **External e-ink panel** under **Frame** and set **External panel size** to 7.3". Then:

1. Install MicroPython on the board (e.g. [for the XIAO ESP32S3](https://micropython.org/download/SEEED_XIAO_ESP32S3/))
2. Set your Wi-Fi credentials and Fugleramme host in this folder's `config.py`. A frame served over HTTPS on port 443 is fetched over TLS.
3. Upload `main.py` and `boot.py` from [`ee02-micropython`](../ee02-micropython/) and `config.py` from this folder to the board (e.g. with [mpremote](https://docs.micropython.org/en/latest/reference/mpremote.html))

## Update

To follow the program, set `FIRMWARE` to `https://raw.githubusercontent.com/fugleramme/clients/main/ee02-micropython/main.py`. See [`ee02-micropython`](../ee02-micropython/README.md#update) for how updates are installed and rolled back.

## Tested on

Not yet tested on a 7.3" panel with this shared program.
