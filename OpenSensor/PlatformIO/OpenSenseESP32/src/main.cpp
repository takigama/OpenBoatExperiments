#ifndef USE_SCANNER
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <QMC5883LCompass.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <WebServer.h>
#include <Preferences.h>
#include "WebPageHandler.h"

/* WIFI Config/Web interface*/
WiFiClient espClient;
Preferences prefs;
WebServer *server = nullptr;
WebPageHandler *pageHandler = nullptr;

#define AP_NAME "openboatsense"
#define AP_PASS "12345678"
#define HOLD_TIME 10000 // 10 seconds hold to trigger
#define BUTTON_PIN 10

const unsigned long CONFIG_PORTAL_TIMEOUT = 120000; // 2 minutes timeout
volatile bool buttonPressed = false;
static bool messageShown = false;
unsigned long buttonPressStart = 0;

String name, password;
bool configRunning = false;

// Interrupt Service Routine (ISR)
void IRAM_ATTR handleButtonInterrupt()
{
  if (!buttonPressed)
  {
    buttonPressed = true;
    buttonPressStart = millis();
  }
}

void runConfigPortal()
{
  configRunning = true;
  // Reset WiFi modes
  WiFi.disconnect(true);
  delay(100);
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);

  // Start Access Point
  WiFi.softAP(AP_NAME, AP_PASS);
  Serial.println("AP Mode Started SSID: " + String(AP_NAME));
  Serial.println("AP IP Address: " + WiFi.softAPIP().toString());

  if (server)
    delete server;
  if (pageHandler)
    delete pageHandler;

  server = new WebServer(80);
  pageHandler = new WebPageHandler(*server);
  pageHandler->begin();

  unsigned long startTime = millis();
  while (millis() - startTime < CONFIG_PORTAL_TIMEOUT)
  {
    server->handleClient();
    delay(1);

    if (pageHandler->isConfigDone())
    {
      Serial.println("Configuration completed. Exiting portal.");
      break;
    }
  }

  server->stop();
  WiFi.disconnect(true);
  delay(100);
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  Serial.println("Portal closed.");

  // Read back the new data to verify
  prefs.begin("device_prefs", true);
  name = prefs.getString("device_name", "N/A");
  password = prefs.getString("password", "N/A");
  prefs.end();

  Serial.println("NEW DATA SAVED:");
  Serial.println("Device Name: " + name);
  Serial.println("Password: " + password);
  Serial.println("---------------------------");

  configRunning = false;
}

void CONFIG_setup()
{
  pinMode(BUTTON_PIN, INPUT);
  // Attach interrupt to the button pin
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonInterrupt, FALLING);

  // Load existing data on startup
  prefs.begin("device_prefs", true);
  name = prefs.getString("device_name", "N/A");
  password = prefs.getString("password", "N/A");

  Serial.println("From Preferences:");
  Serial.println("Device Name: " + name);
  Serial.println("Password: " + password);
  Serial.println("---------------------------");
  delay(2000);

  // If no data exists, force config mode
  if (name == "N/A" || password == "N/A")
  {
    Serial.println("No valid config found. Launching config portal...");
    runConfigPortal();
  }
}

void CONFIG_loop()
{
  if (buttonPressed)
  {
    Serial.println("Button Pressed");
    if (digitalRead(BUTTON_PIN) == LOW)
    {
      if (!messageShown)
      {
        Serial.println("Button pressed. Hold to confirm...");
        messageShown = true;
      }

      if (millis() - buttonPressStart >= HOLD_TIME)
      {
        Serial.println("Button held for 10s — starting config portal...");
        buttonPressed = false;
        messageShown = false;
        runConfigPortal();
      }
    }
    else
    {
      // Button released early
      buttonPressed = false;
      messageShown = false;
      Serial.println("Button Released");
    }
  }
}

/* End Wifi Config Interface*/

/* u8g setup*/

TwoWire Wire1 = TwoWire(1);

#define SCREEN_ADDRESS 0x3C
#define OLED_RESET -1 // Reset pin
#define OLED_SDA 5
#define OLED_SCL 6
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define I2C_FREQ 400000

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, OLED_RESET);

void testdrawcircle(void)
{
  display.clearDisplay();
  Serial.println("SSD1306 setup");

  for (int16_t i = 0; i < max(display.width(), display.height()) / 2; i += 2)
  {
    display.drawCircle(display.width() / 2, display.height() / 2, i, SSD1306_WHITE);
    display.display();
    delay(1);
  }
}

