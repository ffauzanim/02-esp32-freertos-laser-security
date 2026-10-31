// =========================================================
// LASER SECURITY SYSTEM - ESP32-WROOM-32
// RTOS VERSION (FIXED v7 FINAL) + LOW POWER
// =========================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"

// Low Power headers
#include "esp_pm.h"
#include "esp_wifi.h"
#include "esp_sleep.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID     "YOUR_WIFI_SSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

#ifndef BOT_TOKEN
#define BOT_TOKEN     "YOUR_BOT_TOKEN"
#endif

#ifndef CHAT_ID
#define CHAT_ID       "YOUR_CHAT_ID"
#endif

#define TELEGRAM_MSG_LEN 256

LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

const int led1    = 4;
const int led2    = 5;
const int led3    = 18;
const int led4    = 19;
const int buzzer  = 12;
const int ldrPin1 = 35;
const int ldrPin2 = 34;

volatile int ldrValue1 = 0;
volatile int ldrValue2 = 0;

volatile bool alarm1 = false;
volatile bool alarm2 = false;

volatile bool alarmActive = false;
volatile bool buzzerMuted = false;
volatile bool alarmLatch  = false;

volatile int alarmCount = 0;

// =========================================================
// LATENCY MEASUREMENT
// =========================================================
volatile uint32_t triggerTime = 0;
volatile uint32_t responseTime = 0;
volatile bool latencyCaptured = false;

const int threshold = 2800;

SemaphoreHandle_t stateMutex;
SemaphoreHandle_t lcdMutex;
QueueHandle_t telegramQueue;

typedef struct {
  char msg[TELEGRAM_MSG_LEN];
} TelegramMsg_t;

void turnOffAll();
void queueTelegram(const String &msg);
void handleTelegramMessages(int numNewMessages);
void reconnectWiFiIfNeeded();
void initLowPower();

void TaskSensor(void *pvParameters);
void TaskAlarm(void *pvParameters);
void TaskLED(void *pvParameters);
void TaskLCD(void *pvParameters);
void TaskTelegram(void *pvParameters);
void TaskSerial(void *pvParameters);

