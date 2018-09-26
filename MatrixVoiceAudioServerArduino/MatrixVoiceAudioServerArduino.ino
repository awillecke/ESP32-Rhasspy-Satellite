/* ************************************************************************* *
 * Matrix Voice Audio Streamer
 * 
 * This program is written to be a streaming audio server running on the Matrix Voice.
 This is typically used for Snips.AI, it will then be able to replace
 * the Snips Audio Server, by publishing small wave messages to the hermes proticol
 * See https://snips.ai/ for more information
 * 
 * Author:  Paul Romkes
 * Date:    September 2018
 * Version: 2
 * 
 * Changelog:
 * ==========
 * v1:
 *  - first code release. It needs a lot of improvement, no hardcoding stuff
 * v2:
 *  - Change to Arduino IDE
 * v2.1:
 *  - Changed to pubsubclient and fixed other stability issues
 * ************************************************************************ */
#include <WiFi.h>
extern "C" {
  #include "freertos/FreeRTOS.h"
  #include "freertos/timers.h"
  #include "freertos/event_groups.h"
}
#include <AsyncMqttClient.h>
#include <PubSubClient.h>
#include "wishbone_bus.h"
#include "everloop.h"
#include "everloop_image.h"
#include "microphone_array.h"
#include "microphone_core.h"
#include "voice_memory_map.h"
#include "config.h"

//Some needed defines, should be obvious which ones to change
#define RATE 16000
#define SITEID "matrixvoice"
#define MQTT_IP IPAddress(192, 168, 178, 194)
#define MQTT_HOST "192.168.178.194"
#define MQTT_PORT 1883
#define CHUNK 256 //set to multiplications of 256, voice return a set of 256
#define WIDTH 2
#define CHANNELS 1

WiFiClient net;
AsyncMqttClient asyncClient; //ASYNCH client to be able to handle huge messages like WAV files
PubSubClient audioServer(net); //We also need a sync client, asynch leads to errors on the audio thread

//Timers
TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;

//Globals
const int HOTWORD_ON = BIT0;
const int HOTWORD_OFF = BIT1;
static EventGroupHandle_t hwEventGroup;

//Matrix Voice stuff
namespace hal = matrix_hal;
uint16_t voicebuffer[CHUNK];
uint8_t voicemapped[CHUNK*WIDTH];
hal::WishboneBus wb;
hal::Everloop everloop;
hal::MicrophoneArray mics;

//Dynamic topics for MQTT
std::string audioFrameTopic = std::string("hermes/audioServer/") + SITEID + std::string("/audioFrame");
std::string playFinishedTopic = std::string("hermes/audioServer/") + SITEID + std::string("/playFinished");
std::string playBytesTopic = std::string("hermes/audioServer/") + SITEID + std::string("/playBytes/#");

bool DEBUG = false; //If set to true, code will post several messages to topics in case of events
bool boot = true;
bool disconnected = false;

struct wavfile_header {
  char  riff_tag[4];    //4
  int   riff_length;    //4
  char  wave_tag[4];    //4
  char  fmt_tag[4];     //4
  int   fmt_length;     //4
  short audio_format;   //2
  short num_channels;   //2
  int   sample_rate;    //4
  int   byte_rate;      //4
  short block_align;    //2
  short bits_per_sample;//2
  char  data_tag[4];    //4
  int   data_length;    //4
};

struct wavfile_header header;

// ---------------------------------------------------------------------------
// EVERLOOP (The Led Ring)
// ---------------------------------------------------------------------------
void setEverloop(int red, int green, int blue, int white) {
    hal::EverloopImage image1d;
    for (hal::LedValue& led : image1d.leds) {
      led.red = red;
      led.green = green;
      led.blue = blue;
      led.white = white;
    }

    everloop.Write(&image1d);
}

