#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <ESPmDNS.h>

const char* apiEndpoint = "https://api.coincap.io/v2/assets";

// Default Wi-Fi credentials
const String defaultSSID = "SSID";
const String defaultPassword = "Password";

TFT_eSPI tft = TFT_eSPI();  // Invoke custom library

bool dimmed = false;
int brightness[] = {40, 80, 120, 160, 200};
int brightnessIndex = 1;

const int pwmLedChannelTFT = 0;  
const int buttonPin = 35; // GPIO for dimming button

unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

int lastButtonState = HIGH;

// Previous prices for comparison
float prevBtcPrice = -1;
float prevEthPrice = -1;
float prevSolPrice = -1;

// Web server
WebServer server(80);

// Function to write a string to EEPROM
void writeStringToEEPROM(int address, String value) {
  int length = value.length();
  EEPROM.write(address, length); // First byte stores length
  for (int i = 0; i < length; i++) {
    EEPROM.write(address + 1 + i, value[i]);
  }
  EEPROM.commit();
}

// Function to read a string from EEPROM
String readStringFromEEPROM(int address) {
  int length = EEPROM.read(address);
  String value = "";
  for (int i = 0; i < length; i++) {
    value += char(EEPROM.read(address + 1 + i));
  }
  return value;
}

// Function to set default credentials if EEPROM is empty
void setDefaultCredentials() {
  String ssid = readStringFromEEPROM(0);
  String password = readStringFromEEPROM(100);

  if (ssid == "" || password == "") {
    // No credentials in EEPROM, set defaults
    writeStringToEEPROM(0, defaultSSID);
    writeStringToEEPROM(100, defaultPassword);
  }
}

void handleRoot() {
  String page = "<html><body><h1>Wi-Fi Configuration</h1>";
  page += "<form action=\"/save\" method=\"post\">";
  page += "SSID: <input type=\"text\" name=\"ssid\"><br>";
  page += "Password: <input type=\"password\" name=\"password\"><br>";
  page += "<input type=\"submit\" value=\"Save\">";
  page += "</form></body></html>";
  server.send(200, "text/html", page);
}

void handleSave() {
  if (server.method() == HTTP_POST) {
    String ssid = server.arg("ssid");
    String password = server.arg("password");

    if (ssid.length() > 0 && password.length() > 0) {
      // Save the new credentials to EEPROM
      writeStringToEEPROM(0, ssid);
      writeStringToEEPROM(100, password);

      server.send(200, "text/html", "Credentials saved. Please reboot the ESP32.");
      delay(2000);
      ESP.restart();  // Reboot to apply new credentials
    } else {
      server.send(400, "text/html", "Invalid credentials.");
    }
  }
}

void setup() {
  // Initialize Serial for debugging
  Serial.begin(115200);

  // Initialize EEPROM
  EEPROM.begin(512);

  // Set default Wi-Fi credentials if not already set
  setDefaultCredentials();

  // Initialize TFT
  tft.init();
  tft.setRotation(1);  // Set rotation if needed
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);  // Init text size

  // Initialize PWM for TFT brightness control
  ledcSetup(pwmLedChannelTFT, 5000, 8);
  ledcAttachPin(15, pwmLedChannelTFT);  // Assuming GPIO 15 controls backlight
  ledcWrite(pwmLedChannelTFT, brightness[brightnessIndex]);

  // Initialize button
  pinMode(buttonPin, INPUT_PULLUP);

  // Attempt to connect to default Wi-Fi
  String ssid = readStringFromEEPROM(0);
  String password = readStringFromEEPROM(100);
  WiFi.begin(ssid.c_str(), password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    // If unable to connect, start Access Point for configuration
    Serial.println("\nFailed to connect to Wi-Fi. Starting configuration mode...");
    WiFi.softAP("WLAN-ESP32");
    if (!MDNS.begin("wifi")) {
      Serial.println("Error starting mDNS");
    } else {
      Serial.println("mDNS started");
    }
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);

    server.begin();
    Serial.println("Server started. Connect to the access point 'WLAN-ESP32' to configure Wi-Fi.");
    Serial.println("Use 'http://wifi.local' to access the configuration page.");

    // Display initial message
    tft.drawString("Configuring Wi-Fi...", 10, 10);
    tft.drawString("Connect to WLAN-ESP32", 10, 20);
    tft.drawString("Open 'http://wifi.local'", 10, 30);
    
  } else {
    // Connected successfully to Wi-Fi
    Serial.println("Connected to Wi-Fi!");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    
    tft.drawString("Connected to Wi-Fi", 10, 10);
    tft.drawString("Fetching data...", 10, 30);
    tft.setTextSize(3); // Increased text size
  }
}

void loop() {
  server.handleClient();

  int reading = digitalRead(buttonPin);

  // Check button with debouncing
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading == LOW) {
      brightnessIndex = (brightnessIndex + 1) % 5;
      ledcWrite(pwmLedChannelTFT, brightness[brightnessIndex]);
    }
  }
  lastButtonState = reading;

  // Fetch and display prices if connected to Wi-Fi
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(apiEndpoint);
    int httpCode = http.GET();

    if (httpCode > 0) {  // Check for the returning code
      String payload = http.getString();
      Serial.println(payload);

      DynamicJsonDocument doc(4096);
      deserializeJson(doc, payload);

      // Extract prices
      float btcPrice = -1, ethPrice = -1, solPrice = -1;
      for (JsonObject asset : doc["data"].as<JsonArray>()) {
        String id = asset["id"].as<String>();
        if (id == "bitcoin") {
          btcPrice = asset["priceUsd"].as<float>();
        } else if (id == "ethereum") {
          ethPrice = asset["priceUsd"].as<float>();
        } else if (id == "solana") {
          solPrice = asset["priceUsd"].as<float>();
        }
      }

      // Clear the screen
      tft.fillScreen(TFT_BLACK);

      // Calculate Y positions for centered text
      int screenHeight = tft.height();
      int numCoins = 3;
      int spacing = screenHeight / (numCoins + 1);

      // Horizontal positions
      int nameX = 10;
      int priceX = tft.width() / 2 - 25; // Adjusted X position for better spacing

      // Display BTC price
      int yPosition = spacing;
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("BTC:", nameX, yPosition);
      if (btcPrice > prevBtcPrice) {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
      } else if (btcPrice < prevBtcPrice) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
      } else {
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
      }
      tft.drawFloat(btcPrice, 2, priceX, yPosition);

      // Display ETH price
      yPosition += spacing;
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("ETH:", nameX, yPosition);
      if (ethPrice > prevEthPrice) {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
      } else if (ethPrice < prevEthPrice) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
      } else {
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
      }
      tft.drawFloat(ethPrice, 2, priceX, yPosition);

      // Display SOL price
      yPosition += spacing;
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("SOL:", nameX, yPosition);
      if (solPrice > prevSolPrice) {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
      } else if (solPrice < prevSolPrice) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
      } else {
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
      }
      tft.drawFloat(solPrice, 2, priceX, yPosition);

      // Update previous prices
      prevBtcPrice = btcPrice;
      prevEthPrice = ethPrice;
      prevSolPrice = solPrice;

    } else {
      Serial.println("Error on HTTP request");
      tft.drawString("Error fetching data", 10, 50);
    }

    http.end();
  }

  // Wait for a minute before next update
  delay(15000);
}