// =========================================================
// LOW POWER INIT
// =========================================================
void initLowPower() {
  // --- 1. CPU Dynamic Frequency Scaling ---
  // CPU turun ke 80 MHz saat idle, naik ke 240 MHz saat butuh
  esp_pm_config_t pm_config = {
    .max_freq_mhz       = 240,
    .min_freq_mhz       = 80,
    .light_sleep_enable = true  // Light Sleep otomatis antar vTaskDelay
  };
  esp_pm_configure(&pm_config);

  // --- 2. WiFi Modem Sleep (DTIM-based) ---
  // Radio WiFi mati saat tidak ada TX/RX, bangun per interval DTIM
  // Hemat ~100-150 mA tanpa putus koneksi
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  Serial.println("[PM] Low Power Mode aktif: CPU DFS + Modem Sleep");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms     = 20000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_reconfigure(&wdt_config);

  pinMode(led1, OUTPUT);
  pinMode(led2, OUTPUT);
  pinMode(led3, OUTPUT);
  pinMode(led4, OUTPUT);
  pinMode(buzzer, OUTPUT);

  turnOffAll();

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting WiFi");
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi ");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connected! ");
  delay(1000);

  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  initLowPower();

  stateMutex    = xSemaphoreCreateMutex();
  lcdMutex      = xSemaphoreCreateMutex();
  telegramQueue = xQueueCreate(20, sizeof(TelegramMsg_t));

  bot.sendMessage(CHAT_ID, "Laser Security RTOS v7 Online", "");

  xTaskCreatePinnedToCore(TaskSensor,   "Sensor",   4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(TaskAlarm,    "Alarm",    4096, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(TaskLED,      "LED",      2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskLCD,      "LCD",      4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskTelegram, "Telegram", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskSerial,   "Serial",   2048, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}

void TaskSensor(void *pvParameters) {
  esp_task_wdt_add(NULL);

  while (1) {
    esp_task_wdt_reset();

    int v1 = analogRead(ldrPin1);
    int v2 = analogRead(ldrPin2);

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      ldrValue1 = v1;
      ldrValue2 = v2;

      bool newAlarm1 = (v1 > threshold);
      bool newAlarm2 = (v2 > threshold);

      alarm1 = newAlarm1;
      alarm2 = newAlarm2;

      // Simpan timestamp saat pertama kali trigger
      if ((newAlarm1 || newAlarm2) && !latencyCaptured) {
        triggerTime = micros();
        latencyCaptured = true;
      }

      xSemaphoreGive(stateMutex);
    }

    Serial.printf("[S1]%d [S2]%d\n", v1, v2);

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void TaskAlarm(void *pvParameters) {
  esp_task_wdt_add(NULL);

  while (1) {
    esp_task_wdt_reset();

    bool a1, a2, muted;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      a1    = alarm1;
      a2    = alarm2;
      muted = buzzerMuted;

      xSemaphoreGive(stateMutex);
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (muted) {
      digitalWrite(buzzer, LOW);

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        alarmActive = false;
        alarmLatch  = false;
        xSemaphoreGive(stateMutex);
      }

      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (a1 || a2) {
      digitalWrite(buzzer, HIGH);

      // Hitung latency
      if (latencyCaptured && responseTime == 0) {
        responseTime = micros() - triggerTime;

        Serial.println("===== LATENCY TEST =====");
        Serial.print("Response Time (us): ");
        Serial.println(responseTime);

        Serial.print("Response Time (ms): ");
        Serial.println(responseTime / 1000.0);
      }

      bool sendNotif = false;

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (!alarmActive) {
          alarmActive = true;
          alarmLatch  = true;
          alarmCount++;
          sendNotif   = true;
        }

        xSemaphoreGive(stateMutex);
      }

      if (sendNotif) {
        String msg = "ALERT!\n";

        if (a1) msg += "Sensor 1 Triggered\n";
        if (a2) msg += "Sensor 2 Triggered\n";

        msg += "Count: " + String((int)alarmCount);

        queueTelegram(msg);
      }

    } else {
      bool latched;

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        latched = alarmLatch;
        xSemaphoreGive(stateMutex);
      }

      if (!latched) {
        digitalWrite(buzzer, LOW);

        bool wasActive = false;

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          wasActive   = alarmActive;
          alarmActive = false;
          xSemaphoreGive(stateMutex);
        }

        if (wasActive) {
          latencyCaptured = false;
          responseTime = 0;

          queueTelegram("System Normal");
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void TaskLED(void *pvParameters) {
  esp_task_wdt_add(NULL);

  const int leds[] = {led1, led2, led3, led4};
  int index = 0;

  while (1) {
    esp_task_wdt_reset();

    bool active, muted;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      active = alarmActive;
      muted  = buzzerMuted;

      xSemaphoreGive(stateMutex);
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    if (muted || !active) {
      for (int i = 0; i < 4; i++) {
        digitalWrite(leds[i], LOW);
      }

      index = 0;

      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    for (int i = 0; i < 4; i++) {
      digitalWrite(leds[i], LOW);
    }

    digitalWrite(leds[index], HIGH);

    index = (index + 1) % 4;

    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

void TaskLCD(void *pvParameters) {
  esp_task_wdt_add(NULL);

  while (1) {
    esp_task_wdt_reset();

    int v1, v2;
    bool muted, active;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      v1     = ldrValue1;
      v2     = ldrValue2;
      muted  = buzzerMuted;
      active = alarmActive;

      xSemaphoreGive(stateMutex);
    } else {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      lcd.setCursor(0, 0);

      if (muted) {
        lcd.print("Alarm: MUTED   ");
      } else if (active) {
        lcd.print("!! INTRUDER !! ");
      } else {
        lcd.print("Status Normal  ");
      }

      lcd.setCursor(0, 1);

      char buf[17];
      snprintf(buf, sizeof(buf), "S1:%-4d S2:%-4d", v1, v2);
      lcd.print(buf);

      xSemaphoreGive(lcdMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(300));
  }
}

void TaskTelegram(void *pvParameters) {
  esp_task_wdt_add(NULL);

  TelegramMsg_t incoming;

  while (1) {
    esp_task_wdt_reset();

    reconnectWiFiIfNeeded();

    while (xQueueReceive(telegramQueue, &incoming, 0) == pdTRUE) {
      bot.sendMessage(CHAT_ID, String(incoming.msg), "");
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    int n = bot.getUpdates(bot.last_message_received + 1);

    while (n) {
      handleTelegramMessages(n);
      n = bot.getUpdates(bot.last_message_received + 1);
    }

    vTaskDelay(pdMS_TO_TICKS(300));
  }
}

void TaskSerial(void *pvParameters) {
  while (1) {
    if (Serial.available()) {
      Serial.readStringUntil('\n');
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void handleTelegramMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String text    = bot.messages[i].text;
    String chat_id = bot.messages[i].chat_id;

    if (chat_id != CHAT_ID) {
      bot.sendMessage(chat_id, "Unauthorized", "");
      continue;
    }

    if (text == "/silentalarm") {
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        alarmActive = false;
        alarmLatch  = false;
        xSemaphoreGive(stateMutex);
      }

      digitalWrite(buzzer, LOW);
      bot.sendMessage(CHAT_ID, "Alarm di-silent, sistem tetap aktif...", "");

    } else if (text == "/matialarm") {
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        alarmActive = false;
        alarmLatch  = false;
        buzzerMuted = true;
        xSemaphoreGive(stateMutex);
      }

      digitalWrite(buzzer, LOW);
      bot.sendMessage(CHAT_ID, "Sistem alarm dimatikan total", "");

    } else if (text == "/nyalaalarm") {
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        buzzerMuted = false;
        xSemaphoreGive(stateMutex);
      }

      bot.sendMessage(CHAT_ID, "Sistem alarm diaktifkan", "");

    } else if (text == "/status") {
      bool active, muted;

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        active = alarmActive;
        muted  = buzzerMuted;
        xSemaphoreGive(stateMutex);
      } else {
        active = false;
        muted  = false;
      }

      String msg =
        "Alarm: "       + String(active ? "ON"  : "OFF") +
        "\nMuted: "     + String(muted  ? "YES" : "NO")  +
        "\nThreshold: " + String(threshold);

      bot.sendMessage(CHAT_ID, msg, "");
    }
  }
}

void queueTelegram(const String &msg) {
  TelegramMsg_t pkt;
  memset(pkt.msg, 0, sizeof(pkt.msg));
  msg.toCharArray(pkt.msg, sizeof(pkt.msg));

  xQueueSend(telegramQueue, &pkt, pdMS_TO_TICKS(50));
}

void reconnectWiFiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi putus, reconnecting...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retry = 0;

  while (WiFi.status() != WL_CONNECTED && retry < 40) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Reconnected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nReconnect gagal, coba lagi nanti...");
  }
}

void turnOffAll() {
  digitalWrite(buzzer, LOW);
  digitalWrite(led1,   LOW);
  digitalWrite(led2,   LOW);
  digitalWrite(led3,   LOW);
  digitalWrite(led4,   LOW);
}
