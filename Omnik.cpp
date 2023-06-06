#include "Omnik.h"
#include <HAMqtt.h>

#define LOG_LEVEL 2
#define LOG_REMOTE
#define LOG_PREFIX  "[Omnik] - "
#include <Logging.h>

#include <DatedVersion.h>
DATED_VERSION(0, 1)
#define DEVICE_NAME  "Omnik 2.0TL inverter"
#define DEVICE_MODEL "Omnik Logger esp8266"
////////////////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////////////////
#define CONSTRUCT(var)          var("omnik-" #var)
#define CONSTRUCT_P0(var)       var("omnik-" #var, HABaseDeviceType::PrecisionP0)
#define CONSTRUCT_P1(var)       var("omnik-" #var, HABaseDeviceType::PrecisionP1)
#define CONSTRUCT_P2(var)       var("omnik-" #var, HABaseDeviceType::PrecisionP2)

#define CONFIGURE_ICON(var, name, icon)               var.setName(name); var.setIcon("mdi:" icon)
#define CONFIGURE_CLS(var, name, class, unit, icon)   var.setName(name); var.setDeviceClass(class); var.setIcon("mdi:" icon); var.setUnitOfMeasurement(unit)
#define CONFIGURE_STAT(var, name, class, unit, icon)  CONFIGURE_CLS(var, name, class, unit, icon); var.setStateClass("total_increasing")
////////////////////////////////////////////////////////////////////////////////////////////
HAOmnik::HAOmnik() 
: CONSTRUCT(loggerId), CONSTRUCT(inverterId),     
  CONSTRUCT_P0(power), CONSTRUCT_P1(temperature), CONSTRUCT_P2(E_today),  
  CONSTRUCT_P1(E_total), CONSTRUCT_P0(operating_hrs)
{
  CONFIGURE_ICON(loggerId,    "logger-id",    "barcode");
  loggerId.hasAttributes();
  CONFIGURE_ICON(inverterId,  "inverter-id",  "barcode");
  inverterId.hasAttributes();

  CONFIGURE_CLS(temperature,  "temperature",  "temperature",  "°C",   "thermometer");
  CONFIGURE_CLS(power,        "power",        "power",        "W",    "solar-power");
  
  CONFIGURE_STAT(E_today,     "E-today",      "energy",   	  "kWh",  "transmission-tower-import");
  CONFIGURE_STAT(E_total,     "E-total",      "energy",   	  "kWh",  "transmission-tower-import");

  CONFIGURE_CLS(operating_hrs, "operating-hours", "duration",  "h",   "clock-outline");
}

////////////////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////////////////
bool HAOmnik::begin(const byte mac[6], HAMqtt *mqtt) 
{
  setUniqueId(mac, 6);
  setManufacturer("InnoVeer");
  setName(DEVICE_NAME);
  setSoftwareVersion(VERSION);
  setModel(DEVICE_MODEL);

  // register the sensors
  mqtt->addDeviceType(&loggerId);     
  mqtt->addDeviceType(&inverterId);     
  mqtt->addDeviceType(&temperature);
  mqtt->addDeviceType(&power);
  mqtt->addDeviceType(&E_today);
  mqtt->addDeviceType(&E_total);
  mqtt->addDeviceType(&operating_hrs);

//  enableSharedAvailability();
  temperature.setAvailability(false); // cant call disable() as we need need to force availability reporting
  return true;
}
  
////////////////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////////////////
#pragma pack(push, 1)   // structures are to be exact, no compiler alignment
struct OmnikHeader {
  byte start;
  byte payload_lg;
  byte msgtype[2];
  int32_t loggerId1;
  int32_t loggerId2;
};

struct OmnikPayload {
  byte dummy[3];
  char inverter_id[16];
  uint16_t temperature;    //10.0
  uint16_t voltage_pv1;    //10.0
  uint16_t voltage_pv2;    //10.0
  uint16_t voltage_pv3;    //10.0
  uint16_t current_pv1;    //10.0
  uint16_t current_pv2;    //10.0
  uint16_t current_pv3;    //10.0
  uint16_t current_ac1;    //10.0
  uint16_t current_ac2;    //10.0
  uint16_t current_ac3;    //10.0
  uint16_t voltage_ac1;    //10.0
  uint16_t voltage_ac2;    //10.0
  uint16_t voltage_ac3;    //10.0
  uint16_t frequency_ac1;  //100.0
  uint16_t power_ac1;
  uint16_t frequency_ac2;  //100.0
  uint16_t power_ac2;
  uint16_t frequency_ac3;  //100.0
  uint16_t power_ac3;
  uint16_t energy_today;   //100.0
  uint32_t energy_total;   //10.0
  uint32_t operating_hours;
};
#pragma pack(pop)

