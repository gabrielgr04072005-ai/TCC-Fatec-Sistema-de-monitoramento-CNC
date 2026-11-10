// ================================================================
//   Scanner I2C — diagnóstico de hardware
//   Funciona tanto no ESP32 quanto no Arduino UNO sem alterações
//   (a biblioteca Wire já usa os pinos padrão de cada placa:
//    ESP32 -> SDA=21, SCL=22 | Arduino UNO -> SDA=A4, SCL=A5)
//
//   Não depende de nenhuma lógica do MPU6050 — só varre todos os
//   endereços possíveis e avisa quais dispositivos respondem.
// ================================================================

#include <Wire.h>

void setup() {
  Wire.begin();
  Serial.begin(115200);
  delay(500);
  Serial.println("\nScanner I2C iniciado...");
}

void loop() {
  int dispositivosEncontrados = 0;

  Serial.println("Escaneando barramento I2C...");

  for (byte endereco = 1; endereco < 127; endereco++) {
    Wire.beginTransmission(endereco);
    byte erro = Wire.endTransmission();

    if (erro == 0) {
      Serial.print("Dispositivo encontrado no endereco 0x");
      if (endereco < 16) Serial.print("0");
      Serial.print(endereco, HEX);

      if (endereco == 0x68) Serial.print("  <- provavelmente o MPU6050 (AD0=GND)");
      if (endereco == 0x69) Serial.print("  <- provavelmente o MPU6050 (AD0=VCC)");

      Serial.println();
      dispositivosEncontrados++;
    } else if (erro == 4) {
      Serial.print("Erro desconhecido no endereco 0x");
      if (endereco < 16) Serial.print("0");
      Serial.println(endereco, HEX);
    }
  }

  if (dispositivosEncontrados == 0) {
    Serial.println("Nenhum dispositivo I2C encontrado.");
    Serial.println(" -> Verifique alimentacao (VCC), GND comum, e fiacao SDA/SCL.");
  } else {
    Serial.printf("Total: %d dispositivo(s) encontrado(s).\n", dispositivosEncontrados);
  }

  Serial.println("-----------------------------------------");
  delay(3000);
}
