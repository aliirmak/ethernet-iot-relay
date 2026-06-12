#include <Arduino.h>
#include <EEPROM.h>
#include <Ethernet.h>
#include <SPI.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
const uint8_t SETTINGS_VERSION = 1;

// Edit these values to match the local network.
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x32, 0x01};
IPAddress ip(192, 168, 0, 50);
IPAddress dnsServer(192, 168, 0, 1);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);

EthernetServer server(80);

enum PowerState {
  POWER_ON,
  POWER_OFF,
  RESTARTING,
  RESTART_COMPLETE,
  UNKNOWN
};

PowerState powerState = UNKNOWN;
unsigned long restartStartedAt = 0;
uint8_t restartDelaySeconds = DEFAULT_RESTART_DELAY_SECONDS;

struct PersistentSettings {
  uint16_t magic;
  uint8_t version;
  uint8_t restartDelaySeconds;
  uint8_t checksum;
};

uint8_t settingsChecksum(const PersistentSettings &settings) {
  return static_cast<uint8_t>(
      settings.magic ^ (settings.magic >> 8) ^ settings.version ^
      settings.restartDelaySeconds ^ 0xA5);
}

bool isValidRestartDelay(uint8_t seconds) {
  return seconds >= MIN_RESTART_DELAY_SECONDS &&
         seconds <= MAX_RESTART_DELAY_SECONDS;
}

void loadSettings() {
  PersistentSettings settings;
  EEPROM.get(0, settings);

  if (settings.magic == SETTINGS_MAGIC &&
      settings.version == SETTINGS_VERSION &&
      settings.checksum == settingsChecksum(settings) &&
      isValidRestartDelay(settings.restartDelaySeconds)) {
    restartDelaySeconds = settings.restartDelaySeconds;
    return;
  }

  restartDelaySeconds = DEFAULT_RESTART_DELAY_SECONDS;
}

void saveRestartDelay(uint8_t seconds) {
  PersistentSettings settings = {SETTINGS_MAGIC, SETTINGS_VERSION, seconds, 0};
  settings.checksum = settingsChecksum(settings);
  EEPROM.put(0, settings);
  restartDelaySeconds = seconds;
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

void writeRelaySignal(bool signal) {
  digitalWrite(RELAY_PIN, signal ? HIGH : LOW);
}

void setPowerOn() {
  writeRelaySignal(RELAY_SIGNAL_FOR_POWER_ON);
  powerState = POWER_ON;
  Serial.println("Power commanded ON.");
}

void setPowerOff() {
  writeRelaySignal(RELAY_SIGNAL_FOR_POWER_OFF);
  powerState = POWER_OFF;
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
  Serial.println("Restart started: power commanded OFF.");
  return true;
}

void updateRestartState() {
  if (powerState == RESTARTING &&
      millis() - restartStartedAt >= restartDelayMilliseconds()) {
    writeRelaySignal(RELAY_SIGNAL_FOR_POWER_ON);
    powerState = RESTART_COMPLETE;
    Serial.println("Restart complete.");
  }
}

void printIpAddress(Stream &output) {
  IPAddress currentIp = Ethernet.localIP();
  output.print(currentIp[0]);
  output.print('.');
  output.print(currentIp[1]);
  output.print('.');
  output.print(currentIp[2]);
  output.print('.');
  output.println(currentIp[3]);
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

void printStatus(Stream &output) {
  output.print("State: ");
  output.println(stateName(powerState));
  output.print("Relay pin: ");
  output.println(RELAY_PIN);
  output.print("Restart delay: ");
  output.print(restartDelayMilliseconds());
  output.println(" ms");
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  help     Show this help");
  Serial.println("  status   Show commanded state");
  Serial.println("  on       Command power ON");
  Serial.println("  off      Command power OFF");
  Serial.println("  restart  Start a power cycle using the saved delay");
  Serial.println("  ip       Show the configured IP address");
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

void sendStatus(EthernetClient &client) {
  sendCommonHeaders(client, "text/plain; charset=utf-8");
  client.print("state=");
  client.println(stateName(powerState));
  client.print("relay_pin=");
  client.println(RELAY_PIN);
  client.print("restart_in_progress=");
  client.println(powerState == RESTARTING ? "true" : "false");
  client.print("restart_delay_seconds=");
  client.println(restartDelaySeconds);
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
      ".settings{margin-top:1rem}input[type=range]{width:100%}</style>");
  client.println("</head><body><h1>Uno32 Ethernet Power Cycler</h1>");
  if (message != NULL) {
    client.print("<p><strong>");
    client.print(message);
    client.println("</strong></p>");
  }
  client.println("<div class=\"status\">");
  client.print("<p>Status: <strong id=\"powerState\">");
  client.print(stateName(powerState));
  client.println("</strong></p>");
  client.print("<p>Relay pin: D");
  client.print(RELAY_PIN);
  client.println("</p>");
  client.print("<p>Restart delay: ");
  client.print(restartDelaySeconds);
  client.println(" seconds</p></div>");
  client.println("<p><a href=\"/restart\"><button>Restart</button></a>"
                 "<a href=\"/off\"><button>Turn Off</button></a>"
                 "<a href=\"/on\"><button>Turn On</button></a></p>");
  client.println("<form class=\"settings\" action=\"/delay\" method=\"get\">");
  client.println("<label for=\"seconds\"><strong>Restart delay: "
                 "<span id=\"delayValue\"></span> seconds</strong></label>");
  client.print("<input id=\"seconds\" name=\"seconds\" type=\"range\" min=\"");
  client.print(MIN_RESTART_DELAY_SECONDS);
  client.print("\" max=\"");
  client.print(MAX_RESTART_DELAY_SECONDS);
  client.print("\" step=\"1\" value=\"");
  client.print(restartDelaySeconds);
  client.println("\" oninput=\"delayValue.textContent=this.value\">");
  client.println("<button type=\"submit\">Save delay</button></form>");
  client.println("<p><a href=\"/status\">Plain-text status</a></p>");
  client.println("<p><small>Status is commanded state only; AC power is not "
                 "physically verified.</small></p>");
  client.println(
      "<script>"
      "delayValue.textContent=seconds.value;"
      "setInterval(function(){fetch('/status',{cache:'no-store'})"
      ".then(function(r){return r.text()})"
      ".then(function(t){var m=t.match(/^state=(.+)$/m);"
      "if(m)powerState.textContent=m[1]})"
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

void routeRequest(EthernetClient &client, const char *path, const char *query) {
  if (strcmp(path, "/") == 0) {
    sendMainPage(client, NULL);
  } else if (strcmp(path, "/status") == 0) {
    sendStatus(client);
  } else if (strcmp(path, "/on") == 0) {
    Serial.println("Web action: ON");
    setPowerOn();
    sendMainPage(client, "Power commanded ON.");
  } else if (strcmp(path, "/off") == 0) {
    Serial.println("Web action: OFF");
    setPowerOff();
    sendMainPage(client, "Power commanded OFF.");
  } else if (strcmp(path, "/restart") == 0) {
    Serial.println("Web action: RESTART");
    bool started = restartPowerCycle();
    sendMainPage(client,
                 started ? "Restart started." : "Restart already in progress.");
  } else if (strcmp(path, "/delay") == 0) {
    uint8_t seconds = 0;
    if (parseDelaySeconds(query, seconds)) {
      saveRestartDelay(seconds);
      Serial.print("Restart delay saved: ");
      Serial.print(seconds);
      Serial.println(" seconds.");
      sendMainPage(client, "Restart delay saved.");
    } else {
      sendMainPage(client, "Invalid restart delay.");
    }
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
}
