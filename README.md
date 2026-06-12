# Uno32 Ethernet Power Cycler

Firmware for a Digilent chipKIT Uno32 Rev C and a W5100-style Arduino
Ethernet Shield. It controls a Digital Loggers IoT Relay II (or equivalent)
from a local web page or the USB serial monitor.

The displayed state is the **commanded state**. This design has no AC voltage
feedback and cannot verify that the computer is actually powered.

## Hardware

- Digilent chipKIT Uno32 Rev C
- Arduino Ethernet Shield (typically W5100)
- Digital Loggers IoT Relay II or compatible isolated relay interface
- Independent Uno32 power supply
- Ethernet cable

## Wiring

```text
Uno32 D7  -------------------- IoT Relay control input (+)
Uno32 GND -------------------- IoT Relay control input (-/GND)

Router     ------------------- Ethernet Shield RJ45
Independent adapter ---------- Uno32 power input
Computer AC plug ------------- IoT Relay "Normally ON" outlet
```

Do not connect the Uno32 directly to mains wiring. Follow the relay
manufacturer's instructions if its trigger connector labels differ.

The firmware reserves Ethernet Shield pins D4 and D10-D13. The relay signal
uses D7. With the assumed IoT Relay behavior, D7 LOW keeps the Normally ON
outlet energized and D7 HIGH turns it off.

## Configure

Edit the network values near the top of `src/main.cpp`:

```cpp
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x32, 0x01};
IPAddress ip(192, 168, 0, 50);
IPAddress dnsServer(192, 168, 0, 1);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
```

Change `RELAY_SIGNAL_FOR_POWER_ON` and
`RELAY_SIGNAL_FOR_POWER_OFF` if testing shows that the relay polarity is
reversed.

## Build And Upload

Install PlatformIO, connect the Uno32, then run:

```powershell
pio run
pio run --target upload
pio device monitor
```

The serial monitor runs at 115200 baud. If more than one serial device is
connected, set `upload_port` and `monitor_port` in `platformio.ini`.

## Use

Open `http://192.168.0.50/` from the same local network. Available endpoints:

- `/` - control page and commanded status
- `/on` - command power on
- `/off` - command power off
- `/restart` - turn power off for the saved delay, then restore it
- `/delay?seconds=3` - save a restart delay from 1 to 15 seconds
- `/status` - machine-readable plain-text status

The web page includes a 1–15 second restart-delay slider. Its saved value is
stored in the Uno32's EEPROM emulation and survives resets and power loss. The
default is 3 seconds.

Serial commands are `help`, `status`, `on`, `off`, `restart`, and `ip`.
The restart timer is non-blocking; status and serial commands remain
responsive. A second restart request during a restart is rejected.

## Bench Test

Before connecting the IoT Relay, attach an LED with a suitable resistor or a
multimeter to D7 and GND:

1. Start the serial monitor and confirm the startup message.
2. Send `off`; D7 should read HIGH (about 3.3 V).
3. Send `on`; D7 should read LOW.
4. Send `restart`; D7 should remain HIGH for the saved delay and return LOW.
5. Connect Ethernet and verify the control page and `/status`.

Test the Ethernet Shield before making any board modifications. Some Uno32
Rev C and shield combinations may have an SPI/ICSP reset compatibility issue.
Only investigate the Uno32 JP9 guidance if Ethernet activity causes repeated
resets.

## Safety

- Keep this web interface on a trusted LAN or behind a firewall/VPN.
- Do not port-forward it to the public internet.
- Power the Uno32 independently from the controlled computer.
- Use the relay's Normally ON outlet for the computer.
- Configure the computer BIOS/UEFI to restore power after AC loss.
- Avoid repeated rapid switching, which can stress the computer and relay.
- This first version has no authentication; GET links can trigger actions.
