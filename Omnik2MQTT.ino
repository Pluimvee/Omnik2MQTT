#include <ESP8266WiFi.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <HAMqtt.h>
#include <ArduinoOTA.h>
#include "Omnik.h"
#include "secrets.h"
#include <DatedVersion.h>
DATED_VERSION(0, 1)
#include <Clock.h>
#include <Timer.h>
#include <FlashBuffer.h>

////////////////////////////////////////////////////////////////////////////////////////////
// WiFi credentials
const char* sta_ssid      = STA_SSID; 
const char* sta_password  = STA_PASS;
const char* ap_ssid       = AP_SSID; 
const char* ap_password   = AP_PASS; 
// Proxy settings
const char* proxy_server  = "176.58.117.69";
const int   proxy_port    = 10004;
// MQTT settings
const char* mqtt_server   = "192.168.2.170";  // test.mosquitto.org"; //"broker.hivemq.com"; //6fee8b3a98cd45019cc100ef11bf505d.s2.eu.hivemq.cloud";
int         mqtt_port     = 1883;             // 8883;
const char* mqtt_user     = MQTT_USER;
const char *mqtt_passwd   = MQTT_PASS;

///////////////////////////////////////////////////////////////////////////////////////
// omnik sends data each 5 min, 12 packages/hr, 120 packages/day
// We need a flashbuffer of 256b x 128 = 32k
#define OMNIK_CACHE_SIZE    0   
#define OMNIK_CACHE_FILE    "/Omnik.bin"

////////////////////////////////////////////////////////////////////////////////////////////
// Global instances
WiFiServer    server(proxy_port);             // the server recieving Omnik messages
WiFiClient    mqtt_client;                    // the socket to mqtt
HAOmnik       omnik;                          // The Omnik HA device
HAMqtt        mqtt(mqtt_client, omnik,  HADEVICE_SENSOR_COUNT);  // Home Assistant MTTQ    we are at 14 sensors, so set to 20
Clock         rtc;                            // A real (software) time clock
FlashBuffer   cache;                          // caching buffer which stores Omnik packages to be forwarded

////////////////////////////////////////////////////////////////////////////////////////////
// For remote logging the log include needs to be after the global MQTT definition
void REMOTE_LOG_WRITE(char *msg) { 
  int i = strlen(msg);
  if (i > 0 && msg[i-1] == '\n')   // remove trailing \n
      msg[i-1] = 0;
  mqtt.publish("OmnikProxy/log", msg, true); 
}

#define LOG_REMOTE
#define LOG_LEVEL 2
#include <Logging.h>

