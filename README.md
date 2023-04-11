# Omnik2MQTT Proxy
Omnik Inverter proxy running on a ESP8266 to be used for the older Omnik inverters with a logger using serial number 601xxxxxxxxx

The Omnik proxy will establish a WiFi Access Point (AP) with DHCP in the IP space which is hard coded into the Omnik logger. As such it will mimic the Solarman servers. 
At the same time it connects as WiFi stationairy (STA) to an MQTT server on the internal network and post Omnik data as sensors to be used in Home Automation.
Omnik messages which are correctly parsed, are also stored in FLash memeory using a cyclic buffer on the LittleFS file system. 
Each half hour - without interfering with Omnik communication - it will temporarely close the AP and flush the cached messages to Solarman.
In the night it will do a reboot to reconfigre all data, timers and sensors.