void I2C_ScannerWire1()
{
  byte error, address;
  int nDevices;

  Serial.println("Scanning...");

  nDevices = 0;
  for (address = 1; address < 127; address++)
  {
    Wire1.beginTransmission(address);
    error = Wire1.endTransmission();

    if (error == 0)
    {
      Serial.print("I2C device found at address 0x");
      if (address < 16)
        Serial.print("0");
      Serial.print(address, HEX);
      Serial.println("  !");

      nDevices++;
    }
    else if (error == 4)
    {
      Serial.print("Unknown error at address 0x");
      if (address < 16)
        Serial.print("0");
      Serial.println(address, HEX);
    }
  }
  if (nDevices == 0)
    Serial.println("No I2C devices found\n");
  else
    Serial.println("done\n");
}

void SSD1306_setup()
{
  Wire1.begin(OLED_SDA, OLED_SCL);
  I2C_ScannerWire1();

  // if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
  // {
  //   Serial.println(F("SSD1306 allocation failed"));
  // }
  // else
  // {
  //   display.clearDisplay();
  //   display.display();
  //   testdrawcircle();
  // }
}

/* end u8g */

/*
WIFI
*/
void WIFI_setup()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(name, password);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(name);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

/*
End WiFi
*/

/*
QMC5883 Code
*/
QMC5883LCompass compass;

void QMC5883_setup(void)
{
  /* If an MPU6050 exists, turn off master mode */
  Wire.beginTransmission(0x68); // MPU6050 address
  Wire.write(0x6A);             // USER_CTRL
  Wire.write(0x00);             // Disable master mode
  Wire.endTransmission();
  Wire.beginTransmission(0x68);
  Wire.write(0x37); // INT_PIN_CFG
  Wire.write(0x02); // Enable bypass
  Wire.endTransmission();

  compass.init();
}

void QMC5883_loop(void)
{
  int x, y, z;

  // Read compass values
  compass.read();

  // Return XYZ readings
  x = compass.getX();
  y = compass.getY();
  z = compass.getZ();

  Serial.print("MAG:");
  Serial.print(x);
  Serial.print(",");
  Serial.print(y);
  Serial.print(",");
  Serial.print(z);
  Serial.println();
}

/*
END of QMC5883 Code
*/

/*
BMP 180 Code
*/

Adafruit_BMP085 bmp;
bool bmpsetup = false;

void BMP085_setup(void)
{
  if (!bmp.begin())
  {
    Serial.println("Could not find a valid BMP085 sensor, check wiring!");
  }
  else
  {
    bmpsetup = true;
  }
}

void BMP085_loop(void)
{
  if (bmpsetup)
  {
    Serial.print("BMP:T:");
    Serial.print(bmp.readTemperature());
    Serial.print(",");

    Serial.print("P:");
    Serial.print(bmp.readPressure());
    Serial.print(",");

    Serial.print("A:");
    Serial.print(bmp.readAltitude());
    Serial.print(",");

    Serial.print("PS:");
    Serial.print(bmp.readSealevelPressure());
    Serial.print(",");

    Serial.print("AR:");
    Serial.print(bmp.readAltitude(101500));

    Serial.println();
  }
}

/* END of bmp180 code*/

/* MPU6050 Code*/
Adafruit_MPU6050 mpu;
bool MPU6050init = false;

void MPU6050_setup(void)
{
  if (!mpu.begin())
  {
    Serial.println("Failed to find MPU6050 chip");
  }
  else
  {
    MPU6050init = true;

    Serial.println("MPU6050 Found!");

    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    Serial.print("Accelerometer range set to: ");
    switch (mpu.getAccelerometerRange())
    {
    case MPU6050_RANGE_2_G:
      Serial.println("+-2G");
      break;
    case MPU6050_RANGE_4_G:
      Serial.println("+-4G");
      break;
    case MPU6050_RANGE_8_G:
      Serial.println("+-8G");
      break;
    case MPU6050_RANGE_16_G:
      Serial.println("+-16G");
      break;
    }
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    Serial.print("Gyro range set to: ");
    switch (mpu.getGyroRange())
    {
    case MPU6050_RANGE_250_DEG:
      Serial.println("+- 250 deg/s");
      break;
    case MPU6050_RANGE_500_DEG:
      Serial.println("+- 500 deg/s");
      break;
    case MPU6050_RANGE_1000_DEG:
      Serial.println("+- 1000 deg/s");
      break;
    case MPU6050_RANGE_2000_DEG:
      Serial.println("+- 2000 deg/s");
      break;
    }

    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.print("Filter bandwidth set to: ");
    switch (mpu.getFilterBandwidth())
    {
    case MPU6050_BAND_260_HZ:
      Serial.println("260 Hz");
      break;
    case MPU6050_BAND_184_HZ:
      Serial.println("184 Hz");
      break;
    case MPU6050_BAND_94_HZ:
      Serial.println("94 Hz");
      break;
    case MPU6050_BAND_44_HZ:
      Serial.println("44 Hz");
      break;
    case MPU6050_BAND_21_HZ:
      Serial.println("21 Hz");
      break;
    case MPU6050_BAND_10_HZ:
      Serial.println("10 Hz");
      break;
    case MPU6050_BAND_5_HZ:
      Serial.println("5 Hz");
      break;
    }

    Serial.println("");
    delay(100);
  }

  /* work around to get the MPU to turn off master mode because hmc wont init otherwise*/
  Wire.beginTransmission(0x68); // MPU6050 address
  Wire.write(0x6A);             // USER_CTRL
  Wire.write(0x00);             // Disable master mode
  Wire.endTransmission();
  Wire.beginTransmission(0x68);
  Wire.write(0x37); // INT_PIN_CFG
  Wire.write(0x02); // Enable bypass
  Wire.endTransmission();
}

