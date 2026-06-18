#include <Arduino.h>
#include <EEPROM.h>
#include <Ethernet.h>
#include <SPI.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icmp_ping.h"

// Hardware and timing settings.
const uint8_t RELAY_PIN = 7;
const uint8_t ETHERNET_CS_PIN = 10;
const uint8_t SHIELD_SD_CS_PIN = 4;
const bool RELAY_SIGNAL_FOR_POWER_ON = LOW;
const bool RELAY_SIGNAL_FOR_POWER_OFF = HIGH;
const uint8_t DEFAULT_RESTART_DELAY_SECONDS = 3;
const uint8_t MIN_RESTART_DELAY_SECONDS = 1;
const uint8_t MAX_RESTART_DELAY_SECONDS = 15;
const uint16_t SETTINGS_MAGIC = 0x5532;
const uint8_t SETTINGS_VERSION = 2;
const bool PING_MONITOR_ENABLED = true;
const unsigned long BOOT_GRACE_MS = 90000UL;
const unsigned long PING_TIMEOUT_MS = 1500UL;
const uint8_t MAX_FAILED_PINGS_BEFORE_NOT_RESPONDING = 3;
const bool DEFAULT_CONTINUOUS_PING_ENABLED = false;
const uint16_t DEFAULT_PING_INTERVAL_SECONDS = 10;
const uint16_t MIN_PING_INTERVAL_SECONDS = 1;
const uint16_t MAX_PING_INTERVAL_SECONDS = 300;