////////////////////////////////////////////////////////////////////////////////////////////
// MQTT connect
////////////////////////////////////////////////////////////////////////////////////////////
void mqtt_connect() {
  INFO("OmnikProxy Logger v%s saying hello\n", VERSION);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Connect to the STA network
////////////////////////////////////////////////////////////////////////////////////////////
void wifi_connect() 
{ 
  if (WiFi.isConnected())
    return;
  DEBUG("Wifi connecting to %s.", sta_ssid);
  WiFi.begin(sta_ssid, sta_password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    DEBUG(".");
  }
  DEBUG("\n");
  INFO("WiFi connected with IP address: %s\n", WiFi.localIP().toString().c_str());
}

////////////////////////////////////////////////////////////////////////////////////////////
// Enable AP and listening server
////////////////////////////////////////////////////////////////////////////////////////////
bool enable_ap() 
{
  if ((WiFi.getMode() & WIFI_AP) != WIFI_AP)  // we are expecting the Wifi to be in WIFI_AP_STA(3) mode
  {
    DEBUG("Starting AP %s\n", ap_ssid);
    IPAddress ip;
    ip.fromString(proxy_server);              // We create an AP on the server IP address
    IPAddress subnet(255, 255, 255, 0);       // Subnet mask of Access Point
    
    WiFi.softAPConfig(ip, ip, subnet);
    if (!WiFi.softAP(ap_ssid, ap_password))   // set AP credentials and enable AP
    {
      ERROR("Failed to start AP %s\n", ap_ssid);
      return false;
    }
    IPAddress apIP = WiFi.softAPIP();         // get the AP IP address
    INFO("AP %s started with ip address %s\n", ap_ssid, apIP.toString().c_str());
    delay(500);  // just started AP.....give it some time to stabilize before starting the server
  }
  if (server.status() != 1) // server not running?
  {
    DEBUG("Starting server on port %d\n", proxy_port);
    server.begin();
    if (server.status() != 1) {
      ERROR("Failed to start server\n");
      return false;
    }
    INFO("Server started listening on port %d\n", proxy_port);
    delay(500);  // just started the server... give it some time to stabilize
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////
bool disable_ap() 
{
  if (server.status() != 0)   // 0=stop, 1=listening, 2=error
  {
    DEBUG("Stopping Server\n");
    server.stop();
    delay(500);
  }
  if ((WiFi.getMode() & WIFI_AP) != WIFI_OFF)  // we expect wifi to be in WIFI_STA(1) mode
  {
    INFO("Turning AP mode off\n");
    if (!WiFi.softAPdisconnect(true))
    {
      ERROR("Failed to stop AP %s\n", ap_ssid);
      return false;
    }
    delay(500);
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////////////////
void sync_rtc() {
  rtc.ntp_sync();
  DEBUG("Clock synchronized to %s\n", rtc.now().timestamp().c_str());
}

///////////////////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////////////////
struct OmnikRecord {
  uint32_t stamp;
  uint16_t length;
  uint8_t message[250];
};

///////////////////////////////////////////////////////////////////////////////////////
// this method will forward any remaining packages in the cache to solarman
// it will ensure the AP mode is disabled to be able to reach solarman
///////////////////////////////////////////////////////////////////////////////////////
Timer T_throttle;   // timer used to prevent message bursts

bool sending_mode(DateTime &now)
{
  if (cache.isempty())
    return true;    // there is nothing to do
  
  disable_ap();     // disable server and AP to free network access to solarman

  if (!T_throttle.alarm())  // wait for timer to prevent message bursts
    return true;  

  WiFiClient omniksolar;
  if (!omniksolar.connect(proxy_server, proxy_port))
  {
    ERROR("Failed to connect to solarman\n");
    return false;
  }
  T_throttle.alarm(5000);   // we are connected, set a new timer for new connection after 5sec
  OmnikRecord record;
  if (cache.peek(&record) &&
      omniksolar.write(record.message, record.length) == record.length)
  {
    INFO("[%s] - Forwarded message of %s with length %d successfully to Solarman\n", 
                now.timestamp(DateTime::TIMESTAMP_TIME).c_str(), 
                DateTime(record.stamp).timestamp().c_str(), 
                record.length);
    DEBUG_BIN("Message: ", record.message, record.length);
    cache.pop();  // succesfully send, remove this package from he cache
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////
// this method will enable the AP and listening server
// when a new client connects, it will wait for a packages and try to parse it as Omnik message
// if successfull, ot will post to the MQTT and push the record in the cache
///////////////////////////////////////////////////////////////////////////////////////
Timer T_last_omnik; // timer since last omnik event

bool receiving_mode(DateTime &now)
{
  enable_ap();
  // Wait for a client to connect
  WiFiClient client = server.available();
  if (client == 0) 
    return true;

  INFO("[%s] - Client connected with IP %s\n", 
            now.timestamp(DateTime::TIMESTAMP_TIME).c_str(), 
            client.remoteIP().toString().c_str());
  T_last_omnik.start();

  OmnikRecord record;
  record.stamp = now.unixtime();
  record.length = 0;
  delay(500);
  // Read data from the client 
  while (client.available() && record.length < sizeof(record.message))
    record.message[record.length++] = client.read();

  DEBUG_BIN("Message: ", record.message, record.length);

  if (!omnik.handle(record.message, record.length)) 
    return false;
  
  if (!cache.push(&record, true)) 
    return false;

  if (cache.isfull())
    INFO("WARNING: Cache limit reached - its FULL\n");

  return true;
}

///////////////////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////////////////
DateTime T_reboot;

enum ProxyMode {
  RECEIVING,
  SENDING,
  REBOOT
};

int scheduler(DateTime &now)
{
  if (T_reboot < now)         // reboot time is in the past, we are initializing
  {
    if (!cache.isempty())     // we first flush the complete cache
      return SENDING;         // 
    
    // set reboot for 1AM by start of today + 1 day & 1 hour 
    T_reboot = DateTime(now.year(), now.month(), now.day()) + TimeSpan(1,1,0,0);
    INFO("Reboot scheduled for %s\n", T_reboot.timestamp().c_str());
  }
  if (now > T_reboot)  // is it time to reboot?
    return REBOOT;

  // NOTE: align the following timing variables
  //  * scheduling (30 minutes)
  //  * cache capacity (31)
  //  * Omnik updates (each 5 minutes)  12/hr
  //  * interval for sending to solarman (each 5 seconds)
  switch (now.minute()) {
  case 30:                          // each 30 minutes 
  case 00: 
    if (cache.isempty())            // check if we have data to send
      break;
    if (T_last_omnik.seconds() < 2) // check if omnik is sending data at this moment
      break;
    if ((T_last_omnik.minutes() < 4) || (T_last_omnik.minutes() > 5))  // each 4,5-5,5 seconds we get new data
      return SENDING;               // All safe!! Lets inform solarman of the omnik data we have cached
    // fall thru
  default:
    break;
  }
  return RECEIVING;         // most of the time we are in receiving mode
}

///////////////////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////////////////
void setup() 
{
  Serial.begin(115200);
  INFO("\n\nVersion %s\n", VERSION);
  wifi_connect();
  sync_rtc();

  INFO("Starting caching library '%s'\n", OMNIK_CACHE_FILE);
  if (!cache.begin(OMNIK_CACHE_FILE, sizeof(OmnikRecord), OMNIK_CACHE_SIZE))  
    ERROR("Failed mounting cache\n");

  INFO("Connecting to MQTT server %s\n", mqtt_server);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  omnik.begin(mac, &mqtt);             // 5) make sure the device gets a unique ID (based on mac address)
  mqtt.onConnected(mqtt_connect);      // register function called when newly connected
  mqtt.begin(mqtt_server, mqtt_port, mqtt_user, mqtt_passwd);  // 

  INFO("Initialize OTA\n");
  // start OTA service
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname("Omnik-Logger");
  ArduinoOTA.setPassword((const char *)"1234");

  ArduinoOTA.onStart([]() {
    INFO("Starting remote software update");
  });
  ArduinoOTA.onEnd([]() {
    INFO("Remote software update finished");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
  });
  ArduinoOTA.onError([](ota_error_t error) {
    ERROR("Error remote software update");
  });
  ArduinoOTA.begin();
  INFO("Setup complete\n\n");
}

///////////////////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////////////////
void loop() 
{
  // ensure we are still connected (STA-mode)
  wifi_connect();
  // handle any OTA requests
  ArduinoOTA.handle();
  // handle MQTT
  mqtt.loop();

  // now lets deterime what we are going to do
  DateTime now = rtc.now();

  switch (scheduler(now))
  {
  case SENDING:
    sending_mode(now);
    break;

  case REBOOT:
    INFO("REBOOT event triggered - see you in a few seconds\n");
    delay(1000);
    ESP.restart();
    break;
  
  default:
  case RECEIVING:
    receiving_mode(now);
    break;
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
