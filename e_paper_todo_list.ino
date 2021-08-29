#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM !!!"
#endif

#include <Arduino.h>
#include <esp_task_wdt.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "epd_driver.h"
#include "firasans.h"
#include "esp_adc_cal.h"
#include <Wire.h>
#include <SPI.h>
#include "logo.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

int vref = 1100;

#define BATT_PIN            36
#define FETCH_INTERVAL 1800000 // ms, == 30 minutes

#define ENABLE_DEEP_SLEEP 1
#define uS_TO_S_FACTOR 1000000ULL  /* Conversion factor for micro seconds to seconds */
#define uS_TO_MIN_FACTOR 60000000ULL  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  1200        /* Time ESP32 will go to sleep (in seconds) */

RTC_DATA_ATTR int bootCount = 0;

const char* ssid     = "AssKicker";     // WiFi SSID to connect to
const char* password = "19970720cool"; // WiFi password needed for the SSID

const char* ntpServer = "ntp.aliyun.com";
const long  gmtOffset_sec = 28800; // +8
const int   daylightOffset_sec = 0;
int currentHour = -1;
int currentMin  = -1;
int sleepHour = 0, wakeHour = 8;

DynamicJsonDocument jsonDoc(1024);
bool todoNeedRefresh = true;
String lastTimeTodoContentString = "";

Rect_t lastTodoListArea = {
        .x = 0,
        .y = 0,
        .width = 0,
        .height = 0,
};

void setup()
{
  Serial.begin(115200);
  
  initDisplay();
  refreshTodo();

#if ENABLE_DEEP_SLEEP
  uint64_t time_to_sleep = TIME_TO_SLEEP * uS_TO_S_FACTOR; // Normal sleep time
  if (currentHour >= sleepHour && currentHour < wakeHour) { // Night sleep time
    time_to_sleep = ((wakeHour - currentHour) * 60 - currentMin) * uS_TO_MIN_FACTOR;
  }
  esp_sleep_enable_timer_wakeup(time_to_sleep);
  Serial.println("Setup ESP32 to sleep for " + String((int)(time_to_sleep/uS_TO_S_FACTOR)) +
  " Seconds");
  
  Serial.println("Going to sleep now");
  Serial.flush(); 
  esp_deep_sleep_start();
  Serial.println("This will never be printed");
#endif
  
  delay(FETCH_INTERVAL);
}

void loop()
{
  refreshTodo();
  delay(FETCH_INTERVAL);
}

void initDisplay() {
  // Correct the ADC reference voltage
  esp_adc_cal_characteristics_t adc_chars;
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
    Serial.printf("eFuse Vref:%u mV", adc_chars.vref);
    vref = adc_chars.vref;
  }

  epd_init();
  epd_poweron();
  epd_clear();

  int initX = 50, initY = 50;
  char * todoTitle = "TODO: ";
  writeln((GFXfont *)&FiraSans, todoTitle, &initX, &initY, NULL);
}

void refreshTodo() {
  // When reading the battery voltage, POWER_EN must be turned on
  epd_poweron();
  fetchTodoList();

  if (todoNeedRefresh) {
    drawStringArray(jsonDoc.as<JsonObject>()["todos"], 50, 100, "➠ ");
    drawStringArray(jsonDoc.as<JsonObject>()["other"], 550, 50, "✭ ");
  }
  else {
    Serial.println("Same content, no need of refresh.");
  }

  getAndDrawTime();
  drawBatteryInfo();

  epd_poweroff_all();
}

uint8_t startWiFi() {
  Serial.print("\r\nConnecting to: "); Serial.println(String(ssid));
  IPAddress dns(8, 8, 8, 8); // Google DNS
  WiFi.disconnect();
  WiFi.mode(WIFI_STA); // switch off AP
//  WiFi.setAutoConnect(true);
//  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  uint8_t connectionStatus;
  bool AttemptConnection = true;
  while (AttemptConnection) {
    connectionStatus = WiFi.status();
    if (millis() > start + 15000) { // Wait 15-secs maximum
      AttemptConnection = false;
    }
    if (connectionStatus == WL_CONNECTED || connectionStatus == WL_CONNECT_FAILED) {
      AttemptConnection = false;
    }
    delay(50);
  }
  if (connectionStatus == WL_CONNECTED) {
    //    wifi_signal = WiFi.RSSI(); // Get Wifi Signal strength now, because the WiFi will be turned off to save power!
    Serial.println("WiFi connected at: " + WiFi.localIP().toString());
  }
  else Serial.println("WiFi connection *** FAILED ***");
  return connectionStatus;
}

