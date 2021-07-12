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

#define BATT_PIN            36
#define FETCH_INTERVAL 1800000 // 30 minutes

uint8_t *framebuffer;
int vref = 1100;

const char* ssid     = "AssKicker";     // WiFi SSID to connect to
const char* password = "19970720cool"; // WiFi password needed for the SSID

DynamicJsonDocument jsonDoc(1024);
bool todoNeedRefresh = false;
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

  // Correct the ADC reference voltage
  esp_adc_cal_characteristics_t adc_chars;
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
    Serial.printf("eFuse Vref:%u mV", adc_chars.vref);
    vref = adc_chars.vref;
  }

  epd_init();

  framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT);
  if (!framebuffer) {
    Serial.println("alloc memory failed !!!");
    while (1);
  }
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  epd_poweron();
  epd_clear();
  epd_poweroff();


  int cursor_x = 50;
  int cursor_y = 50;
  
  epd_poweron();
  fetchTodoList();

  char * todoTitle = "TODO: ";
  writeln((GFXfont *)&FiraSans, todoTitle, &cursor_x, &cursor_y, NULL);
  delay(500);
  drawTodoList(jsonDoc.as<JsonArray>(), 50, 100);
  drawBatteryInfo();

  epd_poweroff_all();
  delay(FETCH_INTERVAL);
}

void loop()
{
  // When reading the battery voltage, POWER_EN must be turned on
  epd_poweron();
  fetchTodoList();

  if (todoNeedRefresh) {
    drawTodoList(jsonDoc.as<JsonArray>(), 50, 100);
  }
  else {
    Serial.println("Same content, no need of refresh.");
  }
  
  drawBatteryInfo();

  epd_poweroff_all();
  delay(FETCH_INTERVAL);
}

uint8_t startWiFi() {
  Serial.print("\r\nConnecting to: "); Serial.println(String(ssid));
  IPAddress dns(8, 8, 8, 8); // Google DNS
//  WiFi.disconnect();
  WiFi.mode(WIFI_STA); // switch off AP
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);
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
  WiFi.mode(WIFI_OFF);
}

void fetchTodoList() {
  if ((startWiFi() == WL_CONNECTED)) {
    WiFiClient client;   // wifi client object
    client.stop(); // close connection before sending a new request
    HTTPClient http;
    String server = "code.lagagggggg.cn";
    int port = 5556;
    String uri = "/todo_list";

    Serial.println("Begin fetching todo list\n");
    http.begin(client, server, port, uri);
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
      Serial.printf("connection failed, error: %s", http.errorToString(httpCode).c_str());
      client.stop();
      http.end();
    }
//    stopWiFi();
  }
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

void drawTodoList(JsonArray todoList, int initX, int initY) {
    epd_clear_area(lastTodoListArea);
  int cursorX = initX;
  int cursorY = initY;
  int maxX = 0;
  for (JsonVariant value : todoList) {
    String s = "➠ " + value.as<String>();
    Serial.printf("draw todo: %s\n", s.c_str());
    writeln((GFXfont *)&FiraSans, (char *)s.c_str(), &cursorX, &cursorY, NULL);
    delay(500);
    maxX = maxX >= cursorX ? maxX : cursorX;
    cursorX = initX;
    cursorY += 50;
  }

  todoNeedRefresh = false;
  lastTodoListArea = {
        .x = initX,
        .y = initY - 30,
        .width = maxX - initX,
        .height = cursorY - initY + 30,
  };
}

void drawBatteryInfo() {
  uint16_t v = analogRead(BATT_PIN);
  float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
  String voltage = "🚀 Vol:" + String(battery_voltage) + "V";
//  Serial.println(voltage);

  Rect_t area = {
    .x = 700,
    .y = 480,
    .width = 240,
    .height = 50,
  };

  int cursor_x = 720;
  int cursor_y = 520;
  epd_clear_area(area);
  writeln((GFXfont *)&FiraSans, (char *)voltage.c_str(), &cursor_x, &cursor_y, NULL);
}
