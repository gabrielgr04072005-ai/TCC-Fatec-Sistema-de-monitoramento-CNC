// ================================================================
//   Teste isolado do MPU6050 no ESP32
//   Pinagem: SDA = GPIO21, SCL = GPIO22 (I2C padrão do ESP32)
//
//   O que este teste faz:
//   - Verifica se o MPU6050 responde no endereço 0x68
//   - Lê aceleração (X/Y/Z), giroscópio (X/Y/Z) e temperatura
//   - Imprime tudo no Serial Monitor a cada 500ms
// ================================================================

#include <Wire.h>

const int ENDERECO_MPU = 0x68;

int16_t acelX, acelY, acelZ;
int16_t giroX, giroY, giroZ;
int16_t temperaturaRaw;

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("=====================================");
  Serial.println("   Teste MPU6050 - ESP32");
  Serial.println("=====================================");

  Wire.begin(); // SDA=21, SCL=22 (padrão do ESP32)

  // Verifica se o sensor responde
  Wire.beginTransmission(ENDERECO_MPU);
  byte erro = Wire.endTransmission();

  if (erro == 0) {
    Serial.println("MPU6050 encontrado no endereco 0x68!");
  } else {
    Serial.println("ERRO: MPU6050 nao respondeu. Verifique:");
    Serial.println(" - Fiacao: SDA->GPIO21, SCL->GPIO22, VCC->3V3, GND->GND");
    Serial.println(" - Se o sensor esta alimentado corretamente");
    Serial.println(" - Se o endereco I2C nao e 0x69 (pino AD0 em HIGH)");
  }

  // Acorda o sensor (sai do modo sleep, registrador PWR_MGMT_1)
  Wire.beginTransmission(ENDERECO_MPU);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission(true);
  delay(100);

  Serial.println("\nIniciando leituras...\n");
  Serial.println("AcelX\tAcelY\tAcelZ\tGiroX\tGiroY\tGiroZ\tTemp(C)");
}

void loop() {
  Wire.beginTransmission(ENDERECO_MPU);
  Wire.write(0x3B); // registrador inicial (ACCEL_XOUT_H)
  Wire.endTransmission(false);
  Wire.requestFrom(ENDERECO_MPU, 14, true);

  if (Wire.available() >= 14) {
    acelX = (Wire.read() << 8) | Wire.read();
    acelY = (Wire.read() << 8) | Wire.read();
    acelZ = (Wire.read() << 8) | Wire.read();
    temperaturaRaw = (Wire.read() << 8) | Wire.read();
    giroX = (Wire.read() << 8) | Wire.read();
    giroY = (Wire.read() << 8) | Wire.read();
    giroZ = (Wire.read() << 8) | Wire.read();

    float temperaturaC = (temperaturaRaw / 340.0) + 36.53;

    Serial.printf("%d\t%d\t%d\t%d\t%d\t%d\t%.2f\n",
                  acelX, acelY, acelZ, giroX, giroY, giroZ, temperaturaC);
  } else {
    Serial.println("Falha na leitura do MPU6050 (sem dados no I2C).");
  }

  delay(500);
}
