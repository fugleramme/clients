#include <FS.h>
using fs::FS;

#include <CRC32.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include "gutils.h"
#include "driver.h"
#include <TFT_eSPI.h>

#define LOW_BATTERY_WIFI 3.3
#define LOW_BATTERY_WARNING 3.5
#define BATTERY_HYSTERESIS 0.1


#define LOW_BATTERY_SLEEP_MINUTES ((uint8_t)60)
#define MIN_SLEEP_MINUTES ((uint8_t)5)

#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */

enum powerMode{ SLEEP, WARN, FULL };

RTC_DATA_ATTR char Etag[33] = "";
RTC_DATA_ATTR powerMode lastMode = FULL;

Preferences prefs;
bool paramsChanged = false;
String frameHost = "fugleramme.local";
String framePort = "8080";
bool useSSL = false;
bool forceUpdate = false;
bool configPortal = false;
bool configReset = false;
char pendingButton = 0;
WiFiClientSecure *client;
String APPass;

EPaper epaper;

#define KEY0  GPIO_NUM_2 // User Button 1
#define KEY1  GPIO_NUM_3 // User Button 2
#define KEY2  GPIO_NUM_5 // User Button 3
#define BATTERY_ADC A0
#define ADC_EN 6
#define VOLTAGE_DIVIDER_RATIO 2.0

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

void displayError(String message, bool persistent = false) {
  // Not yet implemented, just print the error
  Serial.println(message);
}

