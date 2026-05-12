#include <Arduino.h>
#ifndef NO_MQTT
#include <ESP32MQTTClient.h>
#endif
#include <WiFi.h>
//------ you should create your own wifi_config.h with:
//
// For non-CYD builds (all three settings required):
//#define MQTT_URI      "mqtt://x.x.x.x:1883"
//#define MQTT_USERNAME ""
//#define MQTT_PASSWORD ""
//#define WLAN_SSID     "your_ssid"
//#define WLAN_PASS     "your_password"
//
// For CYD builds: wifi_config.h can be empty — WiFi and MQTT are
// configured interactively on the display at first boot.
#include "wifi_config.h"
#ifdef CYD
#include <esp32_smartdisplay.h>
#include "wifisetup.hpp"
#include "cyddisplay.hpp"
#include "i18n.hpp"
#include <DHTesp.h>
#endif
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "globals.hpp"
#include "trumaframes.hpp"
#include "autodiscovery.hpp"
#include "settings.hpp"
#include <Lin_Interface.hpp>
#include "waterboost.hpp"
#include "commandreader.hpp"
#ifdef WEBSERVER
#include "webserver.hpp"
#endif
#include <ArduinoOTA.h>
#ifdef WEBSERVER
void publishSolarBatt();
#endif
#include <esp_task_wdt.h>

//adapt these to your board
//either use one of the presets or define
//your own led, tx and rx pin.
//if you define LED and RED_LED  the
//RED_LED will be used for errors and
//LED for ok, otherwise the same LED
//will be used for both error and ok
#ifdef GOOUUUC3
//this board has an RGB led, each color is on when the output is HIGH
#define RED_LED 3
#define LED 4
#define LED_ON LOW
#define LED_OFF HIGH
#define TX_PIN 19
#define RX_PIN 18
#endif
#ifdef WROOM32
//this board has a single led, on when the output is HIGH
#define LED 2
#define LED_ON HIGH
#define LED_OFF LOW
#define TX_PIN 19
#define RX_PIN 18
#endif
#ifdef C3SUPERMINI
//this board has a single led, on when the output is LOW
#define LED 8
#define LED_ON LOW
#define LED_OFF HIGH
#define TX_PIN 6
#define RX_PIN 7
#endif
#ifdef CYD
// Display boards (CYD / CYD_C5)
// Status shown on LVGL display — no LED used.
#ifdef CYD_C5
// ESP32-C5-WROOM-1 (NM-CYD-C5) — 16MB Flash / 8MB PSRAM
// LIN bus UART on P5 (LP-UART): leaves CN1 I2C (IO8/IO9) free for expansion.
#define TX_PIN 5
#define RX_PIN 4
// IO27 drives the onboard WS2812B RGB LED; LED must be removed/soldered off
// to use this pin for the AM2301 (DHT) sensor.
#define DHT_DATA_PIN 27
#else
// ESP32-2432S028R (Cheap Yellow Display)
// LIN bus UART pins — see platformio.ini for CYD mapping
#define TX_PIN 27
#define RX_PIN 22
#define DHT_DATA_PIN 17
#endif

// ── Sensor exterior AM2301 ───────────────────────────────────────────────────
static DHTesp   s_dht;
static float    s_extTemp   = -273.0f;  // -273 = sin lectura válida
static uint32_t s_lastDhtMs = 0;

static void readExtSensor() {
    TempAndHumidity result = s_dht.getTempAndHumidity();
    if (s_dht.getStatus() == DHTesp::ERROR_NONE &&
        result.temperature > -40.0f && result.temperature < 80.0f) {
        s_extTemp = result.temperature;
        Serial.printf("[am2301] %.1f °C  %.0f %%RH\n",
                      result.temperature, result.humidity);
    } else {
        Serial.printf("[am2301] error: %s\n", s_dht.getStatusString());
    }
}

// ── LVGL task (Core 1) ────────────────────────────────────────────────────
// lv_timer_handler() must be called every few ms for smooth touch response.
// The LIN bus loop takes ~150 ms per iteration so we run LVGL on its own task.
static SemaphoreHandle_t s_lvglMutex = nullptr;

