// =====================================================
//   PZEM-004T V3.0 — Teste no ESP32 (UART de hardware)
// =====================================================
//
// Usa Serial2 (UART de hardware do ESP32), muito mais confiável que
// SoftwareSerial do Arduino UNO para esse tipo de comunicação.
//
// PRECISA instalar a biblioteca antes de compilar:
//   Sketch -> Incluir Biblioteca -> Gerenciar Bibliotecas
//   Busque por "PZEM004Tv30" (autor: Jakub Mandula) e instale.
//
// Pinagem (mesma já definida pro projeto final):
//   PZEM TX  -> ESP32 GPIO4  (RX)
//   PZEM RX  -> ESP32 GPIO13 (TX)
//   PZEM GND -> ESP32 GND
//
// IMPORTANTE: o PZEM-004T só liga de verdade com a rede AC (L/N)
// conectada — o pino "5V" da interface TTL é saída, não entrada.
// =====================================================

#include <PZEM004Tv30.h>

#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17

// Tenta primeiro no endereço 0x01 (mesmo que respondeu, ainda que
// corrompido, no teste do software do Windows).
PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0x01);

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("=====================================");
  Serial.println("   Teste PZEM-004T V3.0 - ESP32");
  Serial.println("=====================================");

  Serial.print("Endereco do dispositivo: 0x");
  Serial.println(pzem.readAddress(), HEX);
}

void loop() {
  float voltage    = pzem.voltage();
  float current    = pzem.current();
  float power      = pzem.power();
  float energy     = pzem.energy();
  float frequency  = pzem.frequency();
  float pf         = pzem.pf();

  if (isnan(voltage)) {
    Serial.println("Erro ao ler o PZEM (sem resposta). Verifique fiacao e alimentacao AC.");
  } else {
    Serial.print("Tensao: ");     Serial.print(voltage);    Serial.println(" V");
    Serial.print("Corrente: ");   Serial.print(current);    Serial.println(" A");
    Serial.print("Potencia: ");   Serial.print(power);      Serial.println(" W");
    Serial.print("Energia: ");    Serial.print(energy, 3);  Serial.println(" kWh");
    Serial.print("Frequencia: "); Serial.print(frequency);  Serial.println(" Hz");
    Serial.print("Fator de Potencia: "); Serial.println(pf);
  }

  Serial.println("-------------------------------------");
  delay(2000);
}