////////////////////////////////////////////////////////////////////////////////////////////
// message format = header | payload | closing
//         header = 0x68 | lg_of_payload | 2 byte dummy | int32 serial (little_endian) | int32 serial
//         closing= crc | 0x16 -> crc = simple the sum of all bytes after payloadlg upto crc wrapped on 0xFF
//  Example message:
//  "\x68\x55\x41\xb0\x8b\xe8\xdc\x23\x8b\xe8\xdc\x23\x81\x02\x01\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x01\x3d"
//  "\x08\x9e\x00\x00\xff\xff\x00\x0b\x00\xb4\xff\xff\x00\x08\xff\xff\xff\xff\x09\x2d\xff\xff\xff\xff\x13\x8a\x00\xd1\xff\xff\xff\xff\xff"
//  "\xff\xff\xff\x03\x30\x00\x00\x00\x51\x00\x00\x00\x0a\x00\x01\x00\x00\x00\x00\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x76\x16"
//  "\x68\x0f\x41\xf0\x8b\xe8\xdc\x23\x8b\xe8\xdc\x23\x44\x41\x54\x41\x20\x53\x45\x4e\x44\x20\x49\x53\x20\x4f\x4b\xfe\x16"
////////////////////////////////////////////////////////////////////////////////////////////
#define ADD_JSON_VAR(var, div, c)    attr += "\"" #var "\": "; attr += ntohs(payl->var) / div; attr+=c

////////////////////////////////////////////////////////////////////////////////////////////
bool HAOmnik::handle(const byte *msg, int lg)
{
  // first we parse the header, and if its a omnik message we publish the loggerid +  msg
  if (lg < sizeof(OmnikHeader)) {  // header is 12 bytes, an empty message is 14
    DEBUG(LOG_PREFIX "Message does not contain an Omnik header\n");
    return false;
  }
  OmnikHeader *hdr = (OmnikHeader *) msg;
  if (hdr->start != 0x68) {
    ERROR(LOG_PREFIX "Unknown start byte, expecting 0x68, received %d\n", hdr->start);
    return false;
  }

  if (lg < (hdr->payload_lg) + sizeof(OmnikHeader) + 2) { // header + 2 closing bytes (crc | 0x16)
    ERROR(LOG_PREFIX "Message to short (%d) according to header (%d)\n", lg, (hdr->payload_lg) + sizeof(OmnikHeader) + 2);
    return false;
  }
  if (hdr->loggerId1 != hdr->loggerId2) {
    ERROR(LOG_PREFIX "LoggerId mismatch in header (%d <-> %d)\n", hdr->loggerId1, hdr->loggerId2);
    return false;
  }
  // now we publish the logger details
  char strbuf[32];
  loggerId.setValue(itoa(hdr->loggerId1, strbuf, 10));
  String attr = "{\"rawmsg\": \"";
  for (int i=0; i<lg; i++)
    attr += itoa(msg[i], strbuf, 16);
  attr += "\"}";
  loggerId.setAttributes(attr.c_str());

  // Lets continue with the payload in the message
  if (hdr->payload_lg < sizeof(OmnikPayload)) {
    ERROR(LOG_PREFIX "Payload to short to retreive inverter attributes\n");
    return false;
  }
  OmnikPayload *payl = (OmnikPayload *) (msg + sizeof(OmnikHeader)); // set pointer behind header

  // publish the inverterid
  memcpy(strbuf, payl->inverter_id, sizeof(payl->inverter_id));
  strbuf[sizeof(payl->inverter_id)] = 0;
  inverterId.setValue(strbuf);

  // publish the sensors
  temperature.setValue(ntohs(payl->temperature) / 10.0f);         // convert network big-endian's to 
  power.setValue((float) ntohs(payl->power_ac1));
  E_today.setValue(ntohs(payl->energy_today) / 100.0f);
  E_total.setValue(ntohl(payl->energy_total) / 10.0f);
  operating_hrs.setValue((float) ntohl(payl->operating_hours));

  // and push some other telemetry as attributes to the inverter_id
  attr = "{";
  ADD_JSON_VAR(voltage_pv1, 10.0f, ',');
  ADD_JSON_VAR(current_pv1, 10.0f, ',');
  ADD_JSON_VAR(current_ac1, 10.0f, ',');
  ADD_JSON_VAR(voltage_ac1, 10.0f, ',');
  ADD_JSON_VAR(frequency_ac1, 100.0f, '}');
  inverterId.setAttributes(attr.c_str());

  // everything went fine - this time - lets return positive
  INFO(LOG_PREFIX "Parsing message successfully\n");
  return true;
}

////////////////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////////////////
void HAOmnik::enable() {
  if (!temperature.isOnline())      // if the device is offline
    temperature.setAvailability(true);
}

////////////////////////////////////////////////////////////////////////////////////////////
void HAOmnik::disable() {
  if (temperature.isOnline()) {     // if the device is online
    temperature.setValue(20.0f);    // reset temp to room temp
    temperature.setAvailability(false);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
