#pragma once

#include <HADevice.h>
#include <device-types\HASensor.h>
#include <device-types\HASensorNumber.h>
#include <device-types\HABinarySensor.h>

#define HADEVICE_SENSOR_COUNT   10
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
  HASensorNumber  E_today;      // energy delivered today
  HASensorNumber  E_total;      // energy delivered total
  HASensorNumber  operating_hrs;// operating hours in which E_total was delivered

  bool begin(const byte mac[6], HAMqtt *mqqt);
  bool handle(const byte *msg, int lg);
  void enable();
  void disable();
};

//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////

