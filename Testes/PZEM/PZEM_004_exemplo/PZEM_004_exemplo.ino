//Programa para ESP32 C3

#include <PZEM004Tv30.h>
PZEM004Tv30 pzem1(Serial2,17,16); // GPIO20 Rx (Tx do PZEM) GPIO21 Tx (Rx do PZEM)
float VOLTAGE, CURRENT, POWER, energy1, Freq, pf1, va1, VAR1;

void setup() 
{
  delay(2000);
  Serial.begin(115200);
  Serial.println("\nFZEM004T");
}

void loop() 
{
  VOLTAGE = pzem1.voltage();
  VOLTAGE = zeroIfNan(VOLTAGE);
  CURRENT = pzem1.current();
  CURRENT = zeroIfNan(CURRENT);
  POWER = pzem1.power();
  POWER = zeroIfNan(POWER);
  energy1 = pzem1.energy() / 1000; //kwh
  energy1 = zeroIfNan(energy1);
  Freq = pzem1.frequency();
  Freq = zeroIfNan(Freq);
  pf1 = pzem1.pf();
  pf1 = zeroIfNan(pf1);
  if (pf1 == 0) 
  va1 = 0;
  else 
  va1 = POWER / pf1;
  
  if (pf1 == 0) 
  VAR1 = 0;
  else
  VAR1 = POWER / pf1 * sqrt(1-sq(pf1));
  delay(1000);
  Serial.println("");
  Serial.printf("Voltage        : %.2f\ V\n", VOLTAGE);
  Serial.printf("Current        : %.2f\ A\n", CURRENT);
  Serial.printf("Power Active   : %.2f\ W\n", POWER);
  Serial.printf("Frequency      : %.2f\ Hz\n", Freq);
  Serial.printf("Cosine Phi     : %.2f\ PF\n", pf1);
  Serial.printf("Energy         : %.2f\ kWh\n", energy1);
  Serial.printf("Apparent Power : %.2f\ VA\n", va1);
  Serial.printf("Reactive Power : %.2f\ VAR\n", VAR1);
  Serial.printf("---------- END ----------");
  Serial.println("");
}

float zeroIfNan(float v) 
{
  if (isnan(v)) 
  v = 0;
  return v;
}