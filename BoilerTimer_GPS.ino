#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <SoftwareSerial.h>

// Pin configuration for ESP-01
#define RELAY_PORT 0    // GPIO0 for relay
#define GPS_RX_PIN 3    // RX pin for GPS (GPIO3 = RXD0)
#define GPS_TX_PIN 1    // TX pin for GPS (GPIO1 = TXD0) - for UBX commands

// Global variables
bool relayState = false;
bool manualMode = false;
ESP8266WebServer server(80);

// WiFi reconnection management
unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 30000; // Check every 30 seconds
int wifiReconnectAttempts = 0;
const int maxReconnectAttempts = 5;

// Time management
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);
bool useGPS = true;
bool autoDST = true;
int timeZoneOffset = 1; // CET = UTC+1
String ntpServer = "pool.ntp.org";

// GPS management
String gpsTime = "";
bool gpsTimeValid = false;
unsigned long lastGPSRead = 0;
unsigned long lastValidGPSTime = 0;
const unsigned long gpsTimeoutMs = 300000; // 5 minutes GPS timeout

// GPS auto-detection
enum GPSProtocol {
  GPS_UNKNOWN,
  GPS_NMEA,
  GPS_UBX
};

GPSProtocol gpsProtocol = GPS_UNKNOWN;
int gpsBaudRate = 9600;
unsigned long gpsDetectionStart = 0;
const unsigned long gpsDetectionTimeout = 10000; // 10 seconds per baudrate/protocol
bool gpsDetectionComplete = false;
int currentBaudrateIndex = 0;
int currentProtocolIndex = 0;

// Common GPS baudrates to try
const int gpsBaudrates[] = {9600, 4800, 38400, 57600, 115200};
const int numBaudrates = sizeof(gpsBaudrates) / sizeof(gpsBaudrates[0]);

// UBX commands
const unsigned char ubxSetNMEA[] = {
  0xB5, 0x62, 0x06, 0x00, 0x14, 0x00, // UBX header + CFG-PRT
  0x01, 0x00, 0x00, 0x00, 0xD0, 0x08, 0x00, 0x00, // Port 1, reserved, txReady
  0x80, 0x25, 0x00, 0x00, 0x07, 0x00, 0x03, 0x00, // Baudrate 9600, in/out NMEA
  0x00, 0x00, 0x00, 0x00, // Reserved
  0xA2, 0xB5 // Checksum
};

// Schedule storage (EEPROM addresses)
struct TimeSlot {
  int startHour;
  int startMinute;
  int endHour;
  int endMinute;
  bool enabled;
};

#define MAX_TIMESLOTS 10
TimeSlot timeSlots[MAX_TIMESLOTS];

// EEPROM addresses
#define SSID_ADDR 0      // 32 bytes
#define PASS_ADDR 32     // 32 bytes
#define TIMEZONE_ADDR 64 // 4 bytes
#define AUTODST_ADDR 68  // 1 byte
#define USEGPS_ADDR 69   // 1 byte
#define NTP_ADDR 70      // 32 bytes
#define SLOTS_ADDR 102   // sizeof(TimeSlot) * MAX_TIMESLOTS
#define GPS_PROTOCOL_ADDR 200 // 1 byte
#define GPS_BAUDRATE_ADDR 201 // 4 bytes

void setup() {
  EEPROM.begin(512);
  
  // Initialize pins
  pinMode(RELAY_PORT, OUTPUT);
  digitalWrite(RELAY_PORT, HIGH); // Relay off
  pinMode(LED_BUILTIN, OUTPUT);
  
  // Load settings from EEPROM
  loadSettings();
  
  // Initialize GPS detection
  gpsDetectionStart = millis();
  Serial.begin(gpsBaudrates[currentBaudrateIndex]);
  
  // Connect to WiFi
  connectWiFi();
  
  // Start web server
  setupWebServer();
  server.begin();
  
  // Initialize NTP
  timeClient.setTimeOffset(timeZoneOffset * 3600);
  timeClient.begin();
  
  Serial.println();
  Serial.println("Boiler Timer started - GPS auto-detection in progress...");
}

