#include <Arduino_MKRIoTCarrier.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFiNINA.h>
#include "arduino_secrets.h"
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define READING_INTERVAL 30000 // ms
#define WIFI_INTERVAL 30000 // ms
#define SAMPLES 6
#define ALARM_THRESHOLD 5  // Number of consecutive error readings

DeviceAddress beerThermometer = { 0x28, 0x61, 0x65, 0x24, 0x05, 0x00, 0x00, 0x62 };
DeviceAddress airThermometer = { 0x28, 0x11, 0x0B, 0x64, 0x03, 0x00, 0x00, 0x78 };

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASSWD;
// Enter your mosquitto test server hostname or IP address:
const char broker[] = SECRET_BROKER;
int port = 1883;

struct brew {
  const char *name;
  int mintemp;
  int maxtemp;
};

struct brew ferment = {"Ferment", 180, 210};
struct brew finish = {"Finish", 100, 150};
struct brew test = {"Test", 220, 240};
struct brew *brewPtr = &ferment;  // Pointer to a brew

MKRIoTCarrier carrier;
unsigned int threshold = 4;
unsigned int threshold_btn_0 = 3;
unsigned int threshold_btn_1 = 5;
unsigned int threshold_btn_2 = 5;
unsigned int threshold_btn_3 = 5;
unsigned int threshold_btn_4 = 5;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

const bool on = true;
const bool off = false;
bool heater_control = off;
bool cooler_control = off;
String heater_state_desc = "Heater: OFF";
String cooler_state_desc = "Cooler: OFF";
int displayScreen = 0;

uint32_t greenColor = carrier.leds.Color( 255, 0, 0);
uint32_t redColor = carrier.leds.Color( 0, 255, 0);
uint32_t blueColor = carrier.leds.Color( 0, 0, 255);
uint32_t noColor = carrier.leds.Color( 0, 0, 0);

// Thermometer error codes:
#define SUCCESS 0
#define BEER_THERMOMETER_FAILUE 1
#define AIR_THERMOMETER_FAILURE 2

// Data wire is plugged into port 21
#define ONE_WIRE_BUS 21
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

struct thermometers {
  int beerTemperature = -127;
  int airTemperature = -127;
};

struct thermometers myThermometers;

// Store temperatures for statistics
int beerTemperatures[SAMPLES];
int airTemperatures[SAMPLES];

unsigned int state = 0;  // Menu mode
unsigned int buttonState = 0;  // Default state
unsigned int previousButtonState = 1;
unsigned long previousMillis = 0;
unsigned long prevConnectMillis = 0;
float previousBeerTemp = -127.0;

long lastReconnectAttempt = 0;

StaticJsonDocument<128> doc;
char buffer[128];

void setup() {
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect
  }
  Serial.println("Beer brewer test!");
  CARRIER_CASE = true;

  carrier.begin();
  carrier.display.setRotation(0);
  carrier.leds.setBrightness(2);
  delay(5000);

  sensors.begin();
  delay(5000);

  mqttClient.setServer(SECRET_BROKER, 1883);

  WiFi.begin(SECRET_SSID, SECRET_PASSWD);
  delay(1500);
  Serial.println(WiFi.firmwareVersion());
  IPAddress ip = WiFi.localIP();
  Serial.println(ip);
  lastReconnectAttempt = 0;
}  // End setup

