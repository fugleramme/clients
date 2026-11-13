# Good Display GDEP133C02, ESP-IDF

An ESP-IDF program for the [Good Display GDEP133C02](https://www.good-display.com/product/559.html), a 13.3" Spectra 6 panel on an ESP32-S3. It stays on the network and fetches for the interval the page's header carries in its **Panel refresh** setting. So a 10 minute setting has it fetch every 10 minutes, and it never fetches more often than its own 5 minute floor, even when **Panel refresh** is off. It downloads the page only if it has changed, and redraws the panel only once the download checks out.

Contributed by [Lucas Ault](https://github.com/lucas-ault).

## Hardware

- ESP32-S3 with octal PSRAM, 16 MB flash, USB-Serial/JTAG
- [Good Display GDEP133C02](https://www.good-display.com/product/559.html), 1200×1600, six colors

SPI is on GPIO 9 (clock), 41 (data 0), and 40 (data 1). Chip selects are GPIO 18 and 17, reset is GPIO 6, busy is GPIO 7, and the panel load switch is GPIO 45.

## Install and configure

On Fugleramme's admin page, enable **External e-ink panel** under **Frame** and set **External panel size** to 13.3". Then:

1. Install [ESP-IDF 6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html) and export it (`source /path/to/esp-idf/export.sh`).
2. Build and flash. `PORT` is the board's USB-Serial/JTAG device, usually `/dev/cu.usbmodem*` on macOS and `/dev/ttyACM*` on Linux.

```bash
cd gdep133c02-esp-idf
idf.py build
idf.py -p PORT flash monitor
```

`idf.py flash` keeps NVS. A network already saved under the `wifi` namespace (`ssid` and `pass`) is reused. The monitor prints `wifi connected, ip ...` and `settings at http://<ip>/`.

Open that address. The page scans for networks, joins one, and stores the Fugleramme host, port, and whether to use HTTPS. **Save and refresh** and **Refresh now** both store those settings and fetch immediately. The host defaults to `fugleramme.local` on port 8080. Set a name or an IP, with no `http://`. A frame served over HTTPS is fetched over TLS, without checking the certificate.

If NVS has no network, or the saved one does not connect within 20 seconds, the board opens a setup network:

- SSID `panel-setup`
- Password `panel-setup`
- Page at `http://192.168.4.1/`

That page is the same settings page. When the station gets an address, the setup network closes. `idf.py erase-flash` wipes the saved network and the frame address, and the next boot opens the setup network again.

## Update

Build and flash again with `idf.py flash`. Wi-Fi and the Fugleramme address stay in NVS. `idf.py erase-flash` clears them.

## Tested on

An ESP32-S3 with a Good Display GDEP133C02. The setup page joined Wi-Fi, then the panel pulled a 1200×1600 `/frame.e6` from Fugleramme on port 8080. The checksum matched and the glass refreshed.