// Edit these values to match the local network.
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x32, 0x01};
IPAddress ip(192, 168, 1, 105);
IPAddress dnsServer(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress slrtTargetIP(192, 168, 1, 101);

EthernetServer server(80);

enum PowerState { POWER_ON, POWER_OFF, RESTARTING, RESTART_COMPLETE, UNKNOWN };
enum PingStatus {
  PING_UNKNOWN,
  PING_BOOTING,
  PING_CHECKING,
  PING_RESPONDING,
  PING_NOT_RESPONDING,
  PING_DISABLED
};

PowerState powerState = UNKNOWN;
PingStatus slrtPingStatus = PING_UNKNOWN;
unsigned long restartStartedAt = 0;
unsigned long lastPingAttemptMs = 0;
unsigned long lastSuccessfulPingMs = 0;
unsigned long bootGraceStartMs = 0;
uint8_t restartDelaySeconds = DEFAULT_RESTART_DELAY_SECONDS;
uint16_t pingIntervalSeconds = DEFAULT_PING_INTERVAL_SECONDS;
uint32_t consecutiveFailedPings = 0;
bool pingCheckRequested = false;
bool bootGraceActive = false;
bool hasPingAttempt = false;
bool continuousPingEnabled = DEFAULT_CONTINUOUS_PING_ENABLED;

struct __attribute__((packed)) LegacyPersistentSettings {
  uint16_t magic;
  uint8_t version;
  uint8_t restartDelaySeconds;
  uint8_t checksum;
};

struct __attribute__((packed)) PersistentSettings {
  uint16_t magic;
  uint8_t version;
  uint8_t restartDelaySeconds;
  uint8_t continuousPingEnabled;
  uint16_t pingIntervalSeconds;
  uint8_t checksum;
};

uint8_t legacySettingsChecksum(const LegacyPersistentSettings &settings) {
  return static_cast<uint8_t>(settings.magic ^ (settings.magic >> 8) ^
                              settings.version ^ settings.restartDelaySeconds ^
                              0xA5);
}

uint8_t settingsChecksum(const PersistentSettings &settings) {
  return static_cast<uint8_t>(
      settings.magic ^ (settings.magic >> 8) ^ settings.version ^
      settings.restartDelaySeconds ^ settings.continuousPingEnabled ^
      settings.pingIntervalSeconds ^ (settings.pingIntervalSeconds >> 8) ^
      0xA5);
}

bool isValidRestartDelay(uint8_t seconds) {
  return seconds >= MIN_RESTART_DELAY_SECONDS &&
         seconds <= MAX_RESTART_DELAY_SECONDS;
}

bool isValidPingInterval(uint16_t seconds) {
  return seconds >= MIN_PING_INTERVAL_SECONDS &&
         seconds <= MAX_PING_INTERVAL_SECONDS;
}

bool saveSettings(uint8_t restartSeconds, bool continuousEnabled,
                  uint16_t intervalSeconds) {
  PersistentSettings settings = {
      SETTINGS_MAGIC, SETTINGS_VERSION, restartSeconds,
      continuousEnabled ? 1U : 0U, intervalSeconds, 0};
  settings.checksum = settingsChecksum(settings);
  EEPROM.put(0, settings);

  PersistentSettings savedSettings;
  EEPROM.get(0, savedSettings);
  bool saved =
      savedSettings.magic == SETTINGS_MAGIC &&
      savedSettings.version == SETTINGS_VERSION &&
      savedSettings.checksum == settingsChecksum(savedSettings) &&
      savedSettings.restartDelaySeconds == restartSeconds &&
      savedSettings.continuousPingEnabled == settings.continuousPingEnabled &&
      savedSettings.pingIntervalSeconds == intervalSeconds;
  if (saved) {
    restartDelaySeconds = restartSeconds;
    continuousPingEnabled = continuousEnabled;
    pingIntervalSeconds = intervalSeconds;
  }
  return saved;
}

void loadSettings() {
  PersistentSettings settings;
  EEPROM.get(0, settings);

  if (settings.magic == SETTINGS_MAGIC &&
      settings.version == SETTINGS_VERSION &&
      settings.checksum == settingsChecksum(settings) &&
      isValidRestartDelay(settings.restartDelaySeconds) &&
      settings.continuousPingEnabled <= 1 &&
      isValidPingInterval(settings.pingIntervalSeconds)) {
    restartDelaySeconds = settings.restartDelaySeconds;
    continuousPingEnabled = settings.continuousPingEnabled != 0;
    pingIntervalSeconds = settings.pingIntervalSeconds;
    return;
  }

  LegacyPersistentSettings legacySettings;
  EEPROM.get(0, legacySettings);
  if (legacySettings.magic == SETTINGS_MAGIC &&
      legacySettings.version == 1 &&
      legacySettings.checksum == legacySettingsChecksum(legacySettings) &&
      isValidRestartDelay(legacySettings.restartDelaySeconds)) {
    restartDelaySeconds = legacySettings.restartDelaySeconds;
  }

  saveSettings(restartDelaySeconds, DEFAULT_CONTINUOUS_PING_ENABLED,
               DEFAULT_PING_INTERVAL_SECONDS);
}

bool saveRestartDelay(uint8_t seconds) {
  return saveSettings(seconds, continuousPingEnabled, pingIntervalSeconds);
}

bool savePingSettings(bool continuousEnabled, uint16_t intervalSeconds) {
  return saveSettings(restartDelaySeconds, continuousEnabled, intervalSeconds);
}

unsigned long restartDelayMilliseconds() {
  return static_cast<unsigned long>(restartDelaySeconds) * 1000UL;
}

const char *stateName(PowerState state) {
  switch (state) {
  case POWER_ON:
    return "POWER_ON";
  case POWER_OFF:
    return "POWER_OFF";
  case RESTARTING:
    return "RESTARTING";
  case RESTART_COMPLETE:
    return "RESTART_COMPLETE";
  default:
    return "UNKNOWN";
  }
}

const char *pingStatusToString(PingStatus status) {
  switch (status) {
  case PING_BOOTING:
    return "PING_BOOTING";
  case PING_CHECKING:
    return "PING_CHECKING";
  case PING_RESPONDING:
    return "PING_RESPONDING";
  case PING_NOT_RESPONDING:
    return "PING_NOT_RESPONDING";
  case PING_DISABLED:
    return "PING_DISABLED";
  default:
    return "PING_UNKNOWN";
  }
}

void printAddress(Stream &output, IPAddress address, bool newline = true) {
  output.print(address[0]);
  output.print('.');
  output.print(address[1]);
  output.print('.');
  output.print(address[2]);
  output.print('.');
  output.print(address[3]);
  if (newline) {
    output.println();
  }
}

unsigned long bootGraceRemainingMs() {
  if (!bootGraceActive) {
    return 0;
  }

  unsigned long elapsed = millis() - bootGraceStartMs;
  return elapsed < BOOT_GRACE_MS ? BOOT_GRACE_MS - elapsed : 0;
}

void beginBootGrace() {
  bootGraceStartMs = millis();
  bootGraceActive = true;
  consecutiveFailedPings = 0;
  slrtPingStatus =
      PING_MONITOR_ENABLED ? PING_BOOTING : PING_DISABLED;
  pingCheckRequested = PING_MONITOR_ENABLED && continuousPingEnabled;
}

void writeRelaySignal(bool signal) {
  digitalWrite(RELAY_PIN, signal ? HIGH : LOW);
}

void setPowerOn() {
  writeRelaySignal(RELAY_SIGNAL_FOR_POWER_ON);
  powerState = POWER_ON;
  beginBootGrace();
  Serial.println("Power commanded ON.");
}

void setPowerOff() {
  writeRelaySignal(RELAY_SIGNAL_FOR_POWER_OFF);
  powerState = POWER_OFF;
  bootGraceActive = false;
  pingCheckRequested = false;
  consecutiveFailedPings = 0;
  slrtPingStatus =
      PING_MONITOR_ENABLED ? PING_UNKNOWN : PING_DISABLED;
  Serial.println("Power commanded OFF.");
}

bool restartPowerCycle() {
  if (powerState == RESTARTING) {
    Serial.println("Restart ignored: a restart is already in progress.");
    return false;
  }

  writeRelaySignal(RELAY_SIGNAL_FOR_POWER_OFF);
  powerState = RESTARTING;
  restartStartedAt = millis();
  bootGraceActive = false;
  pingCheckRequested = false;
  consecutiveFailedPings = 0;
  slrtPingStatus =
      PING_MONITOR_ENABLED ? PING_BOOTING : PING_DISABLED;
  Serial.println("Restart started: power commanded OFF.");
  return true;
}

void updateRestartState() {
  if (powerState == RESTARTING &&
      millis() - restartStartedAt >= restartDelayMilliseconds()) {
    writeRelaySignal(RELAY_SIGNAL_FOR_POWER_ON);
    powerState = POWER_ON;
    beginBootGrace();
    Serial.println("Restart complete.");
  }
}

void printIpAddress(Stream &output) {
  printAddress(output, Ethernet.localIP());
}

void printEthernetDiagnostics() {
  Serial.print("Ethernet hardware: ");
  switch (Ethernet.hardwareStatus()) {
  case EthernetW5100:
    Serial.println("W5100");
    break;
  case EthernetW5200:
    Serial.println("W5200");
    break;
  case EthernetW5500:
    Serial.println("W5500");
    break;
  default:
    Serial.println("NOT DETECTED");
    break;
  }

  Serial.print("Ethernet link: ");
  switch (Ethernet.linkStatus()) {
  case LinkON:
    Serial.println("ON");
    break;
  case LinkOFF:
    Serial.println("OFF");
    break;
  default:
    Serial.println("UNKNOWN");
    break;
  }
}

bool pingSLRTTarget() {
  return icmpPing(slrtTargetIP, PING_TIMEOUT_MS);
}

bool requestPingCheck() {
  if (!PING_MONITOR_ENABLED || powerState != POWER_ON) {
    return false;
  }

  pingCheckRequested = true;
  return true;
}

void updatePingMonitor() {
  if (!PING_MONITOR_ENABLED) {
    slrtPingStatus = PING_DISABLED;
    return;
  }

  if (powerState != POWER_ON) {
    return;
  }

  unsigned long now = millis();
  bool withinBootGrace = false;
  if (bootGraceActive) {
    if (now - bootGraceStartMs < BOOT_GRACE_MS) {
      withinBootGrace = true;
      if (slrtPingStatus != PING_RESPONDING) {
        slrtPingStatus = PING_BOOTING;
      }
    } else {
      bootGraceActive = false;
    }
  }

  unsigned long pingIntervalMs =
      static_cast<unsigned long>(pingIntervalSeconds) * 1000UL;
  bool timeForPeriodicPing =
      continuousPingEnabled &&
      (!hasPingAttempt || now - lastPingAttemptMs >= pingIntervalMs);
  if (!pingCheckRequested && !timeForPeriodicPing) {
    return;
  }

  pingCheckRequested = false;
  hasPingAttempt = true;
  lastPingAttemptMs = now;
  PingStatus previousStatus = slrtPingStatus;
  slrtPingStatus = PING_CHECKING;

  bool responding = pingSLRTTarget();
  if (responding) {
    lastSuccessfulPingMs = millis();
    consecutiveFailedPings = 0;
    bootGraceActive = false;
    slrtPingStatus = PING_RESPONDING;
    Serial.println("SLRT ping: responding.");
    return;
  }

  if (withinBootGrace) {
    consecutiveFailedPings = 0;
    slrtPingStatus = PING_BOOTING;
    Serial.println("SLRT ping: no response during boot grace.");
    return;
  }

  if (consecutiveFailedPings < 0xFFFFFFFFUL) {
    ++consecutiveFailedPings;
  }

  if (consecutiveFailedPings >=
      MAX_FAILED_PINGS_BEFORE_NOT_RESPONDING) {
    slrtPingStatus = PING_NOT_RESPONDING;
  } else if (previousStatus == PING_RESPONDING) {
    slrtPingStatus = PING_RESPONDING;
  } else {
    slrtPingStatus = PING_UNKNOWN;
  }

  Serial.print("SLRT ping: no response (consecutive failures: ");
  Serial.print(consecutiveFailedPings);
  Serial.println(").");
}

void printPingStatus(Stream &output) {
  output.print("SLRT Target IP: ");
  printAddress(output, slrtTargetIP);
  output.print("SLRT Ping Status: ");
  output.println(pingStatusToString(slrtPingStatus));
  output.print("Last Ping Check: ");
  if (hasPingAttempt) {
    output.print((millis() - lastPingAttemptMs) / 1000UL);
    output.println(" seconds ago");
  } else {
    output.println("never");
  }
  output.print("Failed Ping Count: ");
  output.println(consecutiveFailedPings);
  output.print("Boot Grace Remaining: ");
  output.print(bootGraceRemainingMs() / 1000UL);
  output.println(" seconds");
  output.print("Continuous Ping: ");
  output.println(continuousPingEnabled ? "ENABLED" : "DISABLED");
  output.print("Ping Interval: ");
  output.print(pingIntervalSeconds);
  output.println(" seconds");
}

void printStatus(Stream &output) {
  output.print("Commanded Power State: ");
  output.println(stateName(powerState));
  output.print("Relay pin: ");
  output.println(RELAY_PIN);
  output.print("Restart delay: ");
  output.print(restartDelayMilliseconds());
  output.println(" ms");
  printPingStatus(output);
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  help     Show this help");
  Serial.println("  status   Show commanded state");
  Serial.println("  on       Command power ON");
  Serial.println("  off      Command power OFF");
  Serial.println("  restart  Start a power cycle using the saved delay");
  Serial.println("  ip       Show the configured IP address");
  Serial.println("  pingstatus  Show the latest SLRT ping status");
  Serial.println("  pingnow     Request an immediate SLRT ping check");
}

void processSerialCommand(char *command) {
  for (char *cursor = command; *cursor != '\0'; ++cursor) {
    *cursor = static_cast<char>(tolower(static_cast<unsigned char>(*cursor)));
  }

  if (strcmp(command, "help") == 0) {
    printHelp();
  } else if (strcmp(command, "status") == 0) {
    printStatus(Serial);
  } else if (strcmp(command, "on") == 0) {
    setPowerOn();
  } else if (strcmp(command, "off") == 0) {
    setPowerOff();
  } else if (strcmp(command, "restart") == 0) {
    restartPowerCycle();
  } else if (strcmp(command, "ip") == 0) {
    Serial.print("IP address: ");
    printIpAddress(Serial);
  } else if (strcmp(command, "pingstatus") == 0 ||
             strcmp(command, "netstatus") == 0) {
    printPingStatus(Serial);
  } else if (strcmp(command, "pingnow") == 0 ||
             strcmp(command, "checknow") == 0) {
    Serial.println(requestPingCheck()
                       ? "SLRT ping check requested."
                       : "SLRT ping check unavailable while power is OFF.");
  } else if (*command != '\0') {
    Serial.println("Unknown command. Type 'help'.");
  }
}

void handleSerial() {
  static char command[32];
  static uint8_t length = 0;

  while (Serial.available() > 0) {
    char incoming = static_cast<char>(Serial.read());

    if (incoming == '\r' || incoming == '\n') {
      if (length > 0) {
        command[length] = '\0';
        processSerialCommand(command);
        length = 0;
      }
    } else if (length < sizeof(command) - 1) {
      command[length++] = incoming;
    }
  }
}

void sendCommonHeaders(EthernetClient &client, const char *contentType) {
  client.println("HTTP/1.1 200 OK");
  client.print("Content-Type: ");
  client.println(contentType);
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
}

void sendRedirectToDashboard(EthernetClient &client) {
  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /");
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
}

void sendStatus(EthernetClient &client) {
  sendCommonHeaders(client, "text/plain; charset=utf-8");
  client.print("commanded_power_state=");
  client.println(stateName(powerState));
  client.print("state=");
  client.println(stateName(powerState));
  client.print("slrt_ping_status=");
  client.println(pingStatusToString(slrtPingStatus));
  client.print("slrt_target_ip=");
  printAddress(client, slrtTargetIP);
  client.print("last_ping_check_ms_ago=");
  if (hasPingAttempt) {
    client.println(millis() - lastPingAttemptMs);
  } else {
    client.println("-1");
  }
  client.print("failed_ping_count=");
  client.println(consecutiveFailedPings);
  client.print("boot_grace_remaining_ms=");
  client.println(bootGraceRemainingMs());
  client.print("relay_pin=");
  client.println(RELAY_PIN);
  client.print("restart_in_progress=");
  client.println(powerState == RESTARTING ? "true" : "false");
  client.print("restart_delay_seconds=");
  client.println(restartDelaySeconds);
  client.print("continuous_ping_enabled=");
  client.println(continuousPingEnabled ? "true" : "false");
  client.print("ping_interval_seconds=");
  client.println(pingIntervalSeconds);
}

void sendMainPage(EthernetClient &client, const char *message) {
  sendCommonHeaders(client, "text/html; charset=utf-8");
  client.println("<!doctype html><html><head>");
  client.println("<meta name=\"viewport\" "
                 "content=\"width=device-width,initial-scale=1\">");
  client.println("<title>Uno32 Power Cycler</title>");
  client.println(
      "<style>body{font-family:sans-serif;max-width:36rem;margin:3rem "
      "auto;padding:0 1rem}"
      "button{font-size:1rem;margin:.35rem;padding:.8rem 1.1rem}"
      ".status,.settings{padding:1rem;background:#eee;border-radius:.5rem}"
      ".settings{margin-top:1rem}.status p{margin:.55rem 0}"
      "input[type=range]{width:100%}.responding{color:#08752d}"
      ".not-responding{color:#b00020}.checking{color:#8a5a00}"
      "input[type=number]{font-size:1rem;width:5rem;padding:.35rem}"
      "input:disabled{color:#777;background:#ddd}</style>");
  client.println("</head><body><h1>Uno32 Ethernet Power Cycler</h1>");
  if (message != NULL) {
    client.print("<p><strong>");
    client.print(message);
    client.println("</strong></p>");
  }
  client.println("<div class=\"status\">");
  client.print("<p>Commanded Power State: <strong id=\"powerState\">");
  client.print(stateName(powerState));
  client.println("</strong></p>");
  client.print("<p>SLRT Target Ping Status: <strong id=\"pingStatus\" class=\"");
  if (slrtPingStatus == PING_RESPONDING) {
    client.print("responding");
  } else if (slrtPingStatus == PING_NOT_RESPONDING) {
    client.print("not-responding");
  } else if (slrtPingStatus == PING_CHECKING ||
             slrtPingStatus == PING_BOOTING) {
    client.print("checking");
  }
  client.print("\">");
  client.print(pingStatusToString(slrtPingStatus));
  client.println("</strong></p>");
  client.print("<p>Target IP: <span id=\"targetIp\">");
  printAddress(client, slrtTargetIP, false);
  client.println("</span></p>");
  client.print("<p>Last Ping Check: <span id=\"lastPing\">");
  if (hasPingAttempt) {
    client.print((millis() - lastPingAttemptMs) / 1000UL);
    client.println(" seconds ago</span></p>");
  } else {
    client.println("never</span></p>");
  }
  client.print("<p>Failed Ping Count: <span id=\"failedPings\">");
  client.print(consecutiveFailedPings);
  client.println("</span></p>");
  client.print("<p id=\"bootGraceRow\"");
  if (!bootGraceActive) {
    client.print(" style=\"display:none\"");
  }
  client.print(">Boot Grace Remaining: <span id=\"bootGrace\">");
  client.print(bootGraceRemainingMs() / 1000UL);
  client.println(" seconds</span></p>");
  client.print("<p>Relay pin: D");
  client.print(RELAY_PIN);
  client.println("</p>");
  client.print("<p>Restart delay: ");
  client.print(restartDelaySeconds);
  client.println(" seconds</p></div>");
  client.println("<p><a href=\"/restart\"><button>Restart</button></a>"
                 "<a href=\"/off\"><button>Turn Off</button></a>"
                 "<a href=\"/on\"><button>Turn On</button></a>"
                 "<a href=\"/pingnow\"><button>Check Ping Now</button></a></p>");
  client.println("<form class=\"settings\" action=\"/delay\" method=\"get\">");
  client.println("<label for=\"seconds\"><strong>Restart delay: "
                 "<span id=\"delayValue\"></span> seconds</strong></label>");
  client.print("<input id=\"seconds\" name=\"seconds\" type=\"range\" min=\"");
  client.print(MIN_RESTART_DELAY_SECONDS);
  client.print("\" max=\"");
  client.print(MAX_RESTART_DELAY_SECONDS);
  client.print("\" step=\"1\" value=\"");
  client.print(restartDelaySeconds);
  client.println("\" oninput=\"delayValue.textContent=this.value\" "
                 "onchange=\"this.form.submit()\">");
  client.println("<button type=\"submit\">Save delay</button></form>");
  client.println(
      "<form class=\"settings\" action=\"/pingsettings\" method=\"get\">");
  client.print("<label><input id=\"continuousPing\" name=\"enabled\" "
               "type=\"checkbox\" value=\"1\"");
  if (continuousPingEnabled) {
    client.print(" checked");
  }
  client.println(" onchange=\"pingInterval.disabled=!this.checked;"
                 "this.form.submit()\"> <strong>Continuous ping "
                 "checks</strong></label><p>");
  client.print("<label for=\"pingInterval\">Ping interval: </label>"
               "<input id=\"pingInterval\" name=\"interval\" type=\"number\" "
               "min=\"");
  client.print(MIN_PING_INTERVAL_SECONDS);
  client.print("\" max=\"");
  client.print(MAX_PING_INTERVAL_SECONDS);
  client.print("\" value=\"");
  client.print(pingIntervalSeconds);
  client.print("\"");
  if (!continuousPingEnabled) {
    client.print(" disabled");
  }
  client.println("> seconds</p><button type=\"submit\">Save ping "
                 "settings</button></form>");
  client.println("<p><a href=\"/status\">Plain-text status</a></p>");
  client.println("<p><small>Status is commanded state only; AC power is not "
                 "physically verified. Ping reports network reachability "
                 "only.</small></p>");
  client.println("<script>"
                 "delayValue.textContent=seconds.value;"
                 "setInterval(function(){fetch('/status',{cache:'no-store'})"
                 ".then(function(r){return r.text()})"
                 ".then(function(t){var v={};t.trim().split('\\n').forEach("
                 "function(l){var i=l.indexOf('=');if(i>0)v[l.slice(0,i)]="
                 "l.slice(i+1)});"
                 "powerState.textContent=v.commanded_power_state;"
                 "pingStatus.textContent=v.slrt_ping_status;"
                 "pingStatus.className=v.slrt_ping_status==="
                 "'PING_RESPONDING'?'responding':v.slrt_ping_status==="
                 "'PING_NOT_RESPONDING'?'not-responding':"
                 "(v.slrt_ping_status==='PING_CHECKING'||"
                 "v.slrt_ping_status==='PING_BOOTING')?'checking':'';"
                 "targetIp.textContent=v.slrt_target_ip;"
                 "failedPings.textContent=v.failed_ping_count;"
                 "var age=Number(v.last_ping_check_ms_ago);"
                 "lastPing.textContent=age<0?'never':Math.floor(age/1000)+"
                 "' seconds ago';"
                 "var grace=Number(v.boot_grace_remaining_ms);"
                 "bootGraceRow.style.display=grace>0?'':'none';"
                 "bootGrace.textContent=Math.ceil(grace/1000)+' seconds'})"
                 ".catch(function(){})},1000);"
                 "</script>");
  client.println("</body></html>");
}

void sendNotFound(EthernetClient &client) {
  client.println("HTTP/1.1 404 Not Found");
  client.println("Content-Type: text/plain; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.println("Not found");
}

bool parseDelaySeconds(const char *query, uint8_t &seconds) {
  const char prefix[] = "seconds=";
  if (query == NULL || strncmp(query, prefix, sizeof(prefix) - 1) != 0) {
    return false;
  }

  char *end = NULL;
  long value = strtol(query + sizeof(prefix) - 1, &end, 10);
  if (*end != '\0' || value < MIN_RESTART_DELAY_SECONDS ||
      value > MAX_RESTART_DELAY_SECONDS) {
    return false;
  }

  seconds = static_cast<uint8_t>(value);
  return true;
}

bool queryHasEnabled(const char *query) {
  if (query == NULL) {
    return false;
  }

  return strncmp(query, "enabled=1", 9) == 0 ||
         strstr(query, "&enabled=1") != NULL;
}

bool parsePingInterval(const char *query, uint16_t &seconds) {
  if (query == NULL) {
    return false;
  }

  const char *valueStart = strstr(query, "interval=");
  if (valueStart == NULL) {
    return false;
  }

  valueStart += strlen("interval=");
  char *end = NULL;
  unsigned long value = strtoul(valueStart, &end, 10);
  if ((*end != '\0' && *end != '&') ||
      value < MIN_PING_INTERVAL_SECONDS ||
      value > MAX_PING_INTERVAL_SECONDS) {
    return false;
  }

  seconds = static_cast<uint16_t>(value);
  return true;
}

void routeRequest(EthernetClient &client, const char *path, const char *query) {
  if (strcmp(path, "/") == 0) {
    sendMainPage(client, NULL);
  } else if (strcmp(path, "/status") == 0) {
    sendStatus(client);
  } else if (strcmp(path, "/on") == 0) {
    Serial.println("Web action: ON");
    setPowerOn();
    sendRedirectToDashboard(client);
  } else if (strcmp(path, "/off") == 0) {
    Serial.println("Web action: OFF");
    setPowerOff();
    sendRedirectToDashboard(client);
  } else if (strcmp(path, "/restart") == 0) {
    Serial.println("Web action: RESTART");
    restartPowerCycle();
    sendRedirectToDashboard(client);
  } else if (strcmp(path, "/pingnow") == 0) {
    Serial.println("Web action: PING NOW");
    requestPingCheck();
    sendRedirectToDashboard(client);
  } else if (strcmp(path, "/delay") == 0) {
    uint8_t seconds = 0;
    if (parseDelaySeconds(query, seconds)) {
      if (saveRestartDelay(seconds)) {
        Serial.print("Restart delay saved and verified: ");
        Serial.print(seconds);
        Serial.println(" seconds.");
      } else {
        Serial.println("Restart delay EEPROM verification failed.");
      }
    } else {
      Serial.println("Invalid restart delay received from web.");
    }
    sendRedirectToDashboard(client);
  } else if (strcmp(path, "/pingsettings") == 0) {
    bool enabled = queryHasEnabled(query);
    uint16_t intervalSeconds = pingIntervalSeconds;
    bool intervalProvided = parsePingInterval(query, intervalSeconds);
    if ((!enabled && !intervalProvided) ||
        isValidPingInterval(intervalSeconds)) {
      if (savePingSettings(enabled, intervalSeconds)) {
        pingCheckRequested = enabled && powerState == POWER_ON;
        Serial.print("Continuous ping ");
        Serial.print(enabled ? "enabled" : "disabled");
        Serial.print(" at ");
        Serial.print(intervalSeconds);
        Serial.println("-second intervals.");
      } else {
        Serial.println("Ping settings EEPROM verification failed.");
      }
    } else {
      Serial.println("Invalid ping interval received from web.");
    }
    sendRedirectToDashboard(client);
  } else {
    sendNotFound(client);
  }
}

void handleWebClient() {
  EthernetClient client = server.available();
  if (!client) {
    return;
  }

  char requestLine[96];
  size_t length = 0;
  unsigned long connectedAt = millis();
  bool firstLineComplete = false;

  while (client.connected() && millis() - connectedAt < 1000UL) {
    updateRestartState();
    handleSerial();

    while (client.available() > 0) {
      char incoming = static_cast<char>(client.read());
      if (incoming == '\n') {
        requestLine[length] = '\0';
        firstLineComplete = true;
        break;
      }
      if (incoming != '\r' && length < sizeof(requestLine) - 1) {
        requestLine[length++] = incoming;
      }
    }
    if (firstLineComplete) {
      break;
    }
  }

  char method[8] = {0};
  char path[48] = {0};
  if (firstLineComplete && sscanf(requestLine, "%7s %47s", method, path) == 2 &&
      strcmp(method, "GET") == 0) {
    char *query = strchr(path, '?');
    if (query != NULL) {
      *query++ = '\0';
    }
    routeRequest(client, path, query);
  } else {
    client.println("HTTP/1.1 400 Bad Request");
    client.println("Connection: close");
    client.println();
  }

  delay(1);
  client.stop();
}

void setup() {
  // Drive the Normally ON outlet to its safe state before starting peripherals.
  digitalWrite(RELAY_PIN, RELAY_SIGNAL_FOR_POWER_ON ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  powerState = POWER_ON;

  pinMode(SHIELD_SD_CS_PIN, OUTPUT);
  digitalWrite(SHIELD_SD_CS_PIN, HIGH);
  pinMode(ETHERNET_CS_PIN, OUTPUT);
  digitalWrite(ETHERNET_CS_PIN, HIGH);

  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println("Uno32 Ethernet Power Cycler Ready");

  loadSettings();
  Ethernet.init(ETHERNET_CS_PIN);
  Ethernet.begin(mac, ip, dnsServer, gateway, subnet);
  server.begin();

  printEthernetDiagnostics();
  Serial.print("IP address: ");
  printIpAddress(Serial);
  printStatus(Serial);
  Serial.println("Type 'help' for commands.");
}

void loop() {
  updateRestartState();
  handleSerial();
  handleWebClient();
  updatePingMonitor();
}