void loop() {
  static unsigned int touchCounter = 0;
  unsigned long currentMillis = millis();

  // Update MQTT without blocking
  if (!mqttClient.connected()) {
    if (currentMillis - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = currentMillis;
      // Attempt to reconnect - times out after 30 sec
      if (reconnect()) {
        lastReconnectAttempt = 0;
      }
    }
  }
  else {
    // Client connected
    mqttClient.loop();
  }

  switch (state) {
    case 0: // Menu mode
      if (buttonState != previousButtonState) {
        previousButtonState = buttonState;
        menuScreen(buttonState);
      }

      carrier.Buttons.update(); // Check touch pads
      if (carrier.Button0.getTouch()) {
        touchCounter = 0;
        buttonState = 0;
        brewPtr = &ferment;
        Serial.println("Touched Down Button 0");
      }
      if (carrier.Button1.getTouch()) {
        touchCounter = 0;
        buttonState = 1;
        brewPtr = &finish;
        Serial.println("Touched Down Button 1");
      }
      if (carrier.Button2.getTouch()) {
        touchCounter = 0;
        buttonState = 2;
        brewPtr = &test;
        Serial.println("Touched Down Button 2");
      }
      if (carrier.Button4.getTouch()) {
        buttonState = 4;
        touchCounter++;
        if (touchCounter > 200) {
          startScreen();
          Serial.println("Lock...");
          Serial.println(brewPtr->name);
          delay(1000);
          state = 1;
        }
        Serial.println("Touched Down Button 4");
      }

      break;
    case 1: // Init temperature arays
      for (int i = 0; i < SAMPLES; i++) {
        // Get latest temperature readings
        if (!updateReadings(ALARM_THRESHOLD)) {
          beerTemperatures[i] = myThermometers.beerTemperature;
          airTemperatures[i] = myThermometers.airTemperature;
        }
        else {
          state = 3;  // Error state
        }
        delay(1000);
      } // End init
      for (int j = 0; j < SAMPLES; j++) {
        Serial.println(beerTemperatures[j]);
        Serial.println(airTemperatures[j]);
      }
      state = 2;
      break;
    case 2:  // Brewing mode
      if (currentMillis - previousMillis >= READING_INTERVAL) {
        previousMillis = currentMillis;

        // Get latest temperature readings
        if (!updateReadings(ALARM_THRESHOLD)) {
          // Right shift samples
          int i = SAMPLES;
          while (--i) {
            beerTemperatures[i] = beerTemperatures[i - 1];
            airTemperatures[i] = airTemperatures[i - 1];
          }
          beerTemperatures[0] = myThermometers.beerTemperature;
          airTemperatures[0] = myThermometers.airTemperature;
          int statBeerTemp = mode(beerTemperatures, SAMPLES);
          int statAirTemp = mode(airTemperatures, SAMPLES);
          Serial.print("Statistical Beer Temperature: ");
          Serial.println(statBeerTemp);
          Serial.print("Statistical Air Temperature: ");
          Serial.println(statAirTemp);
          if (statBeerTemp != previousBeerTemp) {
            previousBeerTemp = statBeerTemp;
            updateBeerTemperature(statBeerTemp, brewPtr);
            Serial.print("Previous Beer Temperature: ");
            Serial.println(previousBeerTemp);
            for (int j = 0; j < SAMPLES; j++) {
              Serial.println(beerTemperatures[j]);
            }
            brewScreen(statBeerTemp);

            doc["sensor"] = "fridge";
            doc["error"] = "NONE";
            doc["beer"] = statBeerTemp;
            doc["air"] = statAirTemp;
            if (heater_control) {
              doc["heater"] = "ON";
            }
            else {
              doc["heater"] = "OFF";
            }
            if (cooler_control) {
              doc["cooler"] = "ON";
            }
            else {
              doc["cooler"] = "OFF";
            }
            size_t n = serializeJson(doc, buffer);
            mqttClient.publish("/beer/data", buffer, n);
            Serial.println();
            serializeJsonPretty(doc, Serial);
          }
        }
        else {
          state = 3;  // Error state
        }
      }  // End millis
      pulseLoop();
      break;
    case 3:  // Error state (heater & cooler off)
      failSafe();
      errorScreen();
      Serial.println("Sound the alarm!");
      mqttClient.publish("/beer/error", "ERROR");
      while (1) {
        carrier.Buzzer.sound(30);
        delay(50);  // Buzz length
      }
  }  // End switch
  delay(10); // Short delay
}

// Statistical mode
int mode(int a[], int n) {
  int maxValue = 0, maxCount = 0, i, j;
  for (i = 0; i < n; ++i) {
    int count = 0;
    for (j = 0; j < n; ++j) {
      if (a[j] == a[i])
        ++count;
    }
    if (count > maxCount) {
      maxCount = count;
      maxValue = a[i];
    }
  }
  return maxValue;
}

boolean reconnect() {
  if (mqttClient.connect("arduinoClient")) {
    // Once connected, publish an announcement...
    mqttClient.publish("/beer", "hello world");
  }
  return mqttClient.connected();
}

void menuScreen(unsigned int menuItem) {
  carrier.display.fillScreen(ST77XX_BLUE); //blue background
  carrier.display.setTextSize(3); //medium sized text
  carrier.display.setTextColor(ST77XX_BLACK);

  if (menuItem == 0) {
    carrier.display.setTextColor(ST77XX_WHITE);
  }
  carrier.display.setCursor(60, 50);
  carrier.display.print(ferment.name);
  carrier.display.setTextColor(ST77XX_BLACK);

  if (menuItem == 1) {
    carrier.display.setTextColor(ST77XX_WHITE);
  }
  carrier.display.setCursor(60, 80);
  carrier.display.print(finish.name);
  carrier.display.setTextColor(ST77XX_BLACK);

  if (menuItem == 2) {
    carrier.display.setTextColor(ST77XX_WHITE);
  }
  carrier.display.setCursor(60, 110);
  carrier.display.print(test.name);
  carrier.display.setTextColor(ST77XX_BLACK);
}

void startScreen() {
  carrier.display.fillScreen(ST77XX_BLUE); //blue background
  carrier.display.setTextSize(4); //medium sized text
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setCursor(20, 100);
  carrier.display.print("STARTING");
}

void errorScreen() {
  carrier.leds.fill(0, 0, 0);
  carrier.leds.show();
  carrier.display.fillScreen(ST77XX_ORANGE); //blue background
  carrier.display.setTextSize(4); //medium sized text
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setCursor(50, 100);
  carrier.display.print("ERROR");
}

