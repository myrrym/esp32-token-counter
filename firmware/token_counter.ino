/*
 * ESP32 Token Counter — production firmware (single-station reference)
 *
 * Counts tokens through two pulsed 38kHz IR break-beam chutes (black + blue),
 * shows live counts on an OLED, persists to SD as the local source of truth
 * (survives power loss), and POSTs absolute counts to a hub over wired
 * Ethernet (W5500). Restores state on boot from the hub, then SD, then zero.
 *
 * This is the real production sketch with per-unit identity removed. In the
 * deployed build each station had its own MAC, IP, and ID; here that's reduced
 * to a single example set — change it per unit on your own network.
 *
 * Hub expected at 192.168.1.10:8000  (POST /update, GET /session)
 *
 * ---------------------------------------------------------------------------
 * TWO-BUS ARCHITECTURE (hard-won fix):
 *   The level-shifter SD adapter's MISO buffer does NOT tri-state when
 *   deselected, so on a SHARED SPI bus it clamps MISO and makes the W5500
 *   undetectable (hardware status 0). Confirmed by test: pulling SD's MISO
 *   wire instantly restored the W5500.
 *
 *   Fix: give each device its own hardware SPI controller.
 *     - W5500 stays on VSPI (default SPI): SCK=18 MISO=19 MOSI=23, CS=5
 *     - SD moves to  HSPI (its own bus):   SCK=14 MISO=27 MOSI=13, CS=15
 * ---------------------------------------------------------------------------
 *
 * COUNTING TRUTH MODEL:
 *   - SD = local source of truth. Every token flushes to /counts.txt
 *     immediately via ATOMIC temp-file + rename, so a power cut can never
 *     leave you with no file at all.
 *   - POSTs are ABSOLUTE counts, never deltas -> a reconnect never double-counts.
 *   - Boot restore priority: hub /session -> SD file -> zero.
 *
 * KEY IR LESSON baked in: the 38kHz receiver must be driven with SPACED bursts
 * (600us burst + 1400us gap) and the loop must be PACED (~30ms) or the
 * receiver's AGC is overwhelmed and produces phantom counts.
 * Do NOT "optimise" the delays out.
 *
 * Both chutes are break-beam.
 *
 * WIRING (locked pin map):
 *   W5500 (VSPI):  SCK=18  MISO=19  MOSI=23   CS=5
 *   SD    (HSPI):  SCK=14  MISO=27  MOSI=13   CS=15
 *   OLED  SDA=21 SCL=22  (I2C 0x3C)
 *   IR TX black=25 blue=26  |  IR RX black=34 blue=35 (input-only)
 *   Power: 3V3 -> W5500, OLED, all 4 IR modules
 *          5V  -> SD level-shifter adapter VCC only
 *
 * FLASHING NOTE: use Upload Speed 115200. 921600 corrupted a write during the
 * build and required a full erase-flash to recover.
 */

#include <SPI.h>
#include <SD.h>
#include <Ethernet.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---- Network / identity (set per station) ----
// Each station needs a unique MAC, IP, and ID on your LAN.
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01 };
IPAddress ip(192, 168, 1, 101);
IPAddress hubServer(192, 168, 1, 10);
const int HUB_PORT = 8000;
const int STATION_ID = 1;

// ---- W5500 on VSPI (default bus) ----
#define W5500_CS 5

// ---- SD on its OWN HSPI bus ----
#define SD_SCK  14
#define SD_MISO 27
#define SD_MOSI 13
#define SD_CS   15
SPIClass sdSPI(HSPI);                   // dedicated SPI controller for SD

// ---- OLED ----
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---- IR  (black = 25/34, blue = 26/35) ----
#define TX_BLACK 25
#define RX_BLACK 34
#define TX_BLUE  26
#define RX_BLUE  35
#define LEDC_FREQ 38000
#define LEDC_RES  8

// ---- SD ----
const char* COUNT_FILE = "/counts.txt";
const char* TMP_FILE   = "/counts.tmp";
bool sdOK = false;

unsigned long countBlack = 0, countBlue = 0;
bool blackBroken = false, blueBroken = false;
unsigned long lastBlack = 0, lastBlue = 0;
const unsigned long DEBOUNCE_MS = 120;
bool dirty = false;
bool netOK = false;
unsigned long lastShownK = 999999, lastShownB = 999999;

EthernetClient client;