void loop() {
  // Handle WiFi reconnection
  checkWiFiConnection();
  
  server.handleClient();
  
  // GPS auto-detection and reading
  if (useGPS) {
    if (!gpsDetectionComplete) {
      detectGPS();
    } else {
      readGPS();
    }
  }
  
  // Update time
  updateTime();
  
  // Check schedule only if not in manual mode
  if (!manualMode) {
    checkSchedule();
  }
  
  delay(100);
}

void detectGPS() {
  unsigned long currentTime = millis();
  
  // Read any available data for analysis
  String receivedData = "";
  while (Serial.available()) {
    char c = Serial.read();
    receivedData += c;
    if (receivedData.length() > 200) break; // Limit buffer size
  }
  
  // Check for NMEA data
  if (receivedData.indexOf("$GP") >= 0 || receivedData.indexOf("$GN") >= 0) {
    gpsProtocol = GPS_NMEA;
    gpsBaudRate = gpsBaudrates[currentBaudrateIndex];
    gpsDetectionComplete = true;
    saveGPSSettings();
    Serial.println();
    Serial.print("GPS detected: NMEA at ");
    Serial.print(gpsBaudRate);
    Serial.println(" baud");
    return;
  }
  
  // Check for UBX data (starts with 0xB5 0x62)
  if (receivedData.indexOf("\xB5\x62") >= 0) {
    gpsProtocol = GPS_UBX;
    gpsBaudRate = gpsBaudrates[currentBaudrateIndex];
    
    // Try to configure UBX to NMEA
    Serial.println();
    Serial.print("GPS detected: UBX at ");
    Serial.print(gpsBaudRate);
    Serial.println(" baud - Converting to NMEA...");
    
    // Send UBX command to switch to NMEA
    for (int i = 0; i < sizeof(ubxSetNMEA); i++) {
      Serial.write(ubxSetNMEA[i]);
    }
    delay(1000);
    
    // Switch to 9600 baud for NMEA
    Serial.end();
    delay(100);
    Serial.begin(9600);
    gpsBaudRate = 9600;
    gpsProtocol = GPS_NMEA;
    
    gpsDetectionComplete = true;
    saveGPSSettings();
    Serial.println("UBX converted to NMEA at 9600 baud");
    return;
  }
  
  // Timeout for current baudrate/protocol combination
  if (currentTime - gpsDetectionStart > gpsDetectionTimeout) {
    currentBaudrateIndex++;
    
    if (currentBaudrateIndex >= numBaudrates) {
      // Tried all baudrates, give up or try a different approach
      Serial.println();
      Serial.println("GPS auto-detection failed - no valid GPS data found");
      gpsDetectionComplete = true;
      gpsProtocol = GPS_UNKNOWN;
      return;
    }
    
    // Try next baudrate
    Serial.end();
    delay(100);
    Serial.begin(gpsBaudrates[currentBaudrateIndex]);
    gpsDetectionStart = currentTime;
    
    Serial.print("Trying GPS at ");
    Serial.print(gpsBaudrates[currentBaudrateIndex]);
    Serial.println(" baud...");
  }
}

void saveGPSSettings() {
  EEPROM.write(GPS_PROTOCOL_ADDR, (int)gpsProtocol);
  EEPROM.put(GPS_BAUDRATE_ADDR, gpsBaudRate);
  EEPROM.commit();
}

void loadGPSSettings() {
  int savedProtocol = EEPROM.read(GPS_PROTOCOL_ADDR);
  if (savedProtocol >= GPS_UNKNOWN && savedProtocol <= GPS_UBX) {
    gpsProtocol = (GPSProtocol)savedProtocol;
  }
  
  EEPROM.get(GPS_BAUDRATE_ADDR, gpsBaudRate);
  if (gpsBaudRate <= 0 || gpsBaudRate > 115200) {
    gpsBaudRate = 9600; // Default
  }
  
  // If we have saved settings, skip detection
  if (gpsProtocol != GPS_UNKNOWN) {
    gpsDetectionComplete = true;
    Serial.begin(gpsBaudRate);
    Serial.print("Using saved GPS settings: ");
    Serial.print(gpsProtocol == GPS_NMEA ? "NMEA" : "UBX");
    Serial.print(" at ");
    Serial.print(gpsBaudRate);
    Serial.println(" baud");
  }
}

