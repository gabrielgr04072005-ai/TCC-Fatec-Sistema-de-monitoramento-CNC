#include <Wire.h>

// =====================================================
// ENDEREÇO DO MPU6050
// =====================================================

const int ENDERECO_SENSOR = 0x68;

// =====================================================
// VARIÁVEIS
// =====================================================

int16_t acelX;
int16_t acelY;
int16_t acelZ;

int16_t girX;
int16_t girY;
int16_t girZ;

int16_t temperaturaRaw;

float temperaturaC;

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(9600);

  Wire.begin();

  // Inicializa o MPU6050
  Wire.beginTransmission(ENDERECO_SENSOR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission(true);

  delay(100);

  Serial.println("MPU6050 iniciado");
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
  // ===================================================
  // Solicita dados a partir do registrador 0x3B
  // ===================================================

  Wire.beginTransmission(ENDERECO_SENSOR);
  Wire.write(0x3B);
  Wire.endTransmission(false);

  Wire.requestFrom(ENDERECO_SENSOR, 14, true);

  // Verifica se recebeu os 14 bytes
  if (Wire.available() >= 14)
  {
    // =================================================
    // ACELERÔMETRO
    // =================================================

    acelX = (Wire.read() << 8) | Wire.read();
    acelY = (Wire.read() << 8) | Wire.read();
    acelZ = (Wire.read() << 8) | Wire.read();

    // =================================================
    // TEMPERATURA
    // =================================================

    temperaturaRaw = (Wire.read() << 8) | Wire.read();

    // Conversão para Celsius
    temperaturaC = (temperaturaRaw / 340.0) + 36.53;

    // =================================================
    // GIROSCÓPIO
    // =================================================

    girX = (Wire.read() << 8) | Wire.read();
    girY = (Wire.read() << 8) | Wire.read();
    girZ = (Wire.read() << 8) | Wire.read();

    // =================================================
    // SERIAL PLOTTER
    // =================================================

    Serial.print("X:");
    Serial.print(acelX);

    Serial.print("\tY:");
    Serial.print(acelY);

    Serial.print("\tZ:");
    Serial.print(acelZ);

    Serial.print("\tTemp:");
    Serial.print(temperaturaC, 2);
    Serial.println("°C");
  }

  delay(1000);
}

