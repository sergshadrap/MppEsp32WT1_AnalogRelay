# MppEsp32WT1_AnalogRelay
client firmware for Esp32+Ethernet8720 based board WT1 work with AM Server , emulating Relay + DS18B20 temperature sensor

This is simple emulator of mpp relay (on/off only,no momentary) and analog device for ESP32 chip family with Ethernet LAN8720 chip.
This particular configuration is expected to work on WT1 -esp32-Eth board .
The IOs for relay is 14, for DS18B20- 15.
The device starts simple web interface on 80 port and work interface on port 8898 for interacting with AM server and other AM devices
#include <ETH.h> - must follow after the PHY definitions accordingly v.3.0 Ethernet esp32 library cahnges.
The device keep all data etc relay pins configuration and their status in EEPROM after any changes.
It respond on UDP discovery from AM Server with current device configuration.
The device send broadcast UDP for detecting it by server right after the start and obtaining an IP.
Subscription service maintain up to 5 subscribers , the Subscribers  are being notifyed right after PUT/ query from service and changing the status and periodically every 10 min.
Every subscribers send query for resubscription after expires.
The device respond correctly to GET/survey query from server .
Last version support OTA from page 