void MPU6050_loop()
{

  /* Get new sensor events with the readings */
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  /* Print out the values */
  Serial.print("MPU:AX:");
  Serial.print(a.acceleration.x);
  Serial.print(",AY:");
  Serial.print(a.acceleration.y);
  Serial.print(",AZ:");
  Serial.print(a.acceleration.z);
  Serial.print(",");

  Serial.print("RX:");
  Serial.print(g.gyro.x);
  Serial.print(",RY:");
  Serial.print(g.gyro.y);
  Serial.print(",RZ:");
  Serial.print(g.gyro.z);
  Serial.print(",");

  Serial.print("T:");
  Serial.print(temp.temperature);

  Serial.println("");
}

/* End of MPU6050 Code*/

/* Start of GPS Code */
#define MSG_BUFFER_SIZE (256)
char msg[MSG_BUFFER_SIZE];

String receivedMessage = "";

void GPS_setup(void)
{
  // lets set the serial speed up to 115200, we do this as multiple rates, just to make sure
  Serial.println("Begin GPS init");
  Serial1.begin(9600, SERIAL_8N1, 20, 21);
  Serial1.print("\r\n");
  Serial1.print("\r\n");
  Serial1.print("$PQBAUD,W,115200*43\r\n");
  Serial1.print("\r\n");
  Serial1.print("\r\n");

  Serial1.begin(38400, SERIAL_8N1, 20, 21);
  Serial1.print("\r\n");
  Serial1.print("\r\n");
  Serial1.print("$PQBAUD,W,115200*43\r\n");
  Serial1.print("\r\n");
  Serial1.print("\r\n");

  Serial1.begin(115200, SERIAL_8N1, 20, 21);

  // set output of all sentences
  Serial1.print("\r\n");
  Serial1.print("\r\n");
  Serial1.print("$PMTK314,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,1,0*2D\r\n");
  Serial1.print("\r\n");
  Serial1.print("\r\n");
  Serial.println("End GPS init");
}

void GPS_loop(void)
{
  if (Serial1.available())
  {
    while (Serial1.available())
    {
      char incomingChar = Serial1.read(); // Read each character from the buffer

      if (incomingChar == '\n')
      { // Check if the user pressed Enter (new line character)

        // Clear the message buffer for the next input
        snprintf(msg, MSG_BUFFER_SIZE, "%s", receivedMessage.c_str());
        // client.publish("outTopic", msg);
        Serial.printf("GPS:%s\n", msg);
        receivedMessage = "";
      }
      else
      {
        // Append the character to the message string
        receivedMessage += incomingChar;
      }
    }
  }
}
/* End of GPS Code*/

void setup(void)
{
  Serial.begin(115200);
  delay(6000);
  Wire.begin(8, 9);

  CONFIG_setup();

  WIFI_setup();

  // SSD1306_setup();

  GPS_setup();
  BMP085_setup();
  MPU6050_setup();
  QMC5883_setup();
  Serial.println("Setup finished");
}

unsigned long tm, lm;

void loop(void)
{

  CONFIG_loop();

  GPS_loop();
  QMC5883_loop();
  BMP085_loop();
  MPU6050_loop();

  // lets get an idea about loop delay, in testing with all sensors, this is taking about 138ms per cycle
  tm = micros();
  Serial.printf("TMS:%lu, %lu\n", tm - lm, tm);
  lm = tm;
}
#endif