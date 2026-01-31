#ifdef USE_SCANNER
#include <Arduino.h>
#include <Wire.h>

void setup(void)
{
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, 20, 21);
  delay(10000);
  Wire.begin(8, 9);

  Wire.beginTransmission(0x68); // MPU6050 address
  Wire.write(0x6A);             // USER_CTRL
  Wire.write(0x00);             // Disable master mode
  Wire.endTransmission();
  Wire.beginTransmission(0x68);
  Wire.write(0x37); // INT_PIN_CFG
  Wire.write(0x02); // Enable bypass
  Wire.endTransmission();
}

void loop(void)
{

  byte error, address;
  int nDevices;
  Serial.println("Scanning...");
  nDevices = 0;
  for (address = 1; address < 127; address++)
  {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0)
    {
      Serial.print("I2C device found at address 0x");
      if (address < 16)
      {
        Serial.print("0");
      }
      Serial.println(address, HEX);
      nDevices++;
    }
    else if (error == 4)
    {
      Serial.print("Unknow error at address 0x");
      if (address < 16)
      {
        Serial.print("0");
      }
      Serial.println(address, HEX);
    }
  }
  if (nDevices == 0)
  {
    Serial.println("No I2C devices found\n");
  }
  else
  {
    Serial.println("done\n");
  }
  delay(5000);
}
#endif