// ---------------------------------------------------------------------------
// SD helpers  (SD is on its own bus — no release/isolation needed)
// ---------------------------------------------------------------------------
// File format: single line "black,blue\n".
// ATOMIC WRITE: write full line to /counts.tmp, verify, THEN swap it into
// place. The real file is only removed once a complete replacement exists.
void sdFlush() {
  if (!sdOK) return;

  SD.remove(TMP_FILE);
  File f = SD.open(TMP_FILE, FILE_WRITE);
  if (!f) { Serial.println("SD flush FAILED (tmp open)"); return; }
  f.printf("%lu,%lu\n", countBlack, countBlue);
  f.flush();
  f.close();

  // Verify the temp file is non-empty before we trust it.
  File chk = SD.open(TMP_FILE, FILE_READ);
  if (!chk || chk.size() == 0) {
    if (chk) chk.close();
    Serial.println("SD flush FAILED (tmp empty) — keeping old file");
    return;
  }
  chk.close();

  SD.remove(COUNT_FILE);
  if (!SD.rename(TMP_FILE, COUNT_FILE)) {
    Serial.println("SD rename FAILED");
  }
}

bool sdRestore(unsigned long &k, unsigned long &b) {
  if (!sdOK) return false;

  // If a crash left a stale tmp behind but the real file is gone, recover it.
  if (!SD.exists(COUNT_FILE) && SD.exists(TMP_FILE)) {
    SD.rename(TMP_FILE, COUNT_FILE);
  }
  if (!SD.exists(COUNT_FILE)) return false;

  File f = SD.open(COUNT_FILE, FILE_READ);
  if (!f) return false;
  String line = f.readStringUntil('\n');
  f.close();
  int comma = line.indexOf(',');
  if (comma < 0) return false;
  k = line.substring(0, comma).toInt();
  b = line.substring(comma + 1).toInt();
  return true;
}

// ---------------------------------------------------------------------------
// IR
// ---------------------------------------------------------------------------
// Send ONE 38kHz burst on txPin, then read rxPin. true = beam SEEN.
// The trailing gap lets the receiver AGC reset before the next burst.
bool burstSeen(int txPin, int rxPin) {
  ledcWrite(txPin, 128);
  delayMicroseconds(600);
  int seen = (digitalRead(rxPin) == LOW);   // LOW = 38kHz detected
  ledcWrite(txPin, 0);
  delayMicroseconds(1400);
  return seen;
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);  display.print("Station ");  display.println(STATION_ID);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print("Net:");
  display.print(netOK ? "OK " : "FAIL ");
  display.print("SD:");
  display.println(sdOK ? "OK" : "X");
  display.setTextSize(2);
  display.setCursor(0, 34); display.print("BLK: "); display.println(countBlack);
  display.setCursor(0, 50); display.print("BLU: "); display.println(countBlue);
  display.display();
}

// ---------------------------------------------------------------------------
// Network — POST absolute counts (time-bounded, never stalls beam reading)
// ---------------------------------------------------------------------------
void postCounts() {
  if (client.connect(hubServer, HUB_PORT)) {
    String body = "{\"station_id\":" + String(STATION_ID) +
                  ",\"blue\":"  + String(countBlue) +
                  ",\"black\":" + String(countBlack) + "}";
    client.println("POST /update HTTP/1.1");
    client.print("Host: "); client.println(hubServer);
    client.println("Content-Type: application/json");
    client.print("Content-Length: "); client.println(body.length());
    client.println("Connection: close");
    client.println();
    client.print(body);
    unsigned long t = millis();
    while (client.connected() && millis() - t < 500) {
      while (client.available()) client.read();
    }
    client.stop();
    Serial.println("POSTed: " + body);
  } else {
    Serial.println("POST failed");
    client.stop();
  }
}

