// ================================================================
//   Teste simples do MPU6050 (vibração) — ESP32
//   Só le o sensor e mostra no Serial Monitor. Sem Wi-Fi, sem
//   planilha, sem classificação — só pra confirmar que o sensor
//   está respondendo e os valores fazem sentido.
//
//   Pinagem:
//     MPU6050 -> I2C padrão do ESP32 (SDA=GPIO21, SCL=GPIO22, VCC=3.3V, GND=GND)
// ================================================================

#include <Wire.h>

const int ENDERECO_MPU = 0x68;

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin();        // SDA=21, SCL=22 por padrão no ESP32
  Wire.setClock(100000);

  // Acorda o MPU6050 (ele começa em modo "sleep" por padrão)
  Wire.beginTransmission(ENDERECO_MPU);
  Wire.write(0x6B);   // registrador PWR_MGMT_1
  Wire.write(0x00);   // 0x00 = acorda o sensor
  byte erro = Wire.endTransmission(true);

  if (erro == 0) {
    Serial.println("MPU6050 respondeu no endereco 0x68. Iniciando leituras...");
  } else {
    Serial.println("ERRO: MPU6050 nao respondeu. Confere fiacao (SDA=21, SCL=22) e alimentacao.");
  }

  Serial.println("AcelX  | AcelY  | AcelZ  | Temp(C)");
}

void loop() {
  Wire.beginTransmission(ENDERECO_MPU);
  Wire.write(0x3B);  // primeiro registrador de aceleração (ACCEL_XOUT_H)
  byte erro = Wire.endTransmission(false);

  if (erro != 0) {
    Serial.printf("Falha na leitura (endTransmission erro=%d)\n", erro);
    delay(1000);
    return;
  }

  Wire.requestFrom(ENDERECO_MPU, 14, true);

  if (Wire.available() < 14) {
    Serial.println("Falha na leitura (bytes insuficientes)");
    delay(1000);
    return;
  }

  int16_t acelX = (Wire.read() << 8) | Wire.read();
  int16_t acelY = (Wire.read() << 8) | Wire.read();
  int16_t acelZ = (Wire.read() << 8) | Wire.read();
  int16_t tempRaw = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read(); Wire.read(); Wire.read(); Wire.read(); Wire.read(); // giroscópio, ignorado aqui

  float temperaturaC = (tempRaw / 340.0) + 36.53;

  Serial.printf("%6d | %6d | %6d | %.2f\n", acelX, acelY, acelZ, temperaturaC);

  delay(1000); // 1 leitura por segundo
}
