# Wiring notes

Example pin assignments for a single station. Adjust for your own board and modules.

## SPI — two separate buses

The single most important wiring decision. The W5500 and the SD card must **not**
share a bus, because some cheap SD adapters don't release (tri-state) their MISO line
when deselected and will clamp the shared bus, making the W5500 disappear.

### W5500 Ethernet — VSPI

| Signal | GPIO |
|--------|------|
| SCK    | 18   |
| MISO   | 19   |
| MOSI   | 23   |
| CS     | 5    |

### SD card — dedicated HSPI

| Signal | GPIO |
|--------|------|
| SCK    | 14   |
| MISO   | 27   |
| MOSI   | 13   |
| CS     | 15   |

Create the SD bus explicitly as a second `SPIClass(HSPI)` instance. Avoid GPIO 12 for
SD MISO — it's a boot strapping pin and pulling it high at boot can stop the board
booting.

Keep SPI wiring short (≈10cm or less). Longer runs caused SD init failures.

## IR chutes

Two chutes, one per token colour. Each uses an emitter/receiver pair as a break-beam.

| Signal | GPIO | Notes |
|--------|------|-------|
| Chute A emitter | 26 | 38kHz pulsed burst, 3.3V |
| Chute A receiver | 35 | input-only pin |
| Chute B emitter | 25 | 38kHz pulsed burst, 3.3V |
| Chute B receiver | 34 | input-only pin |

GPIO 34 and 35 are input-only — good for receivers. Keep receivers on 3.3V; driving
their output above the ESP32's input tolerance can damage the pin.

## OLED — I2C

| Signal | GPIO |
|--------|------|
| SDA    | 21   |
| SCL    | 22   |

SSD1306 128×64 at address 0x3C.

## Power

Each station runs from its own 5V USB supply. No shared PSU across stations — one
supply per station keeps a single failure from taking down more than one unit.