void checkWiFiConnection() {
  if (millis() - lastWiFiCheck > wifiCheckInterval) {
    lastWiFiCheck = millis();
    
    String ssid = readStringFromEEPROM(SSID_ADDR, 32);
    if (ssid.length() > 0 && WiFi.getMode() != WIFI_AP) {
      
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected, attempting reconnection...");
        
        wifiReconnectAttempts++;
        
        if (wifiReconnectAttempts <= maxReconnectAttempts) {
          WiFi.disconnect();
          delay(1000);
          
          String password = readStringFromEEPROM(PASS_ADDR, 32);
          WiFi.begin(ssid.c_str(), password.c_str());
          
          int attempts = 0;
          while (WiFi.status() != WL_CONNECTED && attempts < 10) {
            delay(500);
            attempts++;
          }
          
          if (WiFi.status() == WL_CONNECTED) {
            Serial.println("WiFi reconnected successfully");
            wifiReconnectAttempts = 0;
          }
        } else {
          Serial.println("Max reconnection attempts reached, starting AP mode");
          WiFi.softAP("BoilerTimer-Backup", "12345678");
          wifiReconnectAttempts = 0;
        }
      } else {
        wifiReconnectAttempts = 0;
      }
    }
  }
}

void loadSettings() {
  // Load GPS settings first
  loadGPSSettings();
  
  // Load time settings
  EEPROM.get(TIMEZONE_ADDR, timeZoneOffset);
  autoDST = EEPROM.read(AUTODST_ADDR);
  useGPS = EEPROM.read(USEGPS_ADDR);
  ntpServer = readStringFromEEPROM(NTP_ADDR, 32);
  
  // Load time slots
  EEPROM.get(SLOTS_ADDR, timeSlots);
  
  // Set defaults
  if (ntpServer.length() == 0) {
    ntpServer = "pool.ntp.org";
  }
  
  // Initialize default time slots if empty
  bool hasValidSlot = false;
  for (int i = 0; i < MAX_TIMESLOTS; i++) {
    if (timeSlots[i].enabled) {
      hasValidSlot = true;
      break;
    }
  }
  
  if (!hasValidSlot) {
    // Set some default time slots
    timeSlots[0] = {6, 0, 8, 0, true};   // 6:00-8:00
    timeSlots[1] = {17, 0, 22, 0, true}; // 17:00-22:00
    for (int i = 2; i < MAX_TIMESLOTS; i++) {
      timeSlots[i] = {0, 0, 0, 0, false};
    }
    saveSettings();
  }
}

void saveSettings() {
  EEPROM.put(TIMEZONE_ADDR, timeZoneOffset);
  EEPROM.write(AUTODST_ADDR, autoDST);
  EEPROM.write(USEGPS_ADDR, useGPS);
  writeStringToEEPROM(NTP_ADDR, ntpServer, 32);
  EEPROM.put(SLOTS_ADDR, timeSlots);
  EEPROM.commit();
}

String readStringFromEEPROM(int addr, int maxLen) {
  String result = "";
  for (int i = 0; i < maxLen; i++) {
    char c = EEPROM.read(addr + i);
    if (c == 0 || c == 255) break;
    result += c;
  }
  return result;
}

void writeStringToEEPROM(int addr, String str, int maxLen) {
  for (int i = 0; i < maxLen; i++) {
    if (i < str.length()) {
      EEPROM.write(addr + i, str[i]);
    } else {
      EEPROM.write(addr + i, 0);
    }
  }
}

