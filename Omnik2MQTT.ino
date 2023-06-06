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
#include <LED.h>
#define LOG_REMOTE
#define LOG_LEVEL 2
#include <Logging.h>

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
// We may need a flashbuffer of 256b x 128 = 32k
#define OMNIK_CACHE_SIZE    0   
#define OMNIK_CACHE_FILE    "/Omnik.bin"

////////////////////////////////////////////////////////////////////////////////////////////
// Global instances
WiFiServer    server(proxy_port);             // the server recieving Omnik messages
WiFiClient    mqtt_client;                    // the socket to mqtt
HAOmnik       omnik;                          // The Omnik HA device
HAMqtt        mqtt(mqtt_client, omnik,  HADEVICE_SENSOR_COUNT);  // Home Assistant MTTQ    we are at 14 sensors, so set to 20
FlashBuffer   cache;                          // caching buffer which stores Omnik packages to be forwarded
Clock         rtc;                            // A real (software) time clock
LED           led;                            // 

////////////////////////////////////////////////////////////////////////////////////////////
// For remote logging we use MQTT
void LOG_CALLBACK(char *msg) { 
  LOG_REMOVE_NEWLINE(msg);
  mqtt.publish("OmnikProxy/log", msg, true); 
}

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
  if (((WiFi.getMode() & WIFI_STA) == WIFI_STA) && WiFi.isConnected())
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
  int retry=0;
  while ((server.status() != 0) && (retry++ < 5))   // 0=stop, 1=listening, 2=error
  {
    server.stop();
    delay(200);
  }
  if ((WiFi.getMode() & WIFI_AP) != WIFI_OFF)  // we expect wifi to be in WIFI_STA(1) mode
  {
    INFO("Turning AP mode off\n");
    if (!WiFi.softAPdisconnect(true))       // TODO: we sometimes crach here, especially when Omnik just reached out to the server
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
int omnik_failure = 0;

bool sending_mode(DateTime &now)
{
  if (cache.isempty())
    return true;    // there is nothing to do
  
  disable_ap();     // disable server and AP to free network access to solarman

  if (!T_throttle.passed())  // wait for timer to prevent message bursts
    return true;  

  T_throttle.set(3000);   // set throttle timer to 5 sec

  WiFiClient omniksolar;
  if (!omniksolar.connect(proxy_server, proxy_port))
  {
    T_throttle.set(1000);   // set throttle timer to 2 sec on connect failure
    ERROR("Failed to connect to solarman\n");
    if (++omnik_failure > 40) {
      WiFi.disconnect();    // reset wifi on so many sequential failures, we may also use a hard reset
      wifi_connect();       // However, it may be that solarman is OOO and we should first try to ping google
      omnik_failure = 0;
    }
    return false;
  }
  omnik_failure = 0;
  OmnikRecord record;
  if (cache.peek(&record) &&
      omniksolar.write(record.message, record.length) == record.length)
  {
    INFO("[%s] - Forwarded message of %s with length %d successfully to Solarman\n", 
                now.timestamp(DateTime::TIMESTAMP_TIME).c_str(), 
                DateTime(record.stamp).timestamp().c_str(), 
                record.length);
    DEBUG_BIN("Message: ", record.message, record.length);
    cache.pop();  // succesfully send, remove this package from the cache
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
    return false;

  INFO("[%s] - Client connected with IP %s\n", 
            now.timestamp(DateTime::TIMESTAMP_TIME).c_str(), 
            client.remoteIP().toString().c_str());
  T_last_omnik.start();
  omnik.enable();

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
    ERROR("BUFFER OVERFLOW - Cache limit reached: oldest record has been overwritten\n");

  return true;
}

///////////////////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////////////////
DateTime bedtime;

void sync_clock()
{
  rtc.ntp_sync();
  DateTime now = rtc.now();
  DEBUG("Clock synchronized to %s\n", now.timestamp().c_str());

  // set deepsleep for 1:00 the following day
  // if its between 0:00 and 0:59 the first 1:00 is skipped
  bedtime = DateTime(now.year(), now.month(), now.day(), 1, 0, 0) + TimeSpan(1,0,0,0);

  INFO("[%s] - Clock synchronized and Bed time set for %s\n", 
              now.timestamp(DateTime::TIMESTAMP_TIME).c_str(), 
              bedtime.timestamp().c_str());
}

///////////////////////////////////////////////////////////////////////////////////////
bool deep_sleep(DateTime &now)
{
#if (1)   // enable to use reboot
  INFO("REBOOT in 2 seconds, after which we start fresh....\n");
  // to flush remote logging to MQTT we handle MQTT
  mqtt.loop();    
  // wait 2 seconds
  delay(2000);  
  ESP.reset();    // use reset() instead of restart() to include hardware reset (eg WiFi) 
#endif

#if (0) // enable to use deepsleep
  // deepsleep requires D0 (GPIO16) to be hard wired with the RST pin. According to some web pages this may cause issues uploading firmware
  // we need to test if this also applies to OTA. So for now we stick to a Restart instead of Sleep

  INFO("[%s] - Its time to enter Deepsleep, set wakeup time for 5:30 AM\n", now.timestamp(DateTime::TIMESTAMP_TIME).c_str());
  // disable server and AP 
  disable_ap();     
  delay(500);
  // TODO: instead of 5:30 use sunrise - 30 minutes
  DateTime wakeup(now.year(), now.month(), now.day(), 5, 30, 0);    // set wakeup for today 5:30, which is correct if ots past 0:00
  if (wakeup < now)                                                 // if that time has already past than its before 0:00
    wakeup = wakeup + TimeSpan(1,0,0,0);                            // and we need to wake up the following day
  int secondsUntilWakeup = wakeup.unixtime() - now.unixtime();      // however the unixtime returned will still be valid

  INFO("We are about to completely turn of the Wifi, so this is the last you here from me\n");
  INFO("According to my calculations I will wake up in %d seconds.... CU!!\n", secondsUntilWakeup);
  // to flush remote logging to MQTT we handle MQTT
  mqtt.loop();    
  // wait 2 seconds
  delay(2000);  
  // SLEEPING........
  ESP.deepSleep(secondsUntilWakeup * 1e6, RF_DISABLED);
#endif
  // default -> Sync clock and reconfigure next sync/reboot/sleep time
  sync_clock(); 
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////
// the main method which determines what we will be doing
///////////////////////////////////////////////////////////////////////////////////////
enum ProxyMode {
  RECEIVING,
  SENDING,
  SLEEP
};

int scheduler(DateTime &now)
{
  if (now > bedtime)  // its past bedtime
    return SLEEP;

//  if ((millis() < 60000) && (!cache.isempty())) // we just rebooted and we have data in the cache, flush data for 1 minute (-boottime)
//    return SENDING;                             // we've seen the rare occasion in which Omnik connected and transmitted data in this first minute 
                                                // which filled the cache and we immediatly switched into Sending mode, interfering with Omnik transmition

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
    if (T_last_omnik.seconds() < 20) // check if omnik is sending data at this moment
      break;
    if ((T_last_omnik.minutes() < 4) || (T_last_omnik.minutes() > 5))  // each 4,5-5,5 seconds we get new data
      return SENDING;               // All safe!! Lets inform solarman of the omnik data we have cached
    // fall thru
  default:
    break;
  }
  if (T_last_omnik.minutes() > 30)  // we didnt receive anything in the past 30 minutes
    omnik.disable();

  return RECEIVING;         // most of the time we are in receiving mode
}

///////////////////////////////////////////////////////////////////////////////////////
// Main Setup 
///////////////////////////////////////////////////////////////////////////////////////
void setup() 
{
  Serial.begin(115200);
  INFO("\n\nOmnik Inverter Logger Version %s\n", VERSION);
  wifi_connect();

  // start MQTT to enable remote logging asap
  INFO("Connecting to MQTT server %s\n", mqtt_server);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  omnik.begin(mac, &mqtt);             // 5) make sure the device gets a unique ID (based on mac address)
  mqtt.onConnected(mqtt_connect);      // register function called when newly connected
  mqtt.begin(mqtt_server, mqtt_port, mqtt_user, mqtt_passwd);  // 
  omnik.disable();

  sync_clock(); 

  INFO("Starting caching library '%s'\n", OMNIK_CACHE_FILE);
  if (!cache.begin(OMNIK_CACHE_FILE, sizeof(OmnikRecord), OMNIK_CACHE_SIZE))  
    ERROR("Failed mounting cache\n");

  INFO("Initialize OTA\n");
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname("Omnik-Logger");
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
    INFO("[%s] - Starting remote software update",
          rtc.now().timestamp(DateTime::TIMESTAMP_TIME).c_str());
  });
  ArduinoOTA.onEnd([]() {
    INFO("[%s] - Remote software update finished",
          rtc.now().timestamp(DateTime::TIMESTAMP_TIME).c_str());
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
// Main loop
///////////////////////////////////////////////////////////////////////////////////////
Timer blink;

void loop() 
{
  // ensure we are still connected (STA-mode)
  wifi_connect();
  // handle any OTA requests
  ArduinoOTA.handle();
  // handle MQTT
  mqtt.loop();
  // whats the time
  DateTime now = rtc.now();
  // now lets deterime what we are going to do
  switch (scheduler(now))
  {
  case SENDING:
    sending_mode(now);
    led.on();                           // led on for sending
    break;
  case SLEEP:
    deep_sleep(now);
    break;
  default:
  case RECEIVING:
    receiving_mode(now);
    if (T_last_omnik.elapsed() < 2000)  // blink fast on omnik transmitions
      led.blink();      
    else if (blink.passed()) {
      led.blink();      
      blink.set(1500);                  // blink slow on listening/awaiting
    }
    break;
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
