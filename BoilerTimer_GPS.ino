#include <ESP8266WiFi.h>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>

const char* ssid = "your_SSID";
const char* password = "your_PASSWORD";

TinyGPSPlus gps;
SoftwareSerial ss(4, 5); // RX, TX for GPS

void setup() {
    Serial.begin(9600);
    ss.begin(9600);
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi");
}

void loop() {
    while (ss.available() > 0) {
        gps.encode(ss.read());
        if (gps.location.isUpdated()) {
            handleGPSData();
        }
    }
    // Add boiler control logic based on GPS location and scheduling
}

void handleGPSData() {
    Serial.print("Latitude: ");
    Serial.print(gps.location.lat(), 6);
    Serial.print(" Longitude: ");
    Serial.println(gps.location.lng(), 6);
    
    // Implement scheduling based on GPS location
}