void connectWiFi() {
  String ssid = readStringFromEEPROM(SSID_ADDR, 32);
  String password = readStringFromEEPROM(PASS_ADDR, 32);
  
  if (ssid.length() > 0) {
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      Serial.println("WiFi connected!");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println();
      Serial.println("WiFi connection failed, starting AP mode");
      WiFi.softAP("BoilerTimer", "12345678");
    }
  } else {
    Serial.println("No WiFi credentials, starting AP mode");
    WiFi.softAP("BoilerTimer", "12345678");
  }
}

void updateTime() {
  if (!useGPS || !isGPSTimeValid()) {
    if (WiFi.status() == WL_CONNECTED) {
      timeClient.setPoolServerName(ntpServer.c_str());
      timeClient.update();
    }
  }
}

bool isGPSTimeValid() {
  return gpsTimeValid && (millis() - lastValidGPSTime < gpsTimeoutMs);
}

void readGPS() {
  if (millis() - lastGPSRead > 1000) {
    lastGPSRead = millis();
    
    while (Serial.available()) {
      String gpsData = Serial.readStringUntil('\n');
      
      if (gpsProtocol == GPS_NMEA) {
        // Parse NMEA sentences
        if (gpsData.startsWith("$GPRMC") || gpsData.startsWith("$GNRMC")) {
          if (parseNMEATime(gpsData)) {
            lastValidGPSTime = millis();
          }
        }
      } else if (gpsProtocol == GPS_UBX) {
        // UBX parsing would go here if needed
        // For now, we convert UBX to NMEA during detection
      }
    }
  }
}

bool parseNMEATime(String nmea) {
  // Parse NMEA RMC sentence: $GPRMC,hhmmss.ss,A,ddmm.mmmm,N,dddmm.mmmm,E,speed,course,ddmmyy,,,A*hh
  int firstComma = nmea.indexOf(',');
  if (firstComma == -1) return false;
  
  // Get time field
  int timeStart = firstComma + 1;
  int timeEnd = nmea.indexOf(',', timeStart);
  if (timeEnd == -1) return false;
  
  // Get status field (should be 'A' for valid)
  int statusStart = timeEnd + 1;
  int statusEnd = nmea.indexOf(',', statusStart);
  if (statusEnd == -1) return false;
  
  String status = nmea.substring(statusStart, statusEnd);
  if (status != "A") return false; // Invalid GPS fix
  
  String timeStr = nmea.substring(timeStart, timeEnd);
  if (timeStr.length() >= 6) {
    int hours = timeStr.substring(0, 2).toInt();
    int minutes = timeStr.substring(2, 4).toInt();
    int seconds = timeStr.substring(4, 6).toInt();
    
    // Apply timezone offset and DST
    hours += timeZoneOffset;
    if (autoDST && isDST()) {
      hours += 1;
    }
    
    if (hours >= 24) hours -= 24;
    if (hours < 0) hours += 24;
    
    gpsTime = "";
    if (hours < 10) gpsTime += "0";
    gpsTime += String(hours) + ":";
    if (minutes < 10) gpsTime += "0";
    gpsTime += String(minutes) + ":";
    if (seconds < 10) gpsTime += "0";
    gpsTime += String(seconds);
    
    gpsTimeValid = true;
    return true;
  }
  return false;
}

bool isDST() {
  time_t now;
  
  if (isGPSTimeValid()) {
    now = timeClient.getEpochTime();
  } else if (WiFi.status() == WL_CONNECTED) {
    now = timeClient.getEpochTime();
  } else {
    return false;
  }
  
  int year = ::year(now);
  int month = ::month(now);
  int day = ::day(now);
  
  if (month < 3 || month > 10) return false;
  if (month > 3 && month < 10) return true;
  
  int lastSundayMarch = 31 - ((5 * year / 4 + 4) % 7);
  int lastSundayOctober = 31 - ((5 * year / 4 + 1) % 7);
  
  if (month == 3) return day >= lastSundayMarch;
  if (month == 10) return day < lastSundayOctober;
  
  return false;
}