void goToSleep(uint8_t minutes) {
  uint64_t button_mask = ((1ULL << KEY0) | (1ULL << KEY1) | (1ULL << KEY2));

  esp_sleep_enable_ext1_wakeup(button_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup(minutes * 60 * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

bool getFrame(String oldEtag, float battery, uint8_t &poll) {
  HTTPClient http;

  bool refresh = false;
  const char* headerKeys[] = {"Etag"};
  size_t headerKeysCount = sizeof(headerKeys) / sizeof(headerKeys[0]);
  http.collectHeaders(headerKeys, headerKeysCount);

  String URL = frameHost + ":" + framePort + "/frame.e6?battery=" + String(battery, 2);

  if(useSSL) {
    http.begin(*client, "https://" + URL);
  } else {
    http.begin("http://" + URL);
  }

  if(oldEtag != "") {
    http.addHeader("If-None-Match",oldEtag);
  }

  http.setTimeout(15000);
  int httpResponseCode = http.GET();

  switch(httpResponseCode) {
    case 304:
      // No change to display
      Serial.println("No change to frame.");
      http.end();
      poll = 0;
      return false;
      break;
    case 404:
      displayError("The frame is not configured to serve external panels. Please enable \"External e-ink panel\" at on the admin page's \"Frame\" tab.",true);
      http.end();
      poll = 0;
      return false;
      break;
    case 503:
      displayError("The frame has not drawn a page yet.");
      http.end();
      poll = 0;
      return false;
      break;
    case 200:
      // Go on to display
      break;
    default:
      displayError("The frame answered with HTTP code " + String(httpResponseCode));
      http.end();
      poll = 0;
      return false;
      break;
  }

  String newEtag = http.header("Etag");

  NetworkClient& sock = http.getStream();
  Header header = {};
  char magic[] = {'E','6','F','1'};

  if(sock.readBytes((uint8_t*)&header, sizeof(Header)) == sizeof(Header)) {
    if(memcmp(header.magic, magic, 4) == 0 && header.version == 1 && header.encoding == 1) {
      Serial.print("Valid page received, length ");
      Serial.println(header.length);
      if(header.width == 1200 && header.height == 1600 && header.width * header.height / 2 == header.length) {
        Serial.println("Page is the correct size");
        uint8_t* pixels = (uint8_t*)ps_malloc(header.length);

        if(!pixels) {
          displayError("Out of memory");
        } else if(sock.readBytes(pixels, header.length) != header.length) {
          // Short read: nothing to do
        } else if(CRC32::calculate(pixels, header.length) != header.crc) {
          Serial.println("Checksum error!");
        } else if(convertBuffer(pixels, header.length, header.width)) {
          Serial.println("We got a frame!");
          if (newEtag.length() < sizeof(Etag)) strlcpy(Etag, newEtag.c_str(), sizeof(Etag));
          else Etag[0] = 0;   // too long to store: never send a truncated ETag
          epaper.setRotation(0);
          epaper.pushImage(0, 0, header.width, header.height, (uint16_t *)pixels);
          epaper.setRotation(1);
          refresh = true;
        }

        free(pixels);  // free(NULL) is harmless
      } else {
        Serial.println("Wrong page size.");
      }
    } else {
      Serial.println("Magic doesn't match.");
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
  int portalTextLoc;

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
  epaper.setTextDatum(TL_DATUM);

  if(WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
    portalIP = WiFi.softAPIP().toString();
    epaper.drawString("To connect to wifi, scan the QR code below", 200, 500);
    epaper.drawString("or manually connect to the network:", 200, 530);
    epaper.drawString(wifi->getConfigPortalSSID() + " with password " + APPass, 200, 560);
    uint8_t* wifiQR = generateQRBitmap(("WIFI:S:" + wifi->getConfigPortalSSID() + ";T:WPA;P:" + APPass + ";;").c_str(), 8, 10, qr_width, qr_bytes);
    int wifiQRLoc = (800 - qr_width) / 2;
    portalQRLoc = 800 + wifiQRLoc;
    portalTextLoc = 1000;
    epaper.drawBitmap(wifiQRLoc, 700, wifiQR, qr_width, qr_width, TFT_BLACK);
    free(wifiQR);
  } else {
    portalIP = WiFi.localIP().toString();
    portalQRLoc = 750 - (qr_width / 2);
    portalTextLoc = 550;
  }

  epaper.drawString("To configure the frame, scan the QR code below, or visit", portalTextLoc, 500);
  epaper.drawString("http://" + portalIP, portalTextLoc, 530);
  epaper.drawString("in your browser and follow the instructions.", portalTextLoc, 560);
  
  uint8_t* portalQR = generateQRBitmap(("http://" + portalIP).c_str(), 8, 10, qr_width, qr_bytes);
  epaper.drawBitmap(portalQRLoc, 700, portalQR, qr_width, qr_width, TFT_BLACK);
  
  epaper.update();
  epaper.sleep();
  free(portalQR);
}

void press_button(char button) {
  frameHost = prefs.getString("frameHost", "fugleramme.local");
  framePort = prefs.getString("framePort", "8080");
  useSSL = prefs.getBool("useSSL", false);
  HTTPClient http;

  String URL = frameHost + ":" + framePort + "/press";

  if(useSSL) {
    http.begin(*client, "https://" + URL);
  } else {
    http.begin("http://" + URL);
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpResponseCode = http.POST("button=" + String(button));
  http.end();
  if(httpResponseCode == 204)
    delay(5000); // Give frame server time to render new frame
  else
    goToSleep(MIN_SLEEP_MINUTES);
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
          Serial.println("Wakeup caused by Button 1");
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
          Serial.println("Wakeup caused by Button 2");
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
          Serial.println("Wakeup caused by Button 3");
          pendingButton = 'A';
        }
        break;
      }
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wakeup caused by timer");
      break;
    default:
      Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason);
      forceUpdate = true;
      break;
  }
}

void saveParams() {
  paramsChanged = true;
  configPortal = false;
  forceUpdate = true;
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
    epaper.drawWideLine(1113, 30, 1185, 130, 2, color);
    drawLowBattery(color);
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
  Serial.begin(115200);
  delay(3000);
  
  prefs.begin("BirdFrame");

  handle_wakeup();

  epaper.begin();
  epaper.setRotation(1);
  epaper.sleep();

  uint8_t sleepMin = MIN_SLEEP_MINUTES;
  float battery = readBatteryVoltage();
  //powerMode lastMode = static_cast<powerMode>(prefs.getUChar("powerMode",static_cast<uint8_t>(FULL)));
  powerMode currentMode = modeForVoltage(battery, lastMode);

  // Force a fetch before drawing a status icon
  if (currentMode != lastMode) forceUpdate = true;

  Serial.printf("Battery voltage is %.2f\n",battery);

  bool need_refresh = false;
    
  // Skip all the processing and go back to sleep if we've been below Wifi threshold twice in a row
  if(!(currentMode == SLEEP && lastMode == SLEEP)) {
    WiFiManager wm;
    
    if(configReset) {
      wm.resetSettings();
      prefs.clear();
    } else {
      frameHost = prefs.getString("frameHost", "fugleramme.local");
      framePort = prefs.getString("framePort", "8080");
      useSSL = prefs.getBool("useSSL", false);
    }

    WiFiManagerParameter fuglerammeHost("frameHost", "Fugleramme Host", frameHost.c_str(), 128);
    WiFiManagerParameter fuglerammePort("framePort", "Fugleramme Port", framePort.c_str(), 5);
    const char* checkBoxHtml = useSSL ? "type=\"checkbox\" checked" : "type=\"checkbox\"";
    WiFiManagerParameter SSL("useSSL", "USE HTTPS", "F", 2, checkBoxHtml);
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
    wm.setConnectTimeout(20);
    wm.setConfigPortalTimeout(300);
    wm.addParameter(&fuglerammeHost);
    wm.addParameter(&fuglerammePort);
    wm.addParameter(&SSL);
    //wm.setEnableConfigPortal(configPortal || !wm.getWiFiIsSaved());

    APPass = generateRandomPassword();

    if(!wm.autoConnect("Fugleramme Setup",APPass.c_str())) {
      lastMode = (currentMode != lastMode && !need_refresh) ? lastMode : currentMode;
      goToSleep(currentMode < FULL ? LOW_BATTERY_SLEEP_MINUTES : MIN_SLEEP_MINUTES);
    }

    if(configPortal){
      //prefs.putString("Etag","");
      Etag[0] = 0;
      
      wm.startWebPortal();
      displaySetupInfo(&wm);
      uint32_t deadline = millis() + 5UL * 60 * 1000;
      while (configPortal && millis() < deadline) { wm.process(); delay(1); }
      wm.stopWebPortal();
    }

    if(paramsChanged) {
      frameHost = fuglerammeHost.getValue();
      framePort = fuglerammePort.getValue();
      useSSL = (String(SSL.getValue()).length() > 0);

      prefs.putString("frameHost", frameHost);
      prefs.putString("framePort", framePort);
      prefs.putBool("useSSL", useSSL);
    } else {
      frameHost = prefs.getString("frameHost", "fugleramme.local");
      framePort = prefs.getString("framePort", "8080");
      useSSL = prefs.getBool("useSSL", false);
    }

    Serial.println("Loaded frame host " + frameHost + ":" + framePort);

    if(useSSL) {
      client = new WiFiClientSecure;
      client->setInsecure();
    }

    if (pendingButton)
      press_button(pendingButton);

    //if(forceUpdate)
    //  Etag = "";
    //else
    //  Etag = prefs.getString("Etag","");

    String oldEtag = forceUpdate ? String("") : String(Etag);

    need_refresh = getFrame(oldEtag, battery, sleepMin);

    if(currentMode < FULL)
      drawLowBattery(TFT_RED);
    
    if(currentMode == SLEEP)
      drawNoWifi(TFT_RED);
    
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
