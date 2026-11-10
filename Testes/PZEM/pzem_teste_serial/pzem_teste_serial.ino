// =====================================================
//   PZEM-004T V3.0 — Teste direto no Serial Monitor
// =====================================================
//
// Este sketch NÃO precisa do software do PC. O próprio Arduino
// consulta o PZEM via Modbus-RTU e mostra os valores na tela.
//
// PRECISA instalar a biblioteca antes de compilar:
//   Sketch -> Incluir Biblioteca -> Gerenciar Bibliotecas
//   Busque por "PZEM004Tv30" (autor: Jakub Mandula) e instale.
//
// Pinagem (mesma do passthrough, não precisa remexer nos fios):
//   PZEM TX  -> Arduino D10 (RX)
//   PZEM RX  -> Arduino D11 (TX)
//   PZEM GND -> Arduino GND
//
// IMPORTANTE: o PZEM-004T só liga de verdade com a rede AC (L/N)
// conectada — o pino "5V" da interface TTL é saída, não entrada.
// =====================================================

#include <PZEM004Tv30.h>

// PZEM configurado no endereço 0x01 (mesmo endereço usado no teste do
// software do Windows, onde alguma resposta chegou). Se você mudar o
// endereço do dispositivo depois, atualize esse valor também.
PZEM004Tv30 pzem(10, 11, 0x01); // RX, TX, endereço

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("=====================================");
  Serial.println("   Teste PZEM-004T V3.0");
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