// ---------------------------------------------------------------------------
// Set the HOTWORD bits
// ---------------------------------------------------------------------------
static void HotWordSetDetected(uint8_t c)
{
    if (c) {
      xEventGroupSetBits(hwEventGroup, HOTWORD_ON);
    } else {
      xEventGroupSetBits(hwEventGroup, HOTWORD_OFF);
    }
}

// ---------------------------------------------------------------------------
// Network functions
// ---------------------------------------------------------------------------
void connectToWifi() {
  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(SSID, PASSWORD);
}

void connectToMqtt() {
  Serial.println("Connecting to MQTT...");
  asyncClient.connect();
}

void connectAudio() {
 while (!audioServer.connect("MatrixVoiceAudio")) {
    delay(1000);
 }
}

// ---------------------------------------------------------------------------
// WIFI event
// Kicks off various stuff in case of connect/disconnect
// ---------------------------------------------------------------------------
void WiFiEvent(WiFiEvent_t event) {
  Serial.printf("[WiFi-event] event: %d\n", event);
  switch(event) {
  case SYSTEM_EVENT_STA_GOT_IP:
      setEverloop(0,0,10,0); //Turn the ledring BLUE
      Serial.println("WiFi connected");
      Serial.println("IP address: ");
      Serial.println(WiFi.localIP());
      connectToMqtt();
      connectAudio();
      break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println("WiFi lost connection");
      xTimerStop(mqttReconnectTimer, 0); //Do not reconnect to MQTT while reconnecting to network
      xTimerStart(wifiReconnectTimer, 0); //Start the reconnect timer
      break;
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// MQTT Connect event
// ---------------------------------------------------------------------------
void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  asyncClient.subscribe(playBytesTopic.c_str(), 0);
  asyncClient.subscribe("hermes/hotword/toggleOff",0);
  asyncClient.subscribe("hermes/hotword/toggleOn",0);
  if (DEBUG) {
    if (boot) {
      boot = false;
      asyncClient.publish("debug/asynch/status",0, false, "boot");
    }
    if (disconnected) {
      asyncClient.publish("debug/asynch/status",0, false, "reconnect");
      disconnected = false;
    }
  }
}

// ---------------------------------------------------------------------------
// MQTT Disonnect event
// ---------------------------------------------------------------------------
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");
  if (DEBUG) {
    disconnected = true;
  }
  if (WiFi.isConnected()) {
    xTimerStart(mqttReconnectTimer, 0);
  }
}

