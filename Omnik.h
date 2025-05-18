#pragma once

#include <HADevice.h>
#include <device-types\HASensor.h>
#include <device-types\HASensorNumber.h>
#include <device-types\HABinarySensor.h>

#define HADEVICE_SENSOR_COUNT   12
//////////////////////////////////////////////////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////////////////////////////////////////////////
class HAOmnik : public HADevice 
{
public:
  HAOmnik();
  HASensor        loggerId;     // the logger S/N
  HASensor        inverterId;   // the inverter S/N
  HASensorNumber  power;        // current power
  HASensorNumber  temperature;  // inverter temperature
  HASensorNumber  signal;       // signal strenth of this logger to the AP
  HASensorNumber  ac_frequency; // grid frequency
  HASensorNumber  ac_voltage;   // grid voltage
  HASensorNumber  E_today;      // energy delivered today
  HASensorNumber  E_total;      // energy delivered total
  HASensorNumber  operating_hrs;// operating hours in which E_total was delivered

  bool begin(const byte mac[6], HAMqtt *mqqt);
  bool handle(const byte *msg, int lg, int8_t rssi=-100);
  void enable();                // inform that Omnik to active
  void disable();               // infomr that Omnik is in sleep
};

//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////

