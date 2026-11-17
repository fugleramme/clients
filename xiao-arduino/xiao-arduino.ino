#include <FS.h>
using fs::FS;

#include <CRC32.h>
#include <UrlEncode.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include "driver.h"
#include <TFT_eSPI.h>
#include "gutils.h"

//#define DEBUG

#ifdef DEBUG
  #define DEBUG_BEGIN(x)   Serial.begin(x)
  #define DEBUG_PRINT(x)   Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_BEGIN(x)
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
#endif

#define LOW_BATTERY_WIFI 3.3
#define LOW_BATTERY_WARNING 3.5
#define BATTERY_HYSTERESIS 0.1
#define MAX_RETRIES 3
#define RETRY_DELAY_MS  5000

#define LOW_BATTERY_SLEEP_MINUTES ((uint8_t)60)
#define MIN_SLEEP_MINUTES ((uint8_t)5)

#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */

enum powerMode{ SLEEP, WARN, FULL };

RTC_DATA_ATTR char Etag[40] = "";
RTC_DATA_ATTR powerMode lastMode = FULL;
RTC_DATA_ATTR uint32_t lastErrorHash = 0;

String frameHost = "fugleramme.local";
String framePort = "8080";
String adminPW = "";
String warningMsg = "";
bool useSSL = false;
bool forceUpdate = false;
bool configPortal = false;
bool configReset = false;
bool paramsChanged = false;
bool modeChanged = false;
bool warning = false;
char pendingButton = 0;

Preferences prefs;
WiFiClientSecure *client;
String APPass;
EPaper epaper;

#define KEY0  GPIO_NUM_2 // User Button 1
#define KEY1  GPIO_NUM_3 // User Button 2
#define KEY2  GPIO_NUM_5 // User Button 3
#define BATTERY_ADC A0
#define ADC_EN 6
#define VOLTAGE_DIVIDER_RATIO 2.0

// Server packs using "<4sHBBHHII"
struct __attribute__((__packed__)) Header {
    char     magic[4];  // 4s: 4-character string/bytes
    uint16_t version;   // H : 16-bit unsigned integer
    uint8_t  encoding;  // B : 8-bit unsigned integer
    uint8_t  poll;      // B : 8-bit unsigned integer
    uint16_t width;     // H : 16-bit unsigned integer
    uint16_t height;    // H : 16-bit unsigned integer
    uint32_t length;    // I : 32-bit unsigned integer
    uint32_t crc;       // I : 32-bit unsigned integer
};

bool displayError(String message) {
  DEBUG_PRINTLN(message);

  Etag[0] = 0;
  
  //djb2 hash algorithm
  uint32_t h = 5381;
  for (size_t i = 0; i < message.length(); i++) h = h * 33 + (uint8_t)message[i];
  if (h == lastErrorHash && !modeChanged) return false;   // already on the panel
  lastErrorHash = h;

  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_RED);
  epaper.setTextSize(1);
  epaper.setTextDatum(TC_DATUM);
  epaper.setFreeFont(&FreeSerif24pt7b);
  drawWrapped(epaper, message, 800, 500, 1300, 60);

  return true; // We need to refresh  
}

void prepWarning(String message) {
  DEBUG_PRINTLN(message);

  Etag[0] = 0;
  warning = true;
  warningMsg = message;
}

void displayWarning() {
  epaper.setTextColor(TFT_RED);
  epaper.setTextSize(1);
  epaper.setTextDatum(TL_DATUM);
  epaper.setFreeFont(&FreeSerif12pt7b);
  epaper.fillRoundRect(1180, 980, 400, 200, 10, TFT_WHITE);
  epaper.drawRoundRect(1180, 980, 400, 200, 10, TFT_RED);
  drawWrapped(epaper, warningMsg, 1200, 1000, 360, 32);

  Etag[0] = 0;
  warning = false;
  warningMsg = "";
}