// Ask the hub for authoritative session counts. Returns true if the hub
// answered with usable black/blue values. Parses the JSON body minimally.
bool hubSession(unsigned long &k, unsigned long &b) {
  if (client.connect(hubServer, HUB_PORT)) {
    client.print("GET /session?station_id="); client.print(STATION_ID);
    client.println(" HTTP/1.1");
    client.print("Host: "); client.println(hubServer);
    client.println("Connection: close");
    client.println();

    String resp = "";
    unsigned long t = millis();
    while (client.connected() && millis() - t < 800) {
      while (client.available()) resp += (char)client.read();
    }
    client.stop();

    int ki = resp.indexOf("\"black\"");
    int bi = resp.indexOf("\"blue\"");
    if (ki < 0 || bi < 0) return false;
    k = resp.substring(resp.indexOf(':', ki) + 1).toInt();
    b = resp.substring(resp.indexOf(':', bi) + 1).toInt();
    return true;
  }
  client.stop();
  return false;
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- Token Counter (2-bus SD, black+blue) ---");

  // I2C / OLED
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDR)) {
    Serial.println("OLED not found!");
  }

  // ---- W5500 on VSPI (default) ----
  pinMode(W5500_CS, OUTPUT); digitalWrite(W5500_CS, HIGH);
  Ethernet.init(W5500_CS);
  Ethernet.begin(mac, ip);
  delay(1000);
  Serial.print("HW status: "); Serial.println(Ethernet.hardwareStatus()); // 0=none
  netOK = (Ethernet.linkStatus() == LinkON);
  Serial.print("Station IP: "); Serial.println(Ethernet.localIP());
  Serial.print("Link: "); Serial.println(netOK ? "ON" : "OFF");

  // ---- SD on its OWN HSPI bus ----
  pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOK = SD.begin(SD_CS, sdSPI);         // pass the dedicated SPI instance
  Serial.print("SD: "); Serial.println(sdOK ? "OK" : "FAILED");

  // IR
  ledcAttach(TX_BLACK, LEDC_FREQ, LEDC_RES);
  ledcAttach(TX_BLUE,  LEDC_FREQ, LEDC_RES);
  ledcWrite(TX_BLACK, 0);
  ledcWrite(TX_BLUE,  0);
  pinMode(RX_BLACK, INPUT);
  pinMode(RX_BLUE,  INPUT);

  // ---- BOOT RESTORE: hub /session -> SD -> zero ----
  unsigned long k = 0, b = 0;
  if (netOK && hubSession(k, b)) {
    countBlack = k; countBlue = b;
    Serial.printf("Restored from hub: K=%lu B=%lu\n", k, b);
    sdFlush();                          // sync SD to hub truth
  } else if (sdRestore(k, b)) {
    countBlack = k; countBlue = b;
    Serial.printf("Restored from SD: K=%lu B=%lu\n", k, b);
  } else {
    countBlack = 0; countBlue = 0;
    Serial.println("No restore source -> starting at zero");
    sdFlush();
  }

  updateDisplay();

  // ARM: require 20 consecutive stable "clear" reads before counting,
  // so startup transients don't register as tokens.
  Serial.println("Arming...");
  delay(800);
  int stable = 0;
  unsigned long armStart = millis();
  while (stable < 20 && (millis() - armStart < 8000)) {
    bool kk = !burstSeen(TX_BLACK, RX_BLACK);
    delay(10);
    bool bb = !burstSeen(TX_BLUE,  RX_BLUE);
    if (!kk && !bb) stable++; else stable = 0;
    delay(20);
  }
  blackBroken = false; blueBroken = false;
  lastBlack = millis(); lastBlue = millis();

  postCounts();                         // announce restored counts to the hub
  updateDisplay();
  Serial.println("Ready.");
}

void loop() {
  unsigned long now = millis();

  bool kBroken = !burstSeen(TX_BLACK, RX_BLACK);
  delay(10);                            // spacing between the two chutes
  bool bBroken = !burstSeen(TX_BLUE,  RX_BLUE);

  if (kBroken && !blackBroken && (now - lastBlack > DEBOUNCE_MS)) {
    countBlack++; lastBlack = now; dirty = true;
    sdFlush();                          // FLUSH-ON-COUNT (own bus, safe)
    Serial.printf("BLACK: %lu\n", countBlack);
  }
  blackBroken = kBroken;

  if (bBroken && !blueBroken && (now - lastBlue > DEBOUNCE_MS)) {
    countBlue++; lastBlue = now; dirty = true;
    sdFlush();                          // FLUSH-ON-COUNT (own bus, safe)
    Serial.printf("BLUE: %lu\n", countBlue);
  }
  blueBroken = bBroken;

  if (countBlack != lastShownK || countBlue != lastShownB) {
    updateDisplay();
    lastShownK = countBlack; lastShownB = countBlue;
  }

  if (dirty) { postCounts(); dirty = false; }

  delay(30);                            // PACING — do not remove (AGC stability)
}