// ---------------------------------------------------------------------------
// MQTT Callback
// Sets the HOTWORD bits to toggle the leds and published a message on playFinished
// to simulate the playFinished. Without it, Snips will not start listening when the 
// feedback sound is toggled on
// ---------------------------------------------------------------------------
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  String topicstr(topic);
  if (len + index >= total) {
    if (topicstr.indexOf("toggleOff") > 0) {
      HotWordSetDetected(1);
    } else if (topicstr.indexOf("toggleOn") > 0) {
      HotWordSetDetected(0);
    } else if (topicstr.indexOf("playBytes") > 0) {
      int pos = 19 + String(SITEID).length() + 11;
      String s = "{\"id\":\"" + topicstr.substring(pos) + "\",\"siteId\":\"" + SITEID + "\",\"sessionId\":null}";
      if (asyncClient.connected()) {
        asyncClient.publish(playFinishedTopic.c_str(), 0, false, s.c_str());
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Hotword tasks, implemented as task due to stability issues
// ---------------------------------------------------------------------------
void hotwordOn(void *pvParam){
    while(1){
        xEventGroupWaitBits(hwEventGroup,HOTWORD_ON,true,true,portMAX_DELAY);
        setEverloop(0,10,0,0);
    }
    vTaskDelete(NULL);
}

void hotwordOff(void *pvParam){
    while(1){
        xEventGroupWaitBits(hwEventGroup,HOTWORD_OFF,true,true,portMAX_DELAY);
        setEverloop(0,0,10,0);
    }
    vTaskDelete(NULL);
}
// ---------------------------------------------------------------------------
// Audiostream -  uses SYNC MQTT client
// ---------------------------------------------------------------------------
void Audiostream( void * parameter ) {
  for(;;){
    if (audioServer.connected()) {
      //We are connected! 
      mics.Read();
      
      //NumberOfSamples() = kMicarrayBufferSize / kMicrophoneChannels = 4069 / 8 = 512
      for (uint32_t s = 0; s < CHUNK; s++) {
          voicebuffer[s] = mics.Beam(s);
      }

      //voicebuffer will hold 256 samples of 2 bytes, but we need it as 1 byte
      //We do a memcpy, because I need to add the wave header as well
      memcpy(voicemapped,voicebuffer,CHUNK*WIDTH);
  
      uint8_t payload[sizeof(header)+(CHUNK*WIDTH)];
      uint8_t payload2[sizeof(header)+(CHUNK*WIDTH)];
      //Add the wave header
      memcpy(payload,&header,sizeof(header));
      memcpy(&payload[sizeof(header)], voicemapped, sizeof(voicemapped));

      audioServer.publish(audioFrameTopic.c_str(), (uint8_t *)payload, sizeof(payload));
      
      //also send to second half as a wav
      for (uint32_t s = CHUNK; s < CHUNK*WIDTH; s++) {
          voicebuffer[s-CHUNK] = mics.Beam(s);
      }
  
      memcpy(voicemapped,voicebuffer,CHUNK*WIDTH);
  
      //Add the wave header
      memcpy(payload2,&header,sizeof(header));
      memcpy(&payload2[sizeof(header)], voicemapped, sizeof(voicemapped));
      
      audioServer.publish(audioFrameTopic.c_str(), (uint8_t *)payload2, sizeof(payload2));
    }
  }
  vTaskDelete(NULL);
}
 
void setup() {
  hwEventGroup = xEventGroupCreate();
  
  wb.Init();
  everloop.Setup(&wb);
  
  //setup mics
  mics.Setup(&wb);
  mics.SetSamplingRate(RATE);
  //mics.SetGain(5);

   // Microphone Core Init
  hal::MicrophoneCore mic_core(mics);
  mic_core.Setup(&wb);

  setEverloop(10,0,0,0); //RED

  strncpy(header.riff_tag,"RIFF",4);
  strncpy(header.wave_tag,"WAVE",4);
  strncpy(header.fmt_tag,"fmt ",4);
  strncpy(header.data_tag,"data",4);

  header.riff_length = (uint32_t)sizeof(header) + (CHUNK * WIDTH);
  header.fmt_length = 16;
  header.audio_format = 1;
  header.num_channels = 1;
  header.sample_rate = RATE;
  header.byte_rate = RATE * WIDTH;
  header.block_align = WIDTH;
  header.bits_per_sample = WIDTH * 8;
  header.data_length = CHUNK * WIDTH;

  Serial.begin(115200);

  //Reconnect timers
  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToWifi));
 
  WiFi.onEvent(WiFiEvent);

  asyncClient.onConnect(onMqttConnect);
  asyncClient.onDisconnect(onMqttDisconnect);
  asyncClient.onMessage(onMqttMessage);
  asyncClient.setServer(MQTT_IP, MQTT_PORT);
  audioServer.setServer(MQTT_IP, 1883);  
  connectToWifi(); 

  //Create the runnings tasks
  xTaskCreate(Audiostream,"Audiostream",10000,NULL,5,NULL);
  xTaskCreate(hotwordOn,"hotwordOn",4096,NULL,5,NULL);
  xTaskCreate(hotwordOff,"hotwordOff",4096,NULL,5,NULL);  
}

void loop() {
  if (!audioServer.connected()) {
    if (asyncClient.connected()) {
      asyncClient.publish("debug/audioserver/status",0, false, "reconnect");
    }
    connectAudio();
  }  
}