void goToSleep(uint8_t minutes) {
  uint64_t button_mask = ((1ULL << KEY0) | (1ULL << KEY1) | (1ULL << KEY2));

  esp_sleep_enable_ext1_wakeup(button_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup(minutes * 60 * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

// Reads exactly len bytes. Over TLS, read() returns -1 while nothing is
// decrypted yet, so that alone must not end the read.
// Thanks to https://github.com/delta0815 for the fix!
static bool readFully(NetworkClient &s, uint8_t *buf, size_t len, uint32_t timeoutMs = 10000) {
  size_t got = 0;
  uint32_t last = millis();
  while (got < len) {
    int n = s.read(buf + got, len - got);
    if (n > 0) {
      got += n;
      last = millis();
    } else if ((!s.connected() && s.available() <= 0) || millis() - last > timeoutMs) {
      DEBUG_PRINTF("Short read: %u of %u bytes\n", (unsigned)got, (unsigned)len);
      return false;
    } else {
      delay(2);
    }
  }
  return true;
}


bool getFrame(String oldEtag, float battery, uint8_t &poll) {
  HTTPClient http;

  bool refresh = false;
  const char* headerKeys[] = {"Etag"};
  const size_t headerKeysCount = sizeof(headerKeys) / sizeof(headerKeys[0]);
  
  String URL = frameHost + ":" + framePort + "/frame.e6?battery=" + String(battery, 2);

  auto startRequest = [&]() {
    if (useSSL) http.begin(*client, "https://" + URL);
    else        http.begin("http://" + URL);
    http.collectHeaders(headerKeys, headerKeysCount);
    http.setTimeout(15000);
    if (oldEtag != "") http.addHeader("If-None-Match", oldEtag);
  };

  startRequest();
  int httpResponseCode = http.GET();

  for(int attempt = 1; (httpResponseCode == 503 || httpResponseCode < 0) && attempt <= MAX_RETRIES; attempt++) {
    DEBUG_PRINTF("Error %d, retry %d of %d\n", httpResponseCode, attempt, MAX_RETRIES);
    http.end();
    if (client) client->stop();          // clean TLS session on HTTPS
    delay(RETRY_DELAY_MS);
    startRequest();
    httpResponseCode = http.GET();
  }

  switch(httpResponseCode) {
    case 304:
      // No change to display
      DEBUG_PRINTLN("No change to frame.");
      http.end();
      poll = 0;
      return false;
      break;
    case 404:
      http.end();
      poll = 0;
      return displayError("The frame server is not configured to serve external panels. Please enable \"External e-ink panel\" on the admin page's \"Frame\" tab.");
      break;
    case 503:
      http.end();
      poll = 0;
      return displayError("The frame server has not drawn a page for quite a while. Please check the state of the server.");
      break;
    case 200:
      // Go on to display
      break;
    default:
      http.end();
      poll = 0;
      if (httpResponseCode > 0)
        return displayError(String("The frame server answered with HTTP code ") + httpResponseCode);
      DEBUG_PRINTLN("Connection failed");
      return false;
      break;
  }

  String newEtag = http.header("Etag");

  NetworkClient& sock = http.getStream();
  
  // Double-check to ensure the Header struct is as expected.
  static_assert(sizeof(Header) == 20);
  Header header = {};
  char magic[] = {'E','6','F','1'};

  if(readFully(sock, (uint8_t*)&header, sizeof(Header))) {
    if(memcmp(header.magic, magic, 4) == 0 && header.version == 1 && header.encoding == 1) {
      DEBUG_PRINT("Valid page received, length ");
      DEBUG_PRINTLN(header.length);
      if(header.width == 1200 && header.height == 1600 && header.width * header.height / 2 == header.length) {
        DEBUG_PRINTLN("Page is the correct size");
        uint8_t* pixels = (uint8_t*)ps_malloc(header.length);

        if(!pixels) {
          refresh = displayError("Out of memory");
        } else if(!readFully(sock, pixels, header.length)) {
          // Short read: nothing to do
        } else if(CRC32::calculate(pixels, header.length) != header.crc) {
          DEBUG_PRINTLN("Checksum error!");
        } else if(convertBuffer(pixels, header.length, header.width)) {
          DEBUG_PRINTLN("We got a frame!");
          if (newEtag.length() < sizeof(Etag)) strlcpy(Etag, newEtag.c_str(), sizeof(Etag));
          else Etag[0] = 0;   // too long to store: never send a truncated ETag
          lastErrorHash = 0;
          epaper.setRotation(0);
          epaper.pushImage(0, 0, header.width, header.height, (uint16_t *)pixels);
          epaper.setRotation(1);
          refresh = true;
        }

        free(pixels);  // free(NULL) is harmless
      } else {
        refresh = displayError(String("The frame server sent an image of the wrong size. Please set the panel size to 1600 x 1200 on the admin page's \"Frame\" tab."));
      }
    } else {
      DEBUG_PRINTLN("Magic doesn't match.");
    }
  }

  sock.stop();
  if(client) client->stop();
  http.end();

  poll = header.poll;
  return refresh;
}

void displaySetupInfo(WiFiManager *wifi) {
  Etag[0] = 0;
  int qr_width = 0;
  size_t qr_bytes = 0;
  
  String portalIP;
  int portalQRLoc;
  int portalCenter;

  lastErrorHash = 0;

  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_BLACK);
  epaper.setTextSize(4);
  epaper.setFreeFont(&FreeSerifItalic24pt7b);
  epaper.setTextDatum(TC_DATUM);
  epaper.drawString("Fugleramme", 800, 50);

  epaper.setTextSize(1);
  epaper.setFreeFont(&FreeSerif24pt7b);
  epaper.drawString("Welcome to Fugleramme, please follow the setup instructions below", 800, 250);

  epaper.setFreeFont(&FreeSerif12pt7b);
  epaper.drawString("Portal will timeout after 5 minutes.", 800, 310);
  
  if(WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
    portalIP = WiFi.softAPIP().toString();
    drawWrapped(epaper, "To connect to wifi, scan the QR code below or manually connect to the network: " + wifi->getConfigPortalSSID() + " with password " + APPass, 400, 500, 600, 32);
    uint8_t* wifiQR = generateQRBitmap(("WIFI:S:" + wifi->getConfigPortalSSID() + ";T:WPA;P:" + APPass + ";;").c_str(), 8, 10, qr_width, qr_bytes);
    int wifiQRLoc = (800 - qr_width) / 2;
    portalCenter = 1200;
    epaper.drawBitmap(wifiQRLoc, 700, wifiQR, qr_width, qr_width, TFT_BLACK);
    free(wifiQR);
  } else {
    portalIP = WiFi.localIP().toString();
    portalCenter = 800;
  }

  drawWrapped(epaper, "To configure the frame, scan the QR code below, or visit http://" + portalIP + " in your browser and follow the instructions.", portalCenter, 500, 600, 32);
  
  uint8_t* portalQR = generateQRBitmap(("http://" + portalIP).c_str(), 8, 10, qr_width, qr_bytes);
  portalQRLoc = portalCenter - (qr_width / 2);
  epaper.drawBitmap(portalQRLoc, 700, portalQR, qr_width, qr_width, TFT_BLACK);
  
  epaper.update();
  epaper.sleep();
  free(portalQR);
}

void press_button(char button) {
  frameHost = prefs.getString("frameHost", "fugleramme.local");
  framePort = prefs.getString("framePort", "8080");
  useSSL = prefs.getBool("useSSL", false);
  adminPW = prefs.getString("adminPW","");
  HTTPClient http;

  String URL = frameHost + ":" + framePort + "/press";

  if(useSSL) {
    http.begin(*client, "https://" + URL);
  } else {
    http.begin("http://" + URL);
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String data = String("button=") + button;
  if(adminPW.length() > 0) data += "&password=" + urlEncode(adminPW);
  int httpResponseCode = http.POST(data);
  http.end();
  switch(httpResponseCode) {
    case 204:
      Etag[0] = 0;
      delay(5000); // Give frame server time to render new frame
      break;
    case 400:
      DEBUG_PRINTF("Unknown button %c\n", button);
      goToSleep(MIN_SLEEP_MINUTES);
      break;
    case 401:
      prepWarning("Incorrect password. Please hold button 2 for 5 seconds to enter setup mode and configure admin password.");
      break;
    case 429:
      prepWarning("Too many incorrect password attempts. This device has been locked out from button presses.");
      break;
    case 404:
      // This is going to just get handled by the main frame get
      break;
    default:
      DEBUG_PRINTF("Unknown response from panel: %d\n", httpResponseCode);
      goToSleep(MIN_SLEEP_MINUTES);
      break;
  }
}

void handle_wakeup() {
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();

  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT1:
      {
        pinMode(KEY0, INPUT_PULLUP);
        pinMode(KEY1, INPUT_PULLUP);
        pinMode(KEY2, INPUT_PULLUP);

        uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
        if (wakeup_pin_mask & (1ULL << KEY0)) {
          DEBUG_PRINTLN("Wakeup caused by Button 1");
          configReset = true;
          for(int i = 0; i < 10; i++) {
            if(digitalRead(KEY0) == HIGH) {
              pendingButton = 'C';
              configReset = false;
              break;
            }
            delay(1000);
          }
        }
        if (wakeup_pin_mask & (1ULL << KEY1)) {
          DEBUG_PRINTLN("Wakeup caused by Button 2");
          configPortal = true;
          for(int i = 0; i < 5; i++) {
            if(digitalRead(KEY1) == HIGH) {
              pendingButton = 'B';
              configPortal = false;
              break;
            }
            delay(1000);
          }
        }          
        if (wakeup_pin_mask & (1ULL << KEY2)) {
          DEBUG_PRINTLN("Wakeup caused by Button 3");
          pendingButton = 'A';
        }
        break;
      }
    case ESP_SLEEP_WAKEUP_TIMER:
      DEBUG_PRINTLN("Wakeup caused by timer");
      break;
    default:
      DEBUG_PRINTF("Wakeup was not caused by deep sleep: %d\n", wakeup_reason);
      forceUpdate = true;
      break;
  }
}

void saveParams() {
  paramsChanged = true;
  configPortal = false;
  forceUpdate = true;
}

void portalTimeout() {
  // Only if portal was from autoconnect timeout
  if(!configPortal) {
    if(displayError("Setup incomplete, panel is going to sleep. To restart setup, press reset button.")) {
      WiFi.disconnect(true);   // true = also turn the radio off
      WiFi.mode(WIFI_OFF);
      epaper.update();
      epaper.sleep();
    }
    // Sleep forever until restarted
    esp_deep_sleep_start();
  }
}

float readBatteryVoltage() {
    analogReadResolution(12);
    pinMode(BATTERY_ADC, INPUT);
    pinMode(ADC_EN, OUTPUT);
    digitalWrite(ADC_EN, HIGH);

    delay(15);

    uint32_t sum = 0;
    for(int i = 0; i < 16; i++) {
        sum += analogReadMilliVolts(BATTERY_ADC);
        delay(1);
    }
    digitalWrite(ADC_EN, LOW);
    return sum * VOLTAGE_DIVIDER_RATIO / 16 / 1000.0;
}

powerMode modeForVoltage(float v, powerMode last) {
  // Going up requires clearing the threshold by the margin.
  // Staying put or going down uses the plain threshold.
  float warnAt = LOW_BATTERY_WARNING + (last < FULL ? BATTERY_HYSTERESIS : 0);
  float wifiAt = LOW_BATTERY_WIFI    + (last == SLEEP ? BATTERY_HYSTERESIS : 0);

  if (v >= warnAt) return FULL;
  if (v >= wifiAt) return WARN;
  return SLEEP;
}

void drawLowBattery(uint32_t color) {
    epaper.fillRoundRect(1128, 30, 42, 100, 3, color);
    epaper.fillRoundRect(1130, 32, 38, 96, 3, TFT_WHITE);
    epaper.fillRoundRect(1133, 35, 32, 10, 2, color);
    epaper.fillRoundRect(1138, 128, 22, 10, 2, color);
}

void drawNoBattery(uint32_t color) {
    drawLowBattery(color);
    epaper.drawWideLine(1113, 30, 1185, 130, 2, color);
}

void drawNoWifi(uint32_t color) {
    int centerX = 1140;
    int centerY = 1540;
    epaper.fillCircle(centerX, centerY, 5, color);
    epaper.drawSmoothArc(centerX, centerY, 15, 10, 220, 320, color, TFT_WHITE, true);
    epaper.drawSmoothArc(centerX, centerY, 25, 20, 220, 320, color, TFT_WHITE, true);
    epaper.drawSmoothArc(centerX, centerY, 35, 30, 220, 320, color, TFT_WHITE, true);
    epaper.drawWideLine(1140, 1520, 1172, 1562, 5, color);
}

String generateRandomPassword() {
    String password = "";
    password.reserve(8); // Allocate memory upfront to prevent heap fragmentation
    
    uint8_t randomBytes[8];
    esp_fill_random(randomBytes, 8); // Fetch hardware random bytes

    for (int i = 0; i < 8; i++) {
        uint8_t idx = randomBytes[i] % 62; // 10 digits + 26 upper + 26 lower
        
        if (idx < 10) {
            password += (char)('0' + idx);
        } else if (idx < 36) {
            password += (char)('A' + (idx - 10));
        } else {
            password += (char)('a' + (idx - 36));
        }
    }
    
    return password;
}

void setup() {
  DEBUG_BEGIN(115200);
  #ifdef DEBUG
    while (!Serial) {
      ;  // wait for serial port to connect. Needed for native USB port only
    }
  #endif

  prefs.begin("BirdFrame");

  handle_wakeup();

  epaper.begin();

  DEBUG_PRINTF("rotation-0  Width: %d, Height: %d\n", epaper.width(), epaper.height());
  epaper.setRotation(1);
  DEBUG_PRINTF("rotation-1  Width: %d, Height: %d\n", epaper.width(), epaper.height());
  epaper.sleep();

  uint8_t sleepMin = MIN_SLEEP_MINUTES;
  float battery = readBatteryVoltage();
  powerMode currentMode = modeForVoltage(battery, lastMode);
  modeChanged = (currentMode != lastMode);  

  // Force a fetch before drawing a status icon
  if (modeChanged) forceUpdate = true;

  DEBUG_PRINTF("Battery voltage is %.2f\n",battery);

  bool need_refresh = false;
    
  // Skip all the processing and go back to sleep if we've been below Wifi threshold twice in a row
  if(!(currentMode == SLEEP && lastMode == SLEEP)) {
    WiFiManager wm;
    
    if(configReset) {
      wm.resetSettings();
      prefs.clear();
      Etag[0] = 0;
    } else {
      frameHost = prefs.getString("frameHost", "fugleramme.local");
      framePort = prefs.getString("framePort", "8080");
      useSSL = prefs.getBool("useSSL", false);
      adminPW = prefs.getString("adminPW","");
    }
  
    // Initialize WiFi STA mode first so NVS config loads properly
    WiFi.mode(WIFI_STA);

    WiFiManagerParameter fuglerammeHost("frameHost", "Fugleramme Host", frameHost.c_str(), 128);
    WiFiManagerParameter fuglerammePort("framePort", "Fugleramme Port", framePort.c_str(), 5);
    const char* checkBoxHtml = useSSL ? "type=\"checkbox\" checked" : "type=\"checkbox\"";
    WiFiManagerParameter SSL("useSSL", "Use HTTPS", "F", 2, checkBoxHtml, WFM_LABEL_AFTER);
    WiFiManagerParameter fuglerammeAdminPW("adminPW", "Fugleramme Admin Password (Leave blank for no password)", adminPW.c_str(), 128, "type=\"password\"");
    WiFiManagerParameter br("<br/>");
    wm.setCustomHeadElement(
        "<script>"
        "document.addEventListener('DOMContentLoaded', function() {"
        "  var buttons = document.getElementsByTagName('button');"
        "  for (var i = 0; i < buttons.length; i++) {"
        "    if (buttons[i].textContent === 'Configure WiFi') {"
        "      buttons[i].textContent = 'Configure Settings';"
        "    }"
        "  }"
        "});"
        "</script>"
      );
    wm.setAPCallback(displaySetupInfo);
    wm.setSaveParamsCallback(saveParams);
    wm.setConfigPortalTimeoutCallback(portalTimeout);
    wm.setConnectTimeout(20);
    wm.setConfigPortalTimeout(300);
    wm.addParameter(&fuglerammeHost);
    wm.addParameter(&fuglerammePort);
    wm.addParameter(&SSL);
    wm.addParameter(&br);
    wm.addParameter(&br);
    wm.addParameter(&fuglerammeAdminPW);
    wm.setEnableConfigPortal(configPortal || !wm.getWiFiIsSaved());

    APPass = generateRandomPassword();

    if(!wm.autoConnect("Fugleramme-Setup",APPass.c_str())) {
      goToSleep(currentMode < FULL ? LOW_BATTERY_SLEEP_MINUTES : MIN_SLEEP_MINUTES);
    }

    if(configPortal){
      Etag[0] = 0;
      forceUpdate = true;
      
      wm.startWebPortal();
      displaySetupInfo(&wm);
      uint32_t deadline = millis() + 5UL * 60 * 1000;
      while (configPortal && millis() < deadline) { wm.process(); delay(1); }
      wm.stopWebPortal();
    }

    if(paramsChanged) {
      frameHost = fuglerammeHost.getValue();
      frameHost.trim();
      framePort = fuglerammePort.getValue();
      framePort.trim();
      useSSL = (String(SSL.getValue()).length() > 0);
      adminPW = fuglerammeAdminPW.getValue();

      prefs.putString("frameHost", frameHost);
      prefs.putString("framePort", framePort);
      prefs.putBool("useSSL", useSSL);
      prefs.putString("adminPW", adminPW);
    } else {
      frameHost = prefs.getString("frameHost", "fugleramme.local");
      framePort = prefs.getString("framePort", "8080");
      useSSL = prefs.getBool("useSSL", false);
      adminPW = prefs.getString("adminPW","");
    }

    DEBUG_PRINTLN("Loaded frame host " + frameHost + ":" + framePort);

    if(useSSL) {
      client = new WiFiClientSecure;
      client->setInsecure();
    }

    if (pendingButton)
      press_button(pendingButton);

    String oldEtag = forceUpdate ? String("") : String(Etag);

    need_refresh = getFrame(oldEtag, battery, sleepMin);

    // Network is done: shut the radio down before e-paper update
    WiFi.disconnect(true);   // true = also turn the radio off
    WiFi.mode(WIFI_OFF);

    if(warning && need_refresh)   displayWarning();
    if(currentMode == SLEEP)      drawNoBattery(TFT_RED);
    else if(currentMode < FULL)   drawLowBattery(TFT_RED);    

    if(need_refresh) {
      epaper.update();
      epaper.sleep();
    }
  }

  lastMode = (currentMode != lastMode && !need_refresh) ? lastMode : currentMode;
  
  uint8_t sleepTime;
  if(currentMode < FULL)
    sleepTime = LOW_BATTERY_SLEEP_MINUTES;
  else
    sleepTime = max(sleepMin,MIN_SLEEP_MINUTES);

  goToSleep(sleepTime);
}

void loop() {
// Zzzzzzzzzzz
}
