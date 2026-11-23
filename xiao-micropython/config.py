# Wi-Fi networks to join, in the order they should be tried: the first one
# that connects wins. Add one by appending an (ssid, password) pair.
WIFI_NETWORKS = (("your-network", "your-password"),)
# The network that connected last time, tried first on the next wake.
WIFI_HINT = "wifi"

# Change to your Fugleramme setup
FRAME_HOST = "fugleramme.local"
FRAME_PORT = 8080
# The frame is served over TLS on 443; set True for HTTPS on another port.
# Note: we don't enable host validation, just encryption to avoid having to store a certificate store
FRAME_TLS = FRAME_PORT == 443

# Set FIRMWARE to a URL serving the panel's latest program to enable OTA
# updates. The URL must return the file directly, with no authentication or
# redirect, for example the raw main.py from the main branch on GitHub:
# https://raw.githubusercontent.com/fugleramme/clients/main/xiao-micropython/main.py
# None disables OTA.
FIRMWARE = None

# The minimum sleep duration between two polls
MIN_SLEEP_MINUTES = 5

# Failed wakes in a row before an unreachable frame is worth a refresh.
NOTICE_AFTER = 3

# The panel's size picks its driver: 1200, 1600 for the 13.3" on a XIAO
# EE02, 800, 480 for the 7.3" on a XIAO EE04.
WIDTH, HEIGHT = 1200, 1600