void checkSchedule() {
  int currentHour, currentMinute;
  
  if (useGPS && isGPSTimeValid()) {
    int colonIndex = gpsTime.indexOf(':');
    currentHour = gpsTime.substring(0, colonIndex).toInt();
    currentMinute = gpsTime.substring(colonIndex + 1, gpsTime.indexOf(':', colonIndex + 1)).toInt();
  } else if (WiFi.status() == WL_CONNECTED) {
    time_t now = timeClient.getEpochTime();
    if (autoDST && isDST()) {
      now += 3600;
    }
    currentHour = hour(now);
    currentMinute = minute(now);
  } else {
    return;
  }
  
  bool shouldBeOn = false;
  
  for (int i = 0; i < MAX_TIMESLOTS; i++) {
    if (timeSlots[i].enabled) {
      int startTime = timeSlots[i].startHour * 60 + timeSlots[i].startMinute;
      int endTime = timeSlots[i].endHour * 60 + timeSlots[i].endMinute;
      int currentTime = currentHour * 60 + currentMinute;
      
      if (startTime <= endTime) {
        if (currentTime >= startTime && currentTime < endTime) {
          shouldBeOn = true;
          break;
        }
      } else {
        if (currentTime >= startTime || currentTime < endTime) {
          shouldBeOn = true;
          break;
        }
      }
    }
  }
  
  if (shouldBeOn != relayState) {
    relayState = shouldBeOn;
    digitalWrite(RELAY_PORT, relayState ? LOW : HIGH);
    digitalWrite(LED_BUILTIN, relayState ? LOW : HIGH);
  }
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/settings", handleSettings);
  server.on("/save", handleSave);
  server.on("/wifi", handleWiFi);
  server.on("/savewifi", handleSaveWiFi);
  server.on("/manual", handleManual);
  server.on("/status", handleStatus);
  server.on("/resetgps", handleResetGPS);
}