void stopWiFi() {
  WiFi.disconnect();
  Serial.println("WiFi disconnected");
//  WiFi.mode(WIFI_OFF);
}

void fetchTodoList() {
  if ((startWiFi() == WL_CONNECTED)) {
    WiFiClient client;   // wifi client object
    client.stop(); // close connection before sending a new request
    HTTPClient http;
    String server = "code.lagagggggg.cn";
    int port = 5556;
    String uri = "/todo_list_with_other_info";

    Serial.println("Begin fetching todo list\n");
    http.begin(client, server, port, uri);
    http.setTimeout(10000);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String content = http.getString();
      Serial.printf("content: %s\n", content.c_str());
      client.stop();
      http.end();
      if (lastTimeTodoContentString != content) {
        todoNeedRefresh = true;
        lastTimeTodoContentString = content;
        Serial.printf("fetched different content, need reload!\n");
        decodeTodoList(content);
      }
    }
    else
    {
      Serial.printf("connection failed, error: %s\n", http.errorToString(httpCode).c_str());
      client.stop();
      http.end();
    }
  }
//  stopWiFi();
}

void decodeTodoList(String json) {
  Serial.print(F("\nDecoding object\n"));
  // Deserialize the JSON document
  DeserializationError error = deserializeJson(jsonDoc, json);
  // Test if parsing succeeds.
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
  }
  else {
    Serial.print(F("deserializeJson() success\n"));
  }
}

void drawStringArray(JsonArray stringArray, int initX, int initY, String prefix) {
//  epd_clear_area(lastTodoListArea);
  int cursorX = initX;
  int cursorY = initY;
  int maxX = 0;
  for (JsonVariant value : stringArray) {
    String s = prefix + value.as<String>();
    Serial.printf("draw todo: %s\n", s.c_str());
    writeln((GFXfont *)&FiraSans, (char *)s.c_str(), &cursorX, &cursorY, NULL);
    delay(100);
    maxX = maxX >= cursorX ? maxX : cursorX;
    cursorX = initX;
    cursorY += 50;
  }

  todoNeedRefresh = false;
//  lastTodoListArea = {
//        .x = initX,
//        .y = initY - 30,
//        .width = maxX - initX,
//        .height = cursorY - initY + 30,
//  };
}

void drawBatteryInfo() {
  uint16_t v = analogRead(BATT_PIN);
  float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
  String voltage = "🚀 Vol:" + String(battery_voltage) + "V";
  Serial.println(voltage);

#if ENABLE_DEEP_SLEEP
#else
  Rect_t area = {
    .x = 700,
    .y = 480,
    .width = 240,
    .height = 50,
  };
  epd_clear_area(area);
#endif

  int cursor_x = 720;
  int cursor_y = 520;
  writeln((GFXfont *)&FiraSans, (char *)voltage.c_str(), &cursor_x, &cursor_y, NULL);
}

void getAndDrawTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    currentHour = -1;
    currentMin  = -1;
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  currentHour = timeinfo.tm_hour;
  currentMin  = timeinfo.tm_min;

  char timeString[30];
  strftime(timeString, sizeof(timeString), "✎ updated at: %H:%M", &timeinfo);  // Creates: '14:05:49'

#if ENABLE_DEEP_SLEEP
#else
  Rect_t area = {
    .x = 40,
    .y = 480,
    .width = 400,
    .height = 50,
  };
  epd_clear_area(area);
#endif

  int cursor_x = 50;
  int cursor_y = 520;
  writeln((GFXfont *)&FiraSans, timeString, &cursor_x, &cursor_y, NULL);
}