static void lvglTask(void*) {
    static uint32_t last = millis();
    static bool s_wmLogged = false;
    for (;;) {
        uint32_t now = millis();
        lv_tick_inc(now - last);
        last = now;
        if (xSemaphoreTake(s_lvglMutex, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvglMutex);
        }
        if (!s_wmLogged && millis() > 12000) {
            s_wmLogged = true;
            Serial.printf("[wm] lvglTask=%u\n",
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// Call before any LVGL function from the main loop.
static inline void lvglLock()   { xSemaphoreTake(s_lvglMutex, portMAX_DELAY); }
static inline void lvglUnlock() { xSemaphoreGive(s_lvglMutex); }

#endif
#ifndef TX_PIN
#error Please define GOOUUUC3, C3SUPERMINI, WROOM32, CYD or CYD_C5
//#include <stop>
#endif

//Lin master
Lin_Interface LinBus(1); 

//Waterboost management
TWaterBoost *WaterBoost;

#ifdef COMBIGAS
#define FRAMES_TO_READ 1
#define FRAMES_TO_WRITE 7
#define MASTER_FRAMES 1
#else
#define FRAMES_TO_READ 2
#define FRAMES_TO_WRITE 7
#define MASTER_FRAMES 2
#endif
//the frames to be read
TFrameBase * frames_to_read[FRAMES_TO_READ];
TFrame21 *Frame21;
TFrame22 *Frame22 = nullptr;
//the frames to be written
TFrameBase * frames_to_write[FRAMES_TO_WRITE];
TFrameSetTemp *SimulateTempFrame;
TFrameSetTemp *RoomSetpointFrame;
TFrameSetTemp *WaterSetpointFrame;
TFrameEnergySelect *EnergySelect;
TFrameSetPowerLimit *SetPowerLimit;
TFrameSetFan *FanFrame;
TFrameSetControl20 *ControlFrame20;
#ifdef COMBIGAS
TFrameSetControlElements *ControlElementsFrame;
#endif
//the setpoints
#define SETPOINTS 5
TMqttSetting * MqttSetpoint[SETPOINTS];

// ── Modo de energía — fuente de verdad compartida entre CYD y web ─────────
// CYD escribe vía cydGetEnergyMode(), web vía /energy_idx WebSocket.
static volatile int s_energyIdx     = 0;
static          int s_lastEnergyIdx = -1;  // para detectar cambio desde CYD
TTempSetting *SimulatedTemp;
TTempSetting *RoomSetpoint;
TBoilerSetting *WaterSetpoint;
TFanSetting *FanMode;
TOnOffSetting *HeatingOn;

//master frames
TMasterFrame * master_frames[MASTER_FRAMES];
//I'll only need to manage these 2 master frames,
//the remaininig one will be directly allocated in master_frames
TOnOff *onOff;
TGetErrorInfo *getErrorInfo;

//only one master frame is sent for each bus cycle, keep track of the next one
int current_master_frame=-1;

//error reset requested
volatile boolean truma_reset=false;
//error reset requested and truma in status 1, stop communication
boolean truma_reset_stop_comm=false;
//new web client or new connection to the broker, force a
//send of the frame received data
volatile boolean doforcesend=false;
//keep truma on (websocket or screen active)
volatile boolean forceon=false;
//delay to stop the communication during reset
unsigned long truma_reset_delay;
//max time to wait for the truma to report status 1 during reset
unsigned long truma_reset_max_time;
//time to keep the truma on when there's nothing on
//(necessary to switch the fan speed to 0)
unsigned long off_delay;

boolean inota=false;

//serial port commands
TCommandReader CommandReader;

//publish to the mqtt broker/websocket client that an error reset is under way
TPubBool PublishReset("/reset");
//publish to the mqtt broker/websocket client that the lin bus comm is ok
TPubBool PublishLinOk("/linok");
//publish mqtt connection state to websocket clients
TPubBool PublishMqttOk("/mqttok");
//heartbeat (for the mqtt clients to detect this program is working)
TPubBool PublishHeartBeat("/heartbeat");

//start an error reset
void HandleCommandReset();
// Forward declaration: linBusTask is defined after loop() but used in setup()
static void linBusTask(void*);
static volatile bool linSniff = false;  // passive sniffer mode (sniff on|off para diagnóstico)
static bool linSniffUartOpen = false;
//led in a separate task (not used on CYD)
#ifndef CYD
void LedLoop(void * pvParameters);
#endif

#ifdef WEBSERVER
//message from the websocket
void wsCallback(const String& topic, const String& payload);
//new client connected
void wsConnected();
#endif

//wifi status and delay to try and restart it
bool wifistarted=false;
unsigned long lastwifi;

//conditions for the led
bool wifiok=false;
volatile bool trumaok=false;
bool mqttok=false;
bool mqttEnabled=false;  // false si el usuario omitió la config MQTT

//---------------------------------------------------------------------
static void logMem(const char* tag) {
  Serial.printf("[mem] %s free=%u largest=%u\n",
    tag,
    (unsigned)ESP.getFreeHeap(),
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void setup() {
  Serial.begin(115200);
  logMem("boot");

  LinBus.baud=9600;
  LinBus.txPin=TX_PIN;
  LinBus.rxPin=RX_PIN;
  LinBus.verboseMode=0;

  #ifndef CYD
  pinMode(LED,OUTPUT);
  #ifdef RED_LED
  pinMode(RED_LED,OUTPUT);
  #endif
  #endif // !CYD

  #ifdef CYD
  smartdisplay_init();
  logMem("after smartdisplay_init");
  // Sensor AM2301 (LED azul desoldado).
  s_dht.setup(DHT_DATA_PIN, DHTesp::AM2302);
  s_lvglMutex = xSemaphoreCreateMutex();
  #ifdef CYD_C5
  if (!LittleFS.begin(true)) {
    Serial.println("[LittleFS] mount failed");
  } else {
    Serial.println("[LittleFS] mounted OK");
  }
  #endif
  auto disp = lv_display_get_default();
  // LV_DISPLAY_ROTATION_270 gives MV=1,MX=0,MY=0 on the ILI9341 = correct
  // landscape for the ESP32-2432S028R.  (ROTATION_90 would give MV=1,MX=1,MY=1
  // which rotates the image 270° instead of 90°, producing the "vertical line"
  // artifact when our horizontal separator is rendered in portrait coordinates.)
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
  // Load touch calibration from NVS.  If none is stored yet (first boot or
  // after clearing NVS), run the interactive 3-point calibration screen.
  if (!loadTouchCalibration(touch_calibration_data)) {
    runTouchCalibration();
  }
  #endif

  //frames to read — CP Plus D protocol uses 0x21 and 0x22
  Frame21=new TFrame21();
  frames_to_read[0] = Frame21;
  #ifndef COMBIGAS
  Frame22 = new TFrame22();
  frames_to_read[1] = Frame22;
  #endif

  //frames to write
  SimulateTempFrame = new TFrameSetTemp(0x02);
  SimulateTempFrame->setTemperature(-273.0);
  RoomSetpointFrame = new TFrameSetTemp(0x03);
  WaterSetpointFrame = new TFrameSetTemp(0x04);
  EnergySelect = new TFrameEnergySelect(0x05);
  SetPowerLimit = new TFrameSetPowerLimit(0x06);
  FanFrame = new TFrameSetFan(0x07);
  ControlFrame20 = new TFrameSetControl20();
  frames_to_write[0] = SimulateTempFrame;
  frames_to_write[1] = RoomSetpointFrame;
  frames_to_write[2] = WaterSetpointFrame;
  frames_to_write[3] = EnergySelect;
  frames_to_write[4] = SetPowerLimit;
  frames_to_write[5] = FanFrame;
  frames_to_write[6] = ControlFrame20;
  #ifdef COMBIGAS
  ControlElementsFrame = new TFrameSetControlElements(0x1D);
  frames_to_write[6] = ControlElementsFrame;
  #endif

  //master frames
  onOff = new TOnOff();
  #ifdef COMBIGAS
  /* to enable diagnostic framess
    new TAssignFrameRanges(0x0b, {0x33, 0x34, 0x35, 0x36});
    new TAssignFrameRanges(0x0f, {0x37, 0x38, 0x39, 0x3a});
    new TAssignFrameRanges(0x13, {0x3b, 0xff, 0xff, 0xff})
  */
  master_frames[0] = onOff;
  #else
  // CP-Plus never uses B7 (TAssignFrameRanges) — omitting it to avoid
  // corrupting the Truma's frame table (confirmed via WomoLIN bus captures).
  master_frames[0] = onOff;
  getErrorInfo = new TGetErrorInfo();
  master_frames[1] = getErrorInfo;
  #endif

  //setpoints
  SimulatedTemp = new TTempSetting("/simultemp", -273.0, 30.0);
  SimulatedTemp->setADName("Simulated temperature")->setADEntity_category("diagnostic")->setADIcon("mdi:thermometer-lines");
  RoomSetpoint = new TTempSetting("/temp", 5.0, 30.0);
  RoomSetpoint->setADName("Temperature setpoint");
  HeatingOn = new TOnOffSetting("/heating");
  HeatingOn->setADName("Heating")->setADIcon("mdi:radiator");
  WaterSetpoint = new TBoilerSetting("/boiler");
  FanMode = new TFanSetting("/fan");
  MqttSetpoint[0] = SimulatedTemp;
  MqttSetpoint[1] = RoomSetpoint;
  MqttSetpoint[2] = HeatingOn;
  MqttSetpoint[3] = WaterSetpoint;
  MqttSetpoint[4] = FanMode;

  //waterboost
  WaterBoost = new TWaterBoost(WaterSetpoint,"high","/waterboost");

  #ifdef CYD
  // ── Persistencia NVS (CYD solamente) ──────────────────────────────────
  RoomSetpoint->setPersist(true);
  RoomSetpoint->loadPersistedValue();
  HeatingOn->setPersist(true);
  HeatingOn->loadPersistedValue();
  WaterSetpoint->setPersist(true);
  WaterSetpoint->loadPersistedValue();
  FanMode->setPersist(true);
  FanMode->loadPersistedValue();

  // Now that all settings objects exist, init the display controls.
  logMem("before cydDisplayInit");
  cydDisplayInit(RoomSetpoint, WaterSetpoint, HeatingOn, FanMode);
  logMem("after cydDisplayInit");
  #endif

  //autodiscovery for local topics
  PublishReset.setADComponent(CKBinary_sensor)->setADName("Resetting")->setADIcon("mdi:sync")->setADDevice_class("connectivity");
  PublishLinOk.setADComponent(CKBinary_sensor)->setADName("LIN bus status")->setADIcon("mdi:serial-port")->setADDevice_class("connectivity");

  // BLE init (or simulated stubs when BLE is disabled)
  logMem("before BLE init");
  victronBleInit();
  ultimatronBleInit();
  logMem("after BLE init");

  //starts the wifi (loop will check if it's connected)
  WiFi.mode(WIFI_STA);
  Serial.print("WiFi MAC: ");
  Serial.println(WiFi.macAddress());
  #ifdef CYD
  {
    String wifiSSID, wifiPass;
    if (!loadWifiCredentials(wifiSSID, wifiPass)) {
      runWifiSetup(wifiSSID, wifiPass);
    }
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  }
  #else
  WiFi.begin(WLAN_SSID, WLAN_PASS);
  #endif
  lastwifi=millis();

  //starts the mqtt connection to the broker
  #ifdef CYD
  #ifndef NO_MQTT
  {
    String mqttUri, mqttUser, mqttPass;
    String mqttHost, mqttPort;
    if (!loadMqttConfig(mqttHost, mqttPort, mqttUser, mqttPass)) {
      runMqttSetup(mqttUri, mqttUser, mqttPass);
      // runMqttSetup devuelve uri="" si el usuario pulsó "Omitir"
    } else if (!mqttHost.isEmpty() && mqttHost != "_skip_") {
      mqttUri = "mqtt://" + mqttHost + ":" + mqttPort;
    }
    // Solo inicia MQTT si hay URI válida (host no vacío)
    if (!mqttUri.isEmpty()) {
      mqttEnabled = true;
      mqttClient.setURI(mqttUri.c_str(), mqttUser.c_str(), mqttPass.c_str());
      mqttClient.enableLastWillMessage(STATUS_TOPIC, STATUS_OFFLINE, true);
      mqttClient.setKeepAlive(30);
      mqttClient.enableDebuggingMessages(false);
      mqttClient.loopStart();
    }
  }
  #endif // NO_MQTT
  #else
  mqttClient.setURI(MQTT_URI, MQTT_USERNAME, MQTT_PASSWORD);
  mqttClient.enableLastWillMessage(STATUS_TOPIC, STATUS_OFFLINE, true);
  mqttClient.setKeepAlive(30);
  mqttClient.enableDebuggingMessages(false);
  mqttClient.loopStart();
  #endif

  //creates the led task (not needed on CYD: status shown on display)
  #ifndef CYD
  xTaskCreate (
      LedLoop,     // Function to implement the task
      "LedLoop",   // Name of the task
      1000,      // Stack size in bytes
      NULL,      // Task input parameter
      1,         // Priority of the task
      NULL      // Task handle
    );
  #endif // !CYD

  // cydDisplayInit() already called right after smartdisplay_init() above

  ArduinoOTA
      .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else {// U_SPIFFS
          type = "filesystem";
          LittleFS.end();
        }
        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        inota=true;
        Serial.println("Start updating " + type);
      })
      .onEnd([]() {
        inota=false;
        Serial.println("\nEnd");
      })
      .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
        esp_task_wdt_reset();
        delay(1);
      })
      .onError([](ota_error_t error) {
        inota=false;
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
      });
  esp_task_wdt_init(10,true);

  // Pre-create the async_tcp task before LinBus/LVGL tasks fragment the heap.
  // AsyncTCP uses a singleton task handle: if the task already exists when
  // server.begin() is called in loop(), it skips xTaskCreate entirely.
  // We use a throw-away AsyncServer on port 9999 and immediately call end()
  // to release the PCB; the task itself keeps running.
  #ifdef WEBSERVER
  {
    logMem("before async_tcp pre-create");
    AsyncServer _pre(9999);
    _pre.begin();  // creates async_tcp task (8 KB stack) while heap is clean
    _pre.end();    // releases port 9999; task handle remains valid
    logMem("after async_tcp pre-create");
  }
  #endif

  // LIN bus task: pinned to Core 0 (WiFi core) so the blocking serial reads
  // don't interfere with LVGL (Core 1) or WebSocket response latency.
  xTaskCreatePinnedToCore(linBusTask, "LinBus", 4096, nullptr, 1, nullptr, 0);
  logMem("after LinBus task");

  #ifdef CYD
  // Start LVGL task only now — all LVGL init (calibration, wifi/mqtt setup
  // screens, cydDisplayInit) is complete, so no race condition.
  xTaskCreatePinnedToCore(lvglTask, "LVGL", 10240, nullptr, 2, nullptr, 1);
  logMem("after LVGL task");
  #endif
}

//enable/disables the master frames to assign frame ranges
void AssignFrameRanges(boolean on) {
  // No-op: B7 (TAssignFrameRanges) removed — CP-Plus never sends it.
  (void)on;
}

// When performing a reset only enable the onOff master frame
void EnableOnlyOnOff(boolean on) {
#ifndef COMBIGAS
  for (int i=0; i<MASTER_FRAMES; i++) {
    master_frames[i]->setEnabled(!on);
  }
  if (on) {
    onOff->setEnabled(true);
  }
#endif
}

// finds the next enabled master frame
void NextMasterFrame() {
#ifdef COMBIGAS
  current_master_frame = 0;
#else
  current_master_frame++;
  if (current_master_frame>=MASTER_FRAMES) {
    current_master_frame=0;
  }
  int count=MASTER_FRAMES;
  while (count>0) { 
    if (master_frames[current_master_frame]->getEnabled()) {
      return;
    }
    count--;
    current_master_frame++;
    if (current_master_frame>=MASTER_FRAMES) {
      current_master_frame=0;
    }
  }
  //no master frame enabled
  current_master_frame=-1;
#endif
}

//checks and restart the wifi connection
void CheckWifi() {
  // check wifi connectivity
  if (WiFi.status()==WL_CONNECTED) {
    if (!wifiok) {
      ArduinoOTA.setHostname("truminus");
      ArduinoOTA.begin();
    }
    wifiok=true;
    lastwifi=millis();
    if (!wifistarted) {
      Serial.print("IP address ");
      Serial.println(WiFi.localIP());
      wifistarted=true;
      #ifdef WEBSERVER
      StartServer(wsCallback, wsConnected);
      #endif
    }
  } else {
    if (wifiok) {
      ArduinoOTA.end();
      inota=false;
    }
    wifiok=false;
    unsigned long elapsed=millis()-lastwifi;
    if (elapsed>10000) {
    WiFi.reconnect();
    lastwifi=millis();
    }
  }
}

// ── LIN bus task (Core 0) ─────────────────────────────────────────────────
// All blocking serial I/O runs here so the main loop (Core 1) stays free
// to service WiFi, MQTT, OTA, WebSocket and the LVGL display.
static void linBusTask(void*) {
  // ── Sniffer state ───────────────────────────────────────────────────────
  enum SniffSt { S_IDLE, S_GOT_BREAK, S_GOT_SYNC, S_IN_FRAME } sniffSt = S_IDLE;
  uint8_t sniffPid = 0, sniffBuf[12]; int sniffN = 0;
  unsigned long sniffLastMs = 0;
  // Dedup cache: print full frame when first seen, then only changed bytes as diff.
  struct { uint8_t pid; uint8_t data[9]; bool seen; } sniffHistory[16];
  int sniffHistoryN = 0;

  for (;;) {
    esp_task_wdt_reset();

    // ── Modo sniff pasivo ─────────────────────────────────────────────────
    if (linSniff) {
      if (!linSniffUartOpen) {
        Serial1.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
        while (Serial1.available()) Serial1.read();
        linSniffUartOpen = true;
        sniffSt = S_IDLE; sniffN = 0;
        sniffHistoryN = 0;
        memset(sniffHistory, 0, sizeof(sniffHistory));
        Serial.println("[sniff] escuchando LIN bus — 'sniff off' para parar");
      }
      while (Serial1.available()) {
        uint8_t b = Serial1.read();
        unsigned long now = millis();
        // frame timeout: print partial frame and reset
        if (sniffSt == S_IN_FRAME && sniffN > 0 && now - sniffLastMs > 8) {
          Serial.printf("%7lu [%02X]", sniffLastMs, sniffPid);
          for (int i = 0; i < sniffN; i++) Serial.printf(" %02X", sniffBuf[i]);
          Serial.println(" TIMEOUT");
          sniffSt = S_IDLE; sniffN = 0;
        }
        sniffLastMs = now;
        switch (sniffSt) {
          case S_IDLE:
          case S_GOT_BREAK:
            if      (b == 0x00)                              sniffSt = S_GOT_BREAK;
            else if (b == 0x55 && sniffSt == S_GOT_BREAK)   sniffSt = S_GOT_SYNC;
            else                                             sniffSt = S_IDLE;
            break;
          case S_GOT_SYNC:
            sniffPid = b; sniffN = 0; sniffSt = S_IN_FRAME;
            break;
          case S_IN_FRAME:
            if (sniffN < 12) sniffBuf[sniffN++] = b;
            if (sniffN == 9) {
              // 0x3C/0x7D = LIN transport layer (master req / slave resp) — siempre cambiantes, sin interés
              if (sniffPid == 0x3C || sniffPid == 0x7D) { sniffSt = S_IDLE; sniffN = 0; break; }
              int slot = -1;
              for (int i = 0; i < sniffHistoryN; i++) {
                if (sniffHistory[i].pid == sniffPid) { slot = i; break; }
              }
              bool isNew = (slot < 0);
              if (isNew && sniffHistoryN < 16) slot = sniffHistoryN++;
              if (slot >= 0) {
                bool changed = isNew || !sniffHistory[slot].seen ||
                               memcmp(sniffHistory[slot].data, sniffBuf, 9) != 0;
                if (changed) {
                  Serial.printf("%7lu [%02X]", now, sniffPid);
                  if (!sniffHistory[slot].seen) {
                    // Primera vez: frame completo
                    for (int i = 0; i < 8; i++) Serial.printf(" %02X", sniffBuf[i]);
                    Serial.printf(" cs=%02X\n", sniffBuf[8]);
                  } else {
                    // Sólo bytes que cambiaron: bN:viejo→nuevo
                    for (int i = 0; i < 9; i++) {
                      if (sniffHistory[slot].data[i] != sniffBuf[i])
                        Serial.printf(" b%d:%02X→%02X", i, sniffHistory[slot].data[i], sniffBuf[i]);
                    }
                    Serial.println();
                  }
                  sniffHistory[slot].pid  = sniffPid;
                  sniffHistory[slot].seen = true;
                  memcpy(sniffHistory[slot].data, sniffBuf, 9);
                }
              }
              sniffSt = S_IDLE; sniffN = 0;
            }
            break;
        }
      }
      // frame timeout when no bytes available
      if (sniffSt == S_IN_FRAME && sniffN > 0 && millis() - sniffLastMs > 8) {
        Serial.printf("%7lu [%02X]", sniffLastMs, sniffPid);
        for (int i = 0; i < sniffN; i++) Serial.printf(" %02X", sniffBuf[i]);
        Serial.println(" TIMEOUT");
        sniffSt = S_IDLE; sniffN = 0;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    // Saliendo de modo sniff: cerrar UART y dejar que LinBus lo retome
    if (linSniffUartOpen) {
      Serial1.end();
      linSniffUartOpen = false;
    }

    byte PumpOrFan;
    double LocSetPointTemp = 0.0;
    double LocRoomSetpoint = RoomSetpoint->getFloatValue();
    double LocWaterSetpoint = WaterSetpoint->getFloatValue();
    boolean LocHeatingOn = HeatingOn->getIntValue()!=0;
    int LocFanMode = FanMode->getIntValue();

    //determines operating mode
    if (!LocHeatingOn) {
       if (LocFanMode > 0) {
         PumpOrFan = 0x10 | LocFanMode;
       } else if (LocFanMode == -2) {
         PumpOrFan = 0x12;  // high fan, no heating
       } else if (LocFanMode == -1) {
         PumpOrFan = 0x11;  // eco fan, no heating
       } else {
         PumpOrFan = 0x10;
       }
    } else {
      LocSetPointTemp = LocRoomSetpoint;
      if (LocFanMode!=-1 && LocFanMode!=-2) {
        if (LocFanMode==2) {
          FanMode->setValue("high",true);
          LocFanMode=-2;
        } else {
          FanMode->setValue("eco",true);
          LocFanMode=-1;
        }
      }
      if (LocFanMode==-2) {
        PumpOrFan = 2;
      } else {
        PumpOrFan = 1;
      }
    }

    //Water boost — sólo para modo "boost", no para "high" aunque ambos sean 60°C
    {
      bool wHeat = Frame22 ? Frame22->getWaterHeating() : Frame21->getWaterDemand();
      if (WaterSetpoint->getStringValue() == "boost") {
         WaterBoost->Start(wHeat);
      } else {
         WaterBoost->Stop();
      }
      if (WaterBoost->Active(wHeat)) {
          LocSetPointTemp=0.0;
          PumpOrFan=0;
      }
    }

    //prepare setpoint frames
    SimulateTempFrame->setTemperature(SimulatedTemp->getFloatValue());
    RoomSetpointFrame->setTemperature(LocSetPointTemp);
    WaterSetpointFrame->setTemperature(LocWaterSetpoint);
    FanFrame->setPumpOrFan(PumpOrFan);
    ControlFrame20->setRoomSetpoint(LocSetPointTemp);   // 0 when heating off → sends 0xAA 0xAA
    ControlFrame20->setWaterSetpoint(LocWaterSetpoint); // K×10>>4 en byte2, 0xAA si apagado
    {
      // bm=1 para cualquier modo activo: confirmado del CP Plus real (siempre envía 1).
      // El setpoint real lo controla byte2 (K×10>>4). bm=0 sólo cuando está apagado.
      uint8_t bm = (LocWaterSetpoint > 0.0) ? 1 : 0;
      ControlFrame20->setFanAndWater(PumpOrFan, bm);
    }

    //truma reset requested, turn off and wait
    if (truma_reset) {
      onOff->SetOn(false);
      if (!truma_reset_stop_comm) {
        unsigned long elapsed=millis()-truma_reset_max_time;
        if (elapsed>120000) {
          truma_reset=false;
          Serial.println("Reset time exceeded");
          EnableOnlyOnOff(false);
        }
      }
    } else
    if (LocHeatingOn || LocWaterSetpoint > 0.0 || LocFanMode != 0 || forceon) {
      onOff->SetOn(true);
      forceon = false;
      off_delay = millis();
    } else {
      if (onOff->GetOn()) {
         unsigned long elapsed=millis()-off_delay;
         if (elapsed>20000) {
          onOff->SetOn(false);
         }
      }
    }

    if (truma_reset_stop_comm) {
      unsigned long elapsed=millis()-truma_reset_delay;
      if (elapsed>10000) {
        truma_reset=false;
        truma_reset_stop_comm=false;
        EnableOnlyOnOff(false);
        Serial.println("Reset done, restarting communication");
      }
    } else {
      if (doforcesend) {
        for (int i=0; i<FRAMES_TO_READ; i++) frames_to_read[i]->setForcesend();
        for (int i=0; i<MASTER_FRAMES; i++) master_frames[i]->setForcesend();
        PublishLinOk.setForcesend();
        PublishReset.setForcesend();
        PublishHeartBeat.setForcesend();
        doforcesend=false;
      }
      //read all the frames (blocking — that's why this runs in its own task)
      for (int i=0; i<FRAMES_TO_READ; i++) {
        if (LinBus.readFrame(frames_to_read[i]->frameid(),8)) {
          frames_to_read[i]->setReadResult(true);
          frames_to_read[i]->setData((uint8_t*)&(LinBus.LinMessage));
        } else {
          frames_to_read[i]->setReadResult(false);
        }
      }

      boolean extraFramesOk=true;
      for (int i=1; i<FRAMES_TO_READ; i++) {
         if (!frames_to_read[i]->getDataOk()) { extraFramesOk=false; break; }
      }
      AssignFrameRanges(!extraFramesOk);

      // LIN OK if frame 0x21 or 0x22 responds.
      trumaok = Frame21->getDataOk();
      for (int i = 1; i < FRAMES_TO_READ && !trumaok; i++)
          trumaok = frames_to_read[i]->getDataOk();

      // Aplicar modo de energía — CYD tiene prioridad local; web sincroniza vía WS
      {
        #ifdef CYD
        // Si el dropdown CYD cambió, propagar a s_energyIdx y broadcast WS
        int cydEm = cydGetEnergyMode();
        if (cydEm != s_lastEnergyIdx && s_lastEnergyIdx >= 0) {
          s_energyIdx = cydEm;
          #ifdef WEBSERVER
          {
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "{\"command\":\"setting\",\"id\":\"/energy_idx\",\"value\":\"%d\"}",
                     s_energyIdx);
            wsQueueSend(buf);
          }
          #endif
        }
        if (cydEm != s_lastEnergyIdx) {
          s_lastEnergyIdx = cydEm;
          s_energyIdx     = cydEm;
        }
        #endif
        auto em = (TEnergySelection)s_energyIdx;
        EnergySelect->setEnergySelection(em);
        SetPowerLimit->setPowerLimit(em);
      }

      for (int i=0; i<FRAMES_TO_WRITE; i++) {
        frames_to_write[i]->getData((uint8_t*)&(LinBus.LinMessage));
        LinBus.writeFrame(frames_to_write[i]->frameid(),8);
      }
      NextMasterFrame();
      if (current_master_frame>=0) {
        master_frames[current_master_frame]->getData((uint8_t*)&(LinBus.LinMessage));
        LinBus.writeFrame(0x3c,8);
        if (LinBus.readFrame(0x3d,8)) {
          master_frames[current_master_frame]->setData((uint8_t*)&(LinBus.LinMessage));
          if (truma_reset && master_frames[current_master_frame]==onOff && onOff->getCurrentState()==1) {
            truma_reset_delay=millis();
            truma_reset_stop_comm=true;
            Serial.println("truma reset: truma off, stopping communication for 10 seconds");
          }
        } else {
          master_frames[current_master_frame]->setReadResult(false);
        }
      }
    }

    PublishReset.setValue(truma_reset);
    PublishLinOk.setValue(trumaok);
    PublishMqttOk.setValue(mqttok);
    PublishHeartBeat.setValue(true);

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

//-------------------------------------------------------------------------
void loop() {
  esp_task_wdt_reset();
  delay(1);
  #ifdef CYD
  // One-shot RAM / stack watermark diagnostic at 10 s after boot.
  {
    static bool s_memLogged = false;
    if (!s_memLogged && millis() > 10000) {
      s_memLogged = true;
      logMem("loop@10s");
      lv_mem_monitor_t mon;
      lv_mem_monitor(&mon);
      Serial.printf("[lvgl] pool used=%u/%u free=%u frag=%u%%\n",
          (unsigned)(mon.total_size - mon.free_size),
          (unsigned)mon.total_size,
          (unsigned)mon.free_size,
          (unsigned)mon.frag_pct);
      Serial.printf("[wm] loopTask=%u\n",
          (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
  }
  #endif
  #ifdef CYD
  // ── Nav request: pantallas de config bloqueantes ─────────────────────
  // Se comprueban ANTES del mutex. runWifiSetup/runMqttSetup gestionan LVGL
  // por sí mismas; tomamos el mutex para que lvglTask no interfiera y lo
  // liberamos antes de ESP.restart().
  {
    CydNavRequest nav = cydGetNavRequest();
    if (nav != CydNavRequest::None) {
      cydClearNavRequest();
      lvglLock();
      bool needRestart = false;
      if (nav == CydNavRequest::WifiSetup) {
        String ss, pp;
        // Free main-screen widgets before entering setup: the wifi screen
        // bundles a keyboard (~16 KB) and a dropdown that, when opened with
        // many networks, exceeds the 32 KB LVGL pool if the main UI is still
        // resident. Same pattern as SolarSetup below.
        cydFreeMainUI();
        needRestart = runWifiSetup(ss, pp);
        cydRebuildUI();
      } else if (nav == CydNavRequest::MqttSetup) {
        String u, us, pp;
        cydFreeMainUI();
        needRestart = runMqttSetup(u, us, pp);
        cydRebuildUI();
      } else if (nav == CydNavRequest::LangChange) {
        // Free main UI to give the lang-select screen the full LVGL pool.
        // cydRebuildUI() below recreates it after selection.
        cydFreeMainUI();
        cydShowLangSelect();
        cydRebuildUI();
      } else if (nav == CydNavRequest::SolarSetup) {
        String addr, key;
        // Free main-screen widgets before entering solar setup: the LVGL keyboard
        // widget alone consumes ~16 KB, and the 32 KB pool cannot hold both the
        // main UI and the setup screen simultaneously.
        cydFreeMainUI();
        // runSolarSetup already saves to NVS; restart so victronBleInit() picks it up.
        needRestart = runSolarSetup(addr, key);
        cydRebuildUI();   // rebuild main UI (replaces the freed widgets)
      }
      cydReloadScreen();
      // If the user cancelled (no restart needed) and came from a sub-screen
      // launched from the settings menu, return to the settings menu instead
      // of dropping all the way to the main UI.
      if (!needRestart &&
          (nav == CydNavRequest::WifiSetup ||
           nav == CydNavRequest::MqttSetup ||
           nav == CydNavRequest::SolarSetup ||
           nav == CydNavRequest::LangChange)) {
        cydOpenSettingsMenu();
      }
      lvglUnlock();
      if (needRestart) ESP.restart();
      // After a language change, broadcast the new language to all web clients
      if (nav == CydNavRequest::LangChange) {
        #ifdef WEBSERVER
        const char* lang = (currentLanguage() == Language::EN) ? "en" : "es";
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"setting\",\"id\":\"/lang\",\"value\":\"%s\"}",
                 lang);
        wsQueueSend(buf);
        #endif
      }
    }
  }
  // ── Sensor AM2301: leer cada 30 s (mínimo 2 s entre lecturas para el sensor)
  {
    uint32_t now = millis();
    // Primera lectura: esperar 2 s desde el arranque para que el sensor se estabilice.
    // Lecturas siguientes: cada 30 s.
    bool firstRead = (s_lastDhtMs == 0 && now > 2000);
    bool periodic  = (s_lastDhtMs > 0 && now - s_lastDhtMs >= 30000);
    if (firstRead || periodic) {
      s_lastDhtMs = now;
      lvglLock();          // pausar lvglTask durante los ~5 ms de lectura
      readExtSensor();
      lvglUnlock();
      // Difundir temperatura exterior a clientes WS
      #ifdef WEBSERVER
      {
        char buf[64];
        if (s_extTemp > -200.0f)
            snprintf(buf, sizeof(buf),
                     "{\"command\":\"status\",\"id\":\"outdoor_temp\",\"value\":\"%.1f\"}",
                     s_extTemp);
        else
            strcpy(buf, "{\"command\":\"status\",\"id\":\"outdoor_temp\",\"value\":\"--\"}");
        wsQueueSend(buf);
      }
      #endif
    }
  }

  // lv_timer_handler() runs in lvglTask (Core 1) every 5 ms.
  // We only need to update display state; grab the mutex briefly.
  if (xSemaphoreTake(s_lvglMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    double wTempDisp = Frame22 ? Frame22->getWaterTemp()    : Frame21->getWaterTemp();
    bool   wHeating  = (Frame22 ? Frame22->getWaterHeating(): Frame21->getWaterDemand())
                       && (WaterSetpoint->getFloatValue() > 0.0);
    cydDisplayUpdate(wifiok, trumaok, truma_reset, inota,
                     (float)Frame21->getRoomTemp(), (float)wTempDisp,
                     wHeating, s_extTemp,
                     getErrorInfo ? getErrorInfo->getErrorClass() : 0,
                     getErrorInfo ? getErrorInfo->getErrorCode()  : 0);
    // Throttle solar/battery display updates to ~1/3 Hz to avoid LVGL redraw
    // overhead interfering with screen dimming animations.
    {
      static uint32_t lastSolarDisp = 0;
      if (millis() - lastSolarDisp > 3333) {
        lastSolarDisp = millis();
        VictronData vd = victronGetData();
        CydSolarData sd = {
            victronIsConfigured(),
            vd.valid,
            vd.battV, vd.battA, vd.pvW, vd.kWhToday,
            vd.state, vd.errCode,
            vd.valid ? (uint32_t)(millis() - vd.lastMs) : 999999u
        };
        cydUpdateSolar(sd);

        UltimatronData ud = ultimatronGetData();
        CydBattData bd = {
            ultimatronIsConfigured(),
            ud.valid,
            ud.soc, ud.battV, ud.battA,
            ud.valid ? (uint32_t)(millis() - ud.lastMs) : 999999u
        };
        cydUpdateBatt(bd);
      }
    }
    xSemaphoreGive(s_lvglMutex);
  }
  #ifdef WEBSERVER
  // Publish solar/battery to WebSocket every 10 s
  {
    static uint32_t lastSolarWs = 0;
    if (wifistarted && millis() - lastSolarWs > 10000) {
      lastSolarWs = millis();
      publishSolarBatt();
    }
  }
  #endif
  #endif
  CheckWifi();
  if (wifiok) {
    ArduinoOTA.handle();
  }

  //accept a command from the serial port
  String cmd;
  String param;
  boolean found=false;
  if (CommandReader.Available(&cmd, &param)) {
    if (cmd=="help") {
       Serial.println("Available commands:");
       Serial.println("sniff on|off         escucha pasiva del bus LIN (splitter)");
       Serial.println("lindebug on|off");
       Serial.println("mqttdebug on|off");
       Serial.println("busindex             estado del bus LIN (frames + raw bytes)");
       Serial.println("reset");
       Serial.println("simultemp -273.0 (off)..30.0");
       Serial.println("temp 5.0..30.0");
       Serial.println("heating 0|1");
       Serial.println("boiler off|eco|high|boost");
       Serial.println("fan off|eco|high|1..10");
       found=true;
    } else if (cmd=="busindex") {
      found=true;
      uint8_t buf[8];
      Serial.println("=== LIN bus index ===");
      Serial.printf("  trumaok : %s\n", trumaok ? "ok" : "sin respuesta");
      {
        bool b8ok = onOff->getDataOk();
        Serial.printf("  B8 onOff: %s  req=%u cur=%u  (%s)\n",
                      b8ok ? "OK" : "NO RESPONDE",
                      onOff->getRequestedState(), onOff->getCurrentState(),
                      onOff->GetOn() ? "on" : "off");
        if (b8ok) {
          uint8_t rb[8];
          onOff->getRawReply(rb);
          Serial.printf("            reply:");
          for (int j = 0; j < 8; j++) Serial.printf(" %02X", rb[j]);
          Serial.println();
        }
      }
      {
        bool b2ok = getErrorInfo->getDataOk();
        Serial.printf("  B2 errInfo: %s\n", b2ok ? "OK" : "NO RESPONDE");
        if (b2ok) {
          uint8_t rb[8];
          getErrorInfo->getRawReply(rb);
          Serial.printf("              reply:");
          for (int j = 0; j < 8; j++) Serial.printf(" %02X", rb[j]);
          Serial.printf("  class:%02Xh code:%02Xh\n",
                        getErrorInfo->getErrorClass(), getErrorInfo->getErrorCode());
        }
      }
      for (int i = 0; i < FRAMES_TO_READ; i++) {
        uint8_t fid = frames_to_read[i]->frameid();
        bool    ok  = frames_to_read[i]->getDataOk();
        Serial.printf("  RX %02Xh : %s", fid, ok ? "OK  " : "---  ");
        if (ok) {
          frames_to_read[i]->getData(buf);
          for (int j = 0; j < 8; j++) Serial.printf(" %02X", buf[j]);
          if (fid == 0x21)
            Serial.printf("  room:%.1f°", Frame21->getRoomTemp());
        else if (fid == 0x22 && Frame22)
            Serial.printf("  agua:%.1f° heat:%d", Frame22->getWaterTemp(), Frame22->getWaterHeating());
        }
        Serial.println();
      }
      {
        ControlFrame20->getData(buf);
        Serial.printf("  TX 20h :     ");
        for (int j = 0; j < 8; j++) Serial.printf(" %02X", buf[j]);
        uint16_t rawSP = (uint16_t)buf[0] | (((uint16_t)(buf[1] & 0x0F)) << 8);
        Serial.printf("  room_sp:%.1f° fan/water:%02Xh\n",
                      rawSP / 10.0 - 273.0, buf[5]);
      }
    } else if (cmd=="sniff") {
      if (param=="on" || param=="off") {
        linSniff = (param=="on");
        found=true;
      }
    } else if (cmd=="lindebug") {
      if (param=="on") {
        LinBus.verboseMode=1;
        found=true;
      } else if (param=="off") {
        LinBus.verboseMode=0;
        found=true;
      }
    } else if(cmd=="mqttdebug") {
      #ifndef NO_MQTT
      if (param=="on" || param=="off") {
        mqttClient.enableDebuggingMessages(param=="on");
        found=true;
      }
      #endif
    } else if(cmd=="reset") {
      found=true;
      HandleCommandReset();
    } else {
      for (int i=0; i<SETPOINTS ;i++) {
        if (MqttSetpoint[i]->MqttMessage(BaseTopicSet+"/"+cmd,param,true)) {
          found=true;
          break;
        }
      }
    }
    if (!found) {
      Serial.println("unkwnown command "+cmd+" "+param);
    }
  }
  #ifdef WEBSERVER
  // Drain any messages queued from Core 0 (linBusTask) before servicing clients.
  wsQueueDrain();
  // Limit concurrent WS clients to keep heap usage bounded; matches WS_MAX_CLIENTS
  // in webserver.cpp. Inactive/stale clients past the limit are evicted here.
  #ifdef CYD
  ws.cleanupClients(2);
  #else
  ws.cleanupClients(4);
  #endif
  #endif
}

/* mqtt handling */
#ifndef NO_MQTT
esp_err_t handleMQTT(esp_mqtt_event_handle_t event) {
  if (event->event_id==MQTT_EVENT_DISCONNECTED || event->event_id == MQTT_EVENT_ERROR) {
    mqttok=false;
  }
  mqttClient.onEventCallback(event);
  return ESP_OK;
}
#endif

//start an error reset (either from an mqtt or websocket message)
void HandleCommandReset() {
  if (truma_reset) {
    Serial.println("truma reset already active");
  } else {
    truma_reset=true;
    truma_reset_delay=millis();
    EnableOnlyOnOff(true);
    truma_reset_max_time=millis();
    Serial.println("truma reset requested");
  }
}

//checks the mqtt/websocket message to adjust a setpoint
void handleSetting(const String& topic, const String& payload, boolean local)  {
    //manage the error reset here
    if (topic=="truma/set/error_reset" && payload=="1") {
      HandleCommandReset();
      return;
    }

    // Modo de energía (índice 0-4 unificado para WS y MQTT)
    if (topic=="truma/set/energy_idx") {
      int idx = payload.toInt();
      if (idx >= 0 && idx <= 4) {
        s_energyIdx     = idx;
        s_lastEnergyIdx = idx;   // evitar re-broadcast desde el CYD
        auto em = (TEnergySelection)idx;
        EnergySelect->setEnergySelection(em);
        SetPowerLimit->setPowerLimit(em);
        #ifdef CYD
        lvglLock();
        cydSetEnergyIdx(idx);
        lvglUnlock();
        #endif
        #ifdef WEBSERVER
        {
          char buf[64];
          snprintf(buf, sizeof(buf),
                   "{\"command\":\"setting\",\"id\":\"/energy_idx\",\"value\":\"%d\"}",
                   idx);
          wsQueueSend(buf);
        }
        #endif
      }
      return;
    }


    //keep the truma on (to refresh values)
    if (topic=="truma/set/ping") {
      forceon = true;
      return;
    }

    //refresh request
    if (topic=="truma/set/refresh") {
      forceon=true;
      doforcesend=true; 
      return;
    }
    //otherwise pass it on to each mqtt setting
    for (int i=0;i<SETPOINTS;i++) {
      if (MqttSetpoint[i]->MqttMessage(topic, payload,local)) 
        break;
    }
}

// message received from the mqtt broker
#ifndef NO_MQTT
void callback(const String& topic, const String& payload) {
  mqttok=true;
  Serial.print("Received mqtt message [");
  Serial.print(topic);
  Serial.print("] payload \"");
  Serial.print(payload);
  Serial.println("\"");
  Serial.flush();
  handleSetting(topic,payload,false);
}
#endif

#ifdef WEBSERVER
// message received from the websocket (treat is as local)
void wsCallback(const String& topic, const String& payload)  {
  String locTopic=BaseTopicSet+topic;
  handleSetting(locTopic,payload,true);
}
//callback when there is a new websocket client
//(actually called with a special initialization message from the client,
//right after the connection the client isn't yet ready to receive)
void wsConnected() {
  Serial.println("wsConnected callback");
  //sends the current settings
  for (int i=0; i<SETPOINTS ;i++) {
    MqttSetpoint[i]->PublishValue(false);
  }
  // Send current energy mode index
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"command\":\"setting\",\"id\":\"/energy_idx\",\"value\":\"%d\"}", (int)s_energyIdx);
    ws.textAll(buf);
  }
  // Send outdoor temperature if available
  if (s_extTemp > -200.0f) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"command\":\"status\",\"id\":\"outdoor_temp\",\"value\":\"%.1f\"}", s_extTemp);
    ws.textAll(buf);
  }
  // Send current UI language so the web follows the CYD setting
  #ifdef CYD
  {
    const char* lang = (currentLanguage() == Language::EN) ? "en" : "es";
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"command\":\"setting\",\"id\":\"/lang\",\"value\":\"%s\"}", lang);
    ws.textAll(buf);
  }
  #endif
  //force publish the next received data
  doforcesend=true;
  publishSolarBatt();
}

void publishSolarBatt() {
  {
    VictronData vd = victronGetData();
    char buf[200];
    if (vd.valid) {
      snprintf(buf, sizeof(buf),
               "{\"command\":\"solar\",\"configured\":%s,\"valid\":true,"
               "\"state\":%u,\"battV\":\"%.2f\",\"battA\":\"%.1f\","
               "\"pvW\":%d,\"kWh\":\"%.2f\"}",
               victronIsConfigured() ? "true" : "false",
               vd.state, vd.battV, vd.battA, (int)vd.pvW, vd.kWhToday);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"command\":\"solar\",\"configured\":%s,\"valid\":false}",
               victronIsConfigured() ? "true" : "false");
    }
    ws.textAll(buf);
  }
  {
    UltimatronData ud = ultimatronGetData();
    char buf[160];
    if (ud.valid) {
      snprintf(buf, sizeof(buf),
               "{\"command\":\"batt\",\"configured\":%s,\"valid\":true,"
               "\"soc\":%u,\"V\":\"%.1f\",\"A\":\"%.2f\",\"tempC\":\"%.1f\"}",
               ultimatronIsConfigured() ? "true" : "false",
               ud.soc, ud.battV, ud.battA, ud.tempC);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"command\":\"batt\",\"configured\":%s,\"valid\":false}",
               ultimatronIsConfigured() ? "true" : "false");
    }
    ws.textAll(buf);
  }
}
#endif

void PublishMqttAutoDiscovery() {
#ifdef AUTODISCOVERY
  //create a reset button in home assistant
  TAutoDiscovery ResetButton;
  ResetButton.setADTopic("/error_reset")->setADComponent(CKButton)->setADName("Error reset")->setADIcon("mdi:restart")->PublishAutoDiscovery();
  //publish autodiscovery for local topics
  PublishReset.PublishAutoDiscovery();
  PublishLinOk.PublishAutoDiscovery();
  //publish autodiscovery for setpoints/master frames/frames to read
  int i;
  for (i=0; i<SETPOINTS ;i++) {
    MqttSetpoint[i]->PublishAutoDiscovery();
  }
  for (i=0; i<MASTER_FRAMES ;i++) {
    master_frames[i]->PublishAutoDiscovery();
  }
  for (i=0; i<FRAMES_TO_READ ;i++) {
    frames_to_read[i]->PublishAutoDiscovery();
  }

#endif
}

// connection to the broker established, subscribe to the settings and
// force publish the next received data
#ifndef NO_MQTT
void onConnectionEstablishedCallback(esp_mqtt_client_handle_t client) {
  doforcesend=true;
  mqttok=true;
  PublishMqttAutoDiscovery();
#ifdef CYD
  // Clear any retained messages the broker has for transient states
  // so they don't get restored on next reboot.  Must be done BEFORE
  // subscribe so we don't immediately receive the old retained value.
  mqttClient.publish(BaseTopicSet+"/heating", "", 0, true);
  mqttClient.publish(BaseTopicSet+"/fan",     "", 0, true);
  mqttClient.publish(BaseTopicSet+"/boiler",  "", 0, true);
#endif
  mqttClient.subscribe(BaseTopicSet+"/#", callback);
  mqttClient.publish(STATUS_TOPIC, STATUS_ONLINE, 2, true);
}
#endif

//------------------------------------------------------------------------
#ifndef CYD
void LedLoop(void * pvParameters) {
  int flashes;
  while(1) {
    while (inota) {
      #ifdef RED_LED
      digitalWrite(RED_LED, LED_OFF);
      #endif
      digitalWrite(LED,LED_ON);
      delay(50);
      digitalWrite(LED,LED_OFF);
      delay(50);
    }
    delay(500);
    if (wifiok && trumaok && mqttok && !truma_reset) {
      #ifdef RED_LED
      digitalWrite(RED_LED, LED_OFF);
      #endif
      digitalWrite(LED,LED_ON);
      continue;
    }
    if (!wifiok) {
      flashes=1;
    } else if (!mqttok) {
      flashes=2;
    } else if (!trumaok) {
      flashes=3;
    } else if (truma_reset) {
      flashes=4;
    }
    while (flashes-->0) {
      #ifdef RED_LED
      digitalWrite(LED, LED_OFF);
      digitalWrite(RED_LED,LED_ON);
      delay(200);
      digitalWrite(RED_LED,LED_OFF);
      delay(200);
      #else
      digitalWrite(LED,LED_ON);
      delay(200);
      digitalWrite(LED,LED_OFF);
      delay(200);
      #endif
    }
  }
}
#endif // !CYD