void handleResetGPS() {
  // Reset GPS detection
  gpsProtocol = GPS_UNKNOWN;
  gpsDetectionComplete = false;
  currentBaudrateIndex = 0;
  gpsDetectionStart = millis();
  
  // Clear saved GPS settings
  EEPROM.write(GPS_PROTOCOL_ADDR, GPS_UNKNOWN);
  EEPROM.put(GPS_BAUDRATE_ADDR, 9600);
  EEPROM.commit();
  
  // Restart serial
  Serial.end();
  delay(100);
  Serial.begin(gpsBaudrates[0]);
  
  server.send(200, "text/html", 
    "<!DOCTYPE html><html><head><title>GPS Reset</title></head><body>"
    "<h1>GPS Detection Reset</h1>"
    "<p>GPS auto-detection restarted. Return to <a href='/'>main page</a>.</p>"
    "</body></html>");
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><title>Minuterie Chaudiere</title>";
  html += "<meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='30'>";
  html += "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}";
  html += ".container{max-width:600px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 0 10px rgba(0,0,0,0.1)}";
  html += ".status{padding:10px;margin:10px 0;border-radius:5px;text-align:center;font-weight:bold}";
  html += ".on{background:#4CAF50;color:white}.off{background:#f44336;color:white}";
  html += ".info{background:#2196F3;color:white;padding:5px;margin:5px 0;border-radius:3px;font-size:12px}";
  html += "button{padding:10px 20px;margin:5px;border:none;border-radius:5px;cursor:pointer;font-size:16px}";
  html += ".btn-primary{background:#2196F3;color:white}.btn-success{background:#4CAF50;color:white}";
  html += ".btn-danger{background:#f44336;color:white}.btn-warning{background:#ff9800;color:white}";
  html += "</style></head><body>";
  
  html += "<div class='container'>";
  html += "<h1>Minuterie Chaudiere</h1>";
  
  // Connection status
  html += "<div class='info'>";
  html += "WiFi: ";
  if (WiFi.status() == WL_CONNECTED) {
    html += "Connecte (" + WiFi.localIP().toString() + ")";
  } else {
    html += "Mode AP actif";
  }
  html += " | GPS: ";
  if (!gpsDetectionComplete) {
    html += "Detection en cours...";
  } else if (gpsProtocol == GPS_NMEA) {
    html += "NMEA " + String(gpsBaudRate) + " baud ";
    html += (isGPSTimeValid() ? "(Valide)" : "(Timeout)");
  } else if (gpsProtocol == GPS_UBX) {
    html += "UBX->NMEA " + String(gpsBaudRate) + " baud ";
    html += (isGPSTimeValid() ? "(Valide)" : "(Timeout)");
  } else {
    html += "Non detecte";
  }
  html += "</div>";
  
  // Current status
  String currentTime = getCurrentTimeString();
  html += "<div class='status ";
  html += (relayState ? "on" : "off");
  html += "'>";
  html += "Chaudiere: ";
  html += (relayState ? "ALLUMEE" : "ETEINTE");
  if (manualMode) {
    html += " (MANUEL)";
  }
  html += "<br>Heure: ";
  html += currentTime;
  html += "</div>";
  
  // Manual controls
  html += "<h3>Controle Manuel</h3>";
  html += "<button class='btn-success' onclick=\"location.href='/manual?state=on'\">Allumer</button>";
  html += "<button class='btn-danger' onclick=\"location.href='/manual?state=off'\">Eteindre</button>";
  html += "<button class='btn-primary' onclick=\"location.href='/manual?state=auto'\">Mode Auto</button>";
  
  // Configuration
  html += "<h3>Configuration</h3>";
  html += "<button class='btn-primary' onclick=\"location.href='/settings'\">Plages Horaires</button>";
  html += "<button class='btn-primary' onclick=\"location.href='/wifi'\">WiFi</button>";
  if (gpsProtocol == GPS_UNKNOWN || !isGPSTimeValid()) {
    html += "<button class='btn-warning' onclick=\"location.href='/resetgps'\">Relancer GPS</button>";
  }
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSettings() {
  String html = "<!DOCTYPE html><html><head><title>Configuration</title>";
  html += "<meta charset='UTF-8'>";
  html += "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}";
  html += ".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:10px}";
  html += "table{width:100%;border-collapse:collapse}th,td{padding:8px;border:1px solid #ddd;text-align:left}";
  html += "input,select{padding:5px;width:60px}button{padding:8px 15px;margin:5px;border:none;border-radius:5px;cursor:pointer}";
  html += ".btn-primary{background:#2196F3;color:white}.btn-success{background:#4CAF50;color:white}";
  html += "</style></head><body>";
  
  html += "<div class='container'>";
  html += "<h1>Configuration des Plages Horaires</h1>";
  
  html += "<form action='/save' method='post'>";
  
  // Time settings
  html += "<h3>Parametres Temporels</h3>";
  html += "<table>";
  html += "<tr><td>Fuseau Horaire (UTC+):</td><td><input type='number' name='timezone' value='";
  html += String(timeZoneOffset);
  html += "' min='-12' max='12'></td></tr>";
  html += "<tr><td>Heure d'ete automatique:</td><td><select name='autodst'><option value='1'";
  if (autoDST) html += " selected";
  html += ">Oui</option><option value='0'";
  if (!autoDST) html += " selected";
  html += ">Non</option></select></td></tr>";
  html += "<tr><td>GPS prioritaire:</td><td><select name='usegps'><option value='1'";
  if (useGPS) html += " selected";
  html += ">Oui</option><option value='0'";
  if (!useGPS) html += " selected";
  html += ">Non</option></select></td></tr>";
  html += "<tr><td>Serveur NTP:</td><td><input type='text' name='ntpserver' value='";
  html += ntpServer;
  html += "' style='width:200px'></td></tr>";
  html += "</table>";
  
  // GPS Status
  html += "<h3>Statut GPS</h3>";
  html += "<table>";
  html += "<tr><td>Protocole:</td><td>";
  if (gpsProtocol == GPS_NMEA) html += "NMEA";
  else if (gpsProtocol == GPS_UBX) html += "UBX->NMEA";
  else html += "Non detecte";
  html += "</td></tr>";
  html += "<tr><td>Baudrate:</td><td>" + String(gpsBaudRate) + "</td></tr>";
  html += "<tr><td>Status:</td><td>";
  if (!gpsDetectionComplete) html += "Detection en cours";
  else if (isGPSTimeValid()) html += "Valide";
  else html += "Timeout/Invalide";
  html += "</td></tr>";
  html += "</table>";
  
  // Time slots
  html += "<h3>Plages Horaires</h3>";
  html += "<table>";
  html += "<tr><th>Actif</th><th>Debut</th><th>Fin</th></tr>";
  
  for (int i = 0; i < MAX_TIMESLOTS; i++) {
    html += "<tr>";
    html += "<td><input type='checkbox' name='slot";
    html += String(i);
    html += "_enabled' value='1'";
    if (timeSlots[i].enabled) html += " checked";
    html += "></td>";
    html += "<td><input type='time' name='slot";
    html += String(i);
    html += "_start' value='";
    if (timeSlots[i].startHour < 10) html += "0";
    html += String(timeSlots[i].startHour);
    html += ":";
    if (timeSlots[i].startMinute < 10) html += "0";
    html += String(timeSlots[i].startMinute);
    html += "'></td>";
    html += "<td><input type='time' name='slot";
    html += String(i);
    html += "_end' value='";
    if (timeSlots[i].endHour < 10) html += "0";
    html += String(timeSlots[i].endHour);
    html += ":";
    if (timeSlots[i].endMinute < 10) html += "0";
    html += String(timeSlots[i].endMinute);
    html += "'></td>";
    html += "</tr>";
  }
  
  html += "</table>";
  
  html += "<br><button type='submit' class='btn-success'>Sauvegarder</button>";
  html += "<button type='button' class='btn-primary' onclick=\"location.href='/'\">Retour</button>";
  html += "</form>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("timezone")) {
    timeZoneOffset = server.arg("timezone").toInt();
  }
  if (server.hasArg("autodst")) {
    autoDST = server.arg("autodst").toInt();
  }
  if (server.hasArg("usegps")) {
    useGPS = server.arg("usegps").toInt();
  }
  if (server.hasArg("ntpserver")) {
    ntpServer = server.arg("ntpserver");
  }
  
  for (int i = 0; i < MAX_TIMESLOTS; i++) {
    String prefix = "slot";
    prefix += String(i);
    prefix += "_";
    
    timeSlots[i].enabled = server.hasArg(prefix + "enabled");
    
    if (server.hasArg(prefix + "start")) {
      String startTime = server.arg(prefix + "start");
      int colonIndex = startTime.indexOf(':');
      if (colonIndex > 0) {
        timeSlots[i].startHour = startTime.substring(0, colonIndex).toInt();
        timeSlots[i].startMinute = startTime.substring(colonIndex + 1).toInt();
      }
    }
    
    if (server.hasArg(prefix + "end")) {
      String endTime = server.arg(prefix + "end");
      int colonIndex = endTime.indexOf(':');
      if (colonIndex > 0) {
        timeSlots[i].endHour = endTime.substring(0, colonIndex).toInt();
        timeSlots[i].endMinute = endTime.substring(colonIndex + 1).toInt();
      }
    }
  }
  
  saveSettings();
  timeClient.setTimeOffset(timeZoneOffset * 3600);
  timeClient.setPoolServerName(ntpServer.c_str());
  
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleWiFi() {
  String html = "<!DOCTYPE html><html><head><title>Configuration WiFi</title>";
  html += "<meta charset='UTF-8'>";
  html += "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}";
  html += ".container{max-width:600px;margin:0 auto;background:white;padding:20px;border-radius:10px}";
  html += "input{padding:8px;margin:5px;width:200px}button{padding:10px 20px;margin:5px;border:none;border-radius:5px;cursor:pointer}";
  html += ".btn-primary{background:#2196F3;color:white}.btn-success{background:#4CAF50;color:white}";
  html += "</style></head><body>";
  
  html += "<div class='container'>";
  html += "<h1>Configuration WiFi</h1>";
  
  html += "<p><strong>Statut actuel:</strong> ";
  if (WiFi.status() == WL_CONNECTED) {
    html += "Connecte a " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")";
  } else {
    html += "Mode Point d'acces actif";
  }
  html += "</p>";
  
  html += "<form action='/savewifi' method='post'>";
  html += "<p>SSID: <br><input type='text' name='ssid' placeholder='Nom du reseau WiFi'></p>";
  html += "<p>Mot de passe: <br><input type='password' name='password' placeholder='Mot de passe WiFi'></p>";
  html += "<button type='submit' class='btn-success'>Sauvegarder</button>";
  html += "<button type='button' class='btn-primary' onclick=\"location.href='/'\">Retour</button>";
  html += "</form>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSaveWiFi() {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    writeStringToEEPROM(SSID_ADDR, server.arg("ssid"), 32);
    writeStringToEEPROM(PASS_ADDR, server.arg("password"), 32);
    EEPROM.commit();
    
    server.send(200, "text/html", 
      "<!DOCTYPE html><html><head><title>WiFi Sauvegarde</title></head><body>"
      "<h1>Configuration WiFi sauvegardee</h1>"
      "<p>Le module va redemarrer pour appliquer les nouveaux parametres.</p>"
      "</body></html>");
    
    delay(2000);
    ESP.restart();
  } else {
    server.sendHeader("Location", "/wifi");
    server.send(302, "text/plain", "");
  }
}

