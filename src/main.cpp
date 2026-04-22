#include <Arduino.h>
#include <ESP32MQTTClient.h>
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
#endif
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
// ESP32-2432S028R (Cheap Yellow Display)
// Status shown on LVGL display — no LED used.
// LIN bus UART: TX=27, RX=22 (available on CN2/P3 connectors, not used by display/touch/SD)
#define TX_PIN 27
#define RX_PIN 22

// ── LVGL task (Core 1) ────────────────────────────────────────────────────
// lv_timer_handler() must be called every few ms for smooth touch response.
// The LIN bus loop takes ~150 ms per iteration so we run LVGL on its own task.
static SemaphoreHandle_t s_lvglMutex = nullptr;

static void lvglTask(void*) {
    static uint32_t last = millis();
    for (;;) {
        uint32_t now = millis();
        lv_tick_inc(now - last);
        last = now;
        if (xSemaphoreTake(s_lvglMutex, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvglMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// Call before any LVGL function from the main loop.
static inline void lvglLock()   { xSemaphoreTake(s_lvglMutex, portMAX_DELAY); }
static inline void lvglUnlock() { xSemaphoreGive(s_lvglMutex); }

#endif
#ifndef TX_PIN
#error Please define GOOUUUC3, C3SUPERMINI, WROOM32 or your custom LED/TX_PIN/RX_PIN
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
#define FRAMES_TO_READ 5
#define FRAMES_TO_WRITE 6
#define MASTER_FRAMES 5
#endif
//the frames to be read
TFrameBase * frames_to_read[FRAMES_TO_READ];
 //I only care about frame 16 content, the remaining frames will be 
 //directly allocated in frames_to_read
TFrame16 *Frame16;
//the frames to be written
TFrameBase * frames_to_write[FRAMES_TO_WRITE];
TFrameSetTemp *SimulateTempFrame;
TFrameSetTemp *RoomSetpointFrame;
TFrameSetTemp *WaterSetpointFrame;
TFrameEnergySelect *EnergySelect;
TFrameSetPowerLimit *SetPowerLimit;
TFrameSetFan *FanFrame;
#ifdef COMBIGAS
TFrameSetControlElements *ControlElementsFrame;
#endif
//the setpoints
#define SETPOINTS 5
TMqttSetting * MqttSetpoint[SETPOINTS];
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
//heartbeat (for the mqtt clients to detect this program is working)
TPubBool PublishHeartBeat("/heartbeat");

//start an error reset
void HandleCommandReset();
// Forward declaration: linBusTask is defined after loop() but used in setup()
static void linBusTask(void*);
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
void setup() {
  Serial.begin(115200);
 
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
  s_lvglMutex = xSemaphoreCreateMutex();
  auto disp = lv_display_get_default();
  // LV_DISPLAY_ROTATION_270 gives MV=1,MX=0,MY=0 on the ILI9341 = correct
  // landscape for the ESP32-2432S028R.  (ROTATION_90 would give MV=1,MX=1,MY=1
  // which rotates the image 270° instead of 90°, producing the "vertical line"
  // artifact when our horizontal separator is rendered in portrait coordinates.)
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
  // Load touch calibration from NVS.  If none is stored yet (first boot or
  // after clearing NVS), run the interactive 3-point calibration screen.
  // raw coordinates pass through when valid=false, which is what
  // runTouchCalibration() needs to record them.
  if (!loadTouchCalibration(touch_calibration_data)) {
    runTouchCalibration();
  }
  #endif

  //frames to read
  //frames 0x14 and 0x37 are defined but are useless for this model of combi D
  //so they're not read
  Frame16=new TFrame16();
  frames_to_read[0] = Frame16;
  #ifndef COMBIGAS
  frames_to_read[1] = new TFrame34();
  frames_to_read[2] = new TFrame39();
  frames_to_read[3] = new TFrame35();
  frames_to_read[4] = new TFrame3b();
  #endif

  //frames to write
  SimulateTempFrame = new TFrameSetTemp(0x02);
  SimulateTempFrame->setTemperature(-273.0);
  RoomSetpointFrame = new TFrameSetTemp(0x03);
  WaterSetpointFrame = new TFrameSetTemp(0x04);
  EnergySelect = new TFrameEnergySelect(0x05);
  SetPowerLimit = new TFrameSetPowerLimit(0x06);
  FanFrame = new TFrameSetFan(0x07);
  frames_to_write[0] = SimulateTempFrame;
  frames_to_write[1] = RoomSetpointFrame;
  frames_to_write[2] = WaterSetpointFrame;
  frames_to_write[3] = EnergySelect;
  frames_to_write[4] = SetPowerLimit;
  frames_to_write[5] = FanFrame;
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
  master_frames[0] = new TAssignFrameRanges(0x09, {0x3b, 0x3a, 0x39, 0x38});
  master_frames[1] = new TAssignFrameRanges(0x0d, {0x37, 0x36, 0x35, 0x34});
  master_frames[2] = new TAssignFrameRanges(0x11, {0x33, 0x32, 0xff, 0xff});
  master_frames[3] = onOff;
  getErrorInfo = new TGetErrorInfo();
  master_frames[4] = getErrorInfo;
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
  // RoomSetpoint: se recuerda entre reinicios.
  RoomSetpoint->setPersist(true);
  RoomSetpoint->loadPersistedValue();   // carga último valor guardado
  // HeatingOn / WaterSetpoint / FanMode: arrancan siempre en el valor por
  // defecto (off) — no se publican con retain para que el broker MQTT no
  // restaure el estado en el siguiente reinicio.
  HeatingOn->setRetain(false);
  WaterSetpoint->setRetain(false);
  FanMode->setRetain(false);

  // Now that all settings objects exist, init the display controls.
  cydDisplayInit(RoomSetpoint, WaterSetpoint, HeatingOn, FanMode);
  #endif

  //autodiscovery for local topics
  PublishReset.setADComponent(CKBinary_sensor)->setADName("Resetting")->setADIcon("mdi:sync")->setADDevice_class("connectivity");
  PublishLinOk.setADComponent(CKBinary_sensor)->setADName("LIN bus status")->setADIcon("mdi:serial-port")->setADDevice_class("connectivity");

  //starts the wifi (loop will check if it's connected)
  WiFi.mode(WIFI_STA);
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

  // LIN bus task: pinned to Core 0 (WiFi core) so the blocking serial reads
  // don't interfere with LVGL (Core 1) or WebSocket response latency.
  xTaskCreatePinnedToCore(linBusTask, "LinBus", 4096, nullptr, 1, nullptr, 0);

  #ifdef CYD
  // Start LVGL task only now — all LVGL init (calibration, wifi/mqtt setup
  // screens, cydDisplayInit) is complete, so no race condition.
  xTaskCreatePinnedToCore(lvglTask, "LVGL", 16384, nullptr, 2, nullptr, 1);
  #endif
}

//enable/disables the master frames to assign frame ranges
void AssignFrameRanges(boolean on) {
#ifndef COMBIGAS
  for (int i=0; i<3; i++) {
    master_frames[i]->setEnabled(on && !truma_reset);
  }
#endif
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
      //start the web server the first time the wifi is connected
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
  for (;;) {
    esp_task_wdt_reset();

    byte PumpOrFan;
    double LocSetPointTemp = 0.0;
    double LocRoomSetpoint = RoomSetpoint->getFloatValue();
    double LocWaterSetpoint = WaterSetpoint->getFloatValue();
    boolean LocHeatingOn = HeatingOn->getIntValue()!=0;
    int LocFanMode = FanMode->getIntValue();

    //determines operating mode
    if (!LocHeatingOn) {
       if (LocWaterSetpoint<=0.0) {
         if (LocFanMode>0) {
          PumpOrFan = 0x10 | LocFanMode;
         } else {
          PumpOrFan = 0x10;
         }
       } else {
        PumpOrFan=0;
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

    //Water boost
    if (LocWaterSetpoint>=60.0) {
       WaterBoost->Start(Frame16->getWaterDemand());
    } else {
       WaterBoost->Stop();
    }
    if (WaterBoost->Active(Frame16->getWaterDemand()) && Frame16->getWaterTemp()<50.0) {
        LocSetPointTemp=0.0;
        PumpOrFan=0;
    }

    //prepare setpoint frames
    SimulateTempFrame->setTemperature(SimulatedTemp->getFloatValue());
    RoomSetpointFrame->setTemperature(LocSetPointTemp);
    WaterSetpointFrame->setTemperature(LocWaterSetpoint);
    FanFrame->setPumpOrFan(PumpOrFan);

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
    if (LocHeatingOn || LocWaterSetpoint > 0.0 || LocFanMode > 0 || forceon) {
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

      trumaok=Frame16->getDataOk();

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
    PublishHeartBeat.setValue(true);

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

//-------------------------------------------------------------------------
void loop() {
  esp_task_wdt_reset();
  delay(1);
  #ifdef CYD
  // lv_timer_handler() runs in lvglTask (Core 1) every 5 ms.
  // We only need to update display state; grab the mutex briefly.
  if (xSemaphoreTake(s_lvglMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    cydDisplayUpdate(wifiok, mqttok, trumaok, truma_reset, inota, mqttEnabled,
                     (float)Frame16->getRoomTemp(), (float)Frame16->getWaterTemp());
    xSemaphoreGive(s_lvglMutex);
  }
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
       Serial.println("lindebug on|off");
       Serial.println("mqttdebug on|off");
       Serial.println("reset");
       Serial.println("simultemp -273.0 (off)..30.0");
       Serial.println("temp 5.0..30.0");
       Serial.println("heating 0|1");
       Serial.println("boiler off|eco|high|boost");
       Serial.println("fan off|eco|high|1..10");   
       found=true;   
    } else if (cmd=="lindebug") {
      if (param=="on") {
        LinBus.verboseMode=1;
        found=true;
      } else if (param=="off") {
        LinBus.verboseMode=0;
        found=true;
      }
    } else if(cmd=="mqttdebug") {
      if (param=="on" || param=="off") {
        mqttClient.enableDebuggingMessages(param=="on");
        found=true;
      }
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
  ws.cleanupClients();
  #endif
}

/* mqtt handling */
esp_err_t handleMQTT(esp_mqtt_event_handle_t event) {
  if (event->event_id==MQTT_EVENT_DISCONNECTED || event->event_id == MQTT_EVENT_ERROR) {
    mqttok=false;
  } 
  mqttClient.onEventCallback(event);
  return ESP_OK;
}

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
  //force publish the next received data
  doforcesend=true;
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
