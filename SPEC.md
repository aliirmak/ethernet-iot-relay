# Uno32 Ethernet Power Cycler Specification

## Implemented Scope

This project targets a Digilent chipKIT Uno32 Rev C using PlatformIO's
`microchippic32` platform and Arduino framework. It controls the Normally ON
outlet of an external IoT Relay through D7 and serves a local W5100 Ethernet
interface.

## Required Behavior

- Start in commanded `POWER_ON` state.
- Use named relay polarity constants rather than scattered HIGH/LOW values.
- Support `POWER_ON`, `POWER_OFF`, `RESTARTING`, and `UNKNOWN` states.
- Report `RESTART_COMPLETE` after power has been restored by a restart.
- Perform a configurable, non-blocking restart with a 3-second default.
- Persist the web-selected 1–15 second restart delay in EEPROM.
- Reject a restart request while another restart is active.
- Provide web routes `/`, `/on`, `/off`, `/restart`, and `/status`.
- Provide serial commands `help`, `status`, `on`, `off`, `restart`, and `ip`.
- Report commanded state only; physical AC state is not sensed.
- Monitor SLRT target `192.168.1.101` using ICMP echo only.
- Report commanded power and target ping status as separate values.
- Use a 90-second boot grace period after power-on and restart.
- Require three consecutive post-grace ping failures before reporting
  `PING_NOT_RESPONDING`.
- Provide web route `/pingnow` and include ping fields in `/status`.
- Provide serial commands `pingstatus` and `pingnow`.
- Use static IP `192.168.1.105` by default.

## Pin Allocation

| Pin | Purpose |
| --- | --- |
| D4 | Ethernet Shield SD chip select, held inactive |
| D7 | IoT Relay control |
| D10 | W5100 chip select |
| D11-D13 | SPI bus |

## Safety Constraints

The Uno32 must not switch or connect directly to AC wiring. The external IoT
Relay provides the mains-rated interface. The controller must remain
independently powered, and the unauthenticated web server must remain on a
trusted local network or behind a firewall/VPN.