void handleManual() {
  if (server.hasArg("state")) {
    String state = server.arg("state");
    if (state == "on") {
      manualMode = true;
      relayState = true;
      digitalWrite(RELAY_PORT, LOW);
      digitalWrite(LED_BUILTIN, LOW);
    } else if (state == "off") {
      manualMode = true;
      relayState = false;
      digitalWrite(RELAY_PORT, HIGH);
      digitalWrite(LED_BUILTIN, HIGH);
    } else if (state == "auto") {
      manualMode = false;
    }
  }
  
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleStatus() {
  String json = "{";
  json += "\"relayState\":" + String(relayState ? "true" : "false");
  json += ",\"currentTime\":\"" + getCurrentTimeString() + "\"";
  json += ",\"useGPS\":" + String(useGPS ? "true" : "false");
  json += ",\"gpsValid\":" + String(isGPSTimeValid() ? "true" : "false");
  json += ",\"gpsProtocol\":\"";
  if (gpsProtocol == GPS_NMEA) json += "NMEA";
  else if (gpsProtocol == GPS_UBX) json += "UBX";
  else json += "UNKNOWN";
  json += "\",\"gpsBaudRate\":" + String(gpsBaudRate);
  json += ",\"gpsDetectionComplete\":" + String(gpsDetectionComplete ? "true" : "false");
  json += ",\"manualMode\":" + String(manualMode ? "true" : "false");
  json += ",\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  if (WiFi.status() == WL_CONNECTED) {
    json += ",\"wifiSSID\":\"" + WiFi.SSID() + "\"";
  }
  json += "}";
  
  server.send(200, "application/json", json);
}

String getCurrentTimeString() {
  if (useGPS && isGPSTimeValid()) {
    return gpsTime;
  } else if (WiFi.status() == WL_CONNECTED) {
    time_t now = timeClient.getEpochTime();
    if (autoDST && isDST()) {
      now += 3600;
    }
    
    int h = hour(now);
    int m = minute(now);
    int s = second(now);
    
    String timeStr = "";
    if (h < 10) timeStr += "0";
    timeStr += String(h) + ":";
    if (m < 10) timeStr += "0";
    timeStr += String(m) + ":";
    if (s < 10) timeStr += "0";
    timeStr += String(s);
    
    return timeStr;
  } else {
    return "No time source";
  }
}