void brewScreen(float Temperature) {
  if (displayScreen == 0) {
    carrier.display.fillScreen(ST77XX_BLUE); //blue background
    carrier.leds.fill(blueColor, 0, 5);
  }

  else if (displayScreen == 1) {
    carrier.display.fillScreen(ST77XX_GREEN); //green background
    carrier.leds.fill(greenColor, 0, 5);
  }

  else if (displayScreen == 2) {
    carrier.display.fillScreen(ST77XX_RED); //red background
    carrier.leds.fill(redColor, 0, 5);
  }

  carrier.display.setTextColor(ST77XX_WHITE); //white text
  carrier.display.setTextSize(3); //medium sized text
  carrier.display.setCursor(60, 40);
  carrier.display.print(brewPtr->name);  // Whats brewing!

  carrier.display.setTextColor(ST77XX_WHITE); //white text
  carrier.display.setTextSize(6); //medium sized text
  carrier.display.setCursor(40, 80);
  carrier.display.print(String(Temperature, 1));
  carrier.display.print("C");

  //carrier.display.setTextColor(ST77XX_YELLOW); //yellow text
  carrier.display.setTextColor(ST77XX_BLACK); //yellow text
  carrier.display.setTextSize(2); //medium sized text
  carrier.display.setCursor(50, 150);
  carrier.display.print(heater_state_desc);

  carrier.display.setCursor(50, 170);
  carrier.display.print(cooler_state_desc);
}

void failSafe() {
  heater_control = off;
  cooler_control = off;
  onHeaterControlChange();
  onCoolerControlChange();
}

void updateBeerTemperature(int Temperature, struct brew *brew) {
  if (Temperature < brew->mintemp) {  // Too cold
    cooler_control = off;  // Make sure cooler is off
    heater_control = on;
    displayScreen = 0;
  }
  else if (Temperature >= brew->maxtemp - 15) {  // Too warm
    heater_control = off;  // Make sure heater is off
    cooler_control = on;
    displayScreen = 2;
  }
  else {
    heater_control = off;
    cooler_control = off;
    displayScreen = 1;
  }
  onHeaterControlChange();
  onCoolerControlChange();
}

void onHeaterControlChange() {
  if (heater_control == on) {
    carrier.Relay1.open();
    heater_state_desc = "Heater: ON";
    carrier.Relay2.close();
    cooler_state_desc = "Cooler: OFF";
  } else {
    carrier.Relay1.close();
    heater_state_desc = "Heater: OFF";
  }
  delay(50);
}

void onCoolerControlChange() {
  if (cooler_control == on) {
    carrier.Relay2.open();
    cooler_state_desc = "Cooler: ON";
    carrier.Relay1.close();
    heater_state_desc = "Heater: OFF";
  } else {
    carrier.Relay2.close();
    cooler_state_desc = "Cooler: OFF";
  }
  delay(50);
}

void pulseLoop() {
  static unsigned int i = 0;
  const int max_brightness = 10;
  // Convert mod i degrees to radians
  float b = (i++ % 180) * PI / 180;
  b = sin(b) * max_brightness;
  carrier.leds.setBrightness(b);
  carrier.leds.show();
}

// Safety wrapper round thermometer readings
// Returns 1 if there are multiple reading failures
int updateReadings(unsigned int maxError) {
  static unsigned int errorCounter = 0;
  while (int err = readThermometers(sensors, &myThermometers)) {
    if (err == 1) {
      Serial.println("Error: Could not read Beer Thermometer");
    }
    if (err == 2) {
      Serial.println("Error: Could not read Air Thermometer");
    }
    errorCounter++;
    Serial.println(errorCounter);
    // Error if we get multiple failures
    if (errorCounter > maxError) {
      return 1;
    }
    delay(1000);
  }
  return 0;
}

// Read both thermometers.
// The thermometers struct only updates if both readings were successful.
// Returns zero on success or a positive error code
int readThermometers(DallasTemperature _sensors, struct thermometers *_thermometers) {
  // Issue global temperature request to all devices on the bus
  Serial.print("Requesting temperatures...");
  _sensors.requestTemperatures(); // Send the command to get temperatures
  Serial.println("DONE");
  // After the temperatures are read, we can extract them here using the device address
  float beerTempC = _sensors.getTempC(beerThermometer);

  // Check if reading was successful
  if ((beerTempC != DEVICE_DISCONNECTED_C) && (beerTempC < 85.0)) {  // Device error
    _thermometers->beerTemperature = beerTempC * 10;  // Convert to int
    Serial.print("Beer Temperature: ");
    Serial.println( _thermometers->beerTemperature);
  }
  else return BEER_THERMOMETER_FAILUE;

  float airTempC = _sensors.getTempC(airThermometer);

  if ((airTempC != DEVICE_DISCONNECTED_C) && (airTempC < 85.0)) {
    _thermometers->airTemperature = airTempC * 10;
    Serial.print("Air Temperature: ");
    Serial.println(_thermometers->airTemperature);
  }
  else return AIR_THERMOMETER_FAILURE;

  return SUCCESS;
}
