// ================================================================
//   Monitor de Máquina — TCC FATEC
//   ESP32 + MPU6050 + Google Sheets
//   v21 — MODO DE TESTE: captura bruta de vibração
//   (baseado na v18, ramo separado — não é a v20/PZEM da linha principal)
//
//   O que mudou em relação à v18:
//   - Corrente (SCT-013), tensão (ZMPT101B), classificação por
//     z-score/tiers, LEDs de status e MQTT foram TEMPORARIAMENTE
//     removidos. Esta versão é só pra validar o envio de vibração.
//   - Ciclo de leitura passou de 5s para 1s.
//   - Cada leitura de vibração é enviada IMEDIATAMENTE pra planilha,
//     em um POST HTTP separado por segundo (sem agrupar em lote/array).
//   - Contadores "leituraNoCiclo" (1 a 20) e "ciclo" identificam
//     blocos de 20 segundos na planilha, só pra facilitar visualizar
//     as janelas de 20 leituras depois.
//   - Toda a robustez de leitura I2C do MPU6050 (3 tentativas, checagem
//     de endTransmission, bus recovery manual do SCL/SDA, reaproveitar
//     o último valor válido em vez de zerar) foi mantida idêntica à v18.
//
//   Pendências / suposições (confirmar com Gabriel):
//   - O payload continua usando os MESMOS nomes de campo que o Apps
//     Script atual já espera (irms, tensao, vibX, vibY, vibZ,
//     temperatura, nivel, estado, mpu_ok), só que irms/tensao vão
//     zerados e nivel="TESTE_VIBRACAO" — isso evita ter que mexer no
//     .gs agora. Adicionei "leituraNoCiclo" e "ciclo" como campos
//     extras; se o Apps Script não tiver colunas pra eles, ele
//     provavelmente vai simplesmente ignorar esses dois campos.
//     Manda o .gs se quiser que eu já deixe as colunas certas.
//   - O loop roda continuamente (não para depois de um único bloco de
//     20) — a cada 20 leituras o contador zera e o número do ciclo
//     sobe, então a planilha vai recebendo blocos de 20 em 20 segundos
//     sem parar. Se era pra rodar só UM bloco de 20 e parar, me avisa.
//
//   Pinagem:
//     MPU6050   -> I2C padrão (SDA=GPIO21, SCL=GPIO22)
//     LEDs      -> GPIO32,33,25,26,27,14,12 (SINK, LOW = acende)
//                  — usados só no teste de boot nesta versão.
// ================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Wire.h>

// ── Wi-Fi ────────────────────────────────────────────────────────
#define WIFI_SSID  "Silva-2G"
#define WIFI_PASS  "04072005b"

// ── Google Sheets ─────────────────────────────────────────────────
// IMPORTANTE: mesma implantação do Apps Script já em uso. Se criar
// uma implantação NOVA (não editar a existente), troque a URL abaixo.
#define GOOGLE_URL "https://script.google.com/macros/s/AKfycbxowTRzRHfvT6_0x0U7oIz1T1sNKipHV-6jm3E76VSRFoP_0xHzvbpQd3B1j7cczjWe/exec"

// Pinos I2C (usados pela recuperação manual de barramento)
const int PINO_SDA = 21;
const int PINO_SCL = 22;

// LEDs (lógica SINK: LOW = acende, HIGH = apaga) — só teste de boot
const int led_vermelho_baixo = 32;
const int led_laranja_baixo  = 33;
const int led_amarelo_baixo  = 25;
const int led_verde          = 26;
const int led_amarelo_alto   = 27;
const int led_laranja_alto   = 14;
const int led_vermelho_alto  = 12;

// ── MPU6050 ───────────────────────────────────────────────────────
const int ENDERECO_MPU   = 0x68;
const int MPU_TENTATIVAS = 3;   // tentativas de leitura antes de desistir

// Struct precisa estar declarada antes de qualquer função (a IDE do
// Arduino gera os protótipos das funções logo no topo do arquivo,
// antes de qualquer tipo definido mais abaixo no .ino).
struct LeituraMPU {
  int16_t acelX;
  int16_t acelY;
  int16_t acelZ;
  float   temperaturaC;
  bool    valida;
};

// ── Timing do ciclo de leitura ────────────────────────────────────
const unsigned long INTERVALO_LEITURA = 1000;  // 1s entre leituras/envios
unsigned long ultimaLeitura = 0;

// ── Contadores do "modo teste" (blocos de 20 leituras = 20s) ──────
const int LEITURAS_POR_CICLO = 20;
int           leituraNoCiclo = 0;   // 1..20, reinicia a cada bloco
unsigned long numeroCiclo    = 1;   // sobe a cada 20 leituras

// ── Retenção do último valor válido do MPU6050 (mantido da v18) ──
// Usado quando uma leitura falha mesmo após as tentativas: em vez de
// zerar vibração/temperatura (o que contaminava dados com zeros
// falsos), reaproveita a última leitura boa conhecida.
int16_t ultimoVibXValido = 0;
int16_t ultimoVibYValido = 0;
int16_t ultimoVibZValido = 0;
float   ultimaTemperaturaValida = 0.0;
bool    jaTeveLeituraValidaMPU  = false;

// ── Contadores de falha do MPU6050 (mantido da v18) ───────────────
unsigned long totalCiclosMPU = 0;
unsigned long totalFalhasMPU = 0;

// ================================================================
//   LEDS (só teste de boot nesta versão)
// ================================================================
void apagarLeds() {
  digitalWrite(led_vermelho_baixo, HIGH);
  digitalWrite(led_laranja_baixo,  HIGH);
  digitalWrite(led_amarelo_baixo,  HIGH);
  digitalWrite(led_verde,          HIGH);
  digitalWrite(led_amarelo_alto,   HIGH);
  digitalWrite(led_laranja_alto,   HIGH);
  digitalWrite(led_vermelho_alto,  HIGH);
}

void testarLedsBoot() {
  int leds[] = {led_vermelho_baixo, led_laranja_baixo, led_amarelo_baixo,
                led_verde, led_amarelo_alto, led_laranja_alto, led_vermelho_alto};
  for (int i = 0; i < 7; i++) {
    digitalWrite(leds[i], LOW);
    delay(150);
    digitalWrite(leds[i], HIGH);
  }
}

// ================================================================
//   I2C — RECUPERAÇÃO DE BARRAMENTO (idêntico à v18)
// ================================================================
// Se um escravo trava no meio de uma transação (comum com ruído
// elétrico de motor/inversor), o SDA pode ficar preso em nível baixo
// e o barramento não se recupera sozinho. A correção clássica é
// "bater" o SCL manualmente até 9 vezes (liberando qualquer bit que
// o escravo estivesse tentando enviar) e depois soltar com um STOP.
void recuperarBarramentoI2C() {
  Serial.println("  [I2C] Tentando recuperar o barramento (bus recovery)...");

  Wire.end();

  pinMode(PINO_SDA, INPUT_PULLUP);
  pinMode(PINO_SCL, OUTPUT);

  for (int i = 0; i < 9; i++) {
    digitalWrite(PINO_SCL, HIGH);
    delayMicroseconds(5);
    digitalWrite(PINO_SCL, LOW);
    delayMicroseconds(5);
  }

  // Gera uma condição de STOP manual: SDA sobe enquanto SCL está alto
  pinMode(PINO_SDA, OUTPUT);
  digitalWrite(PINO_SDA, LOW);
  digitalWrite(PINO_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(PINO_SDA, HIGH);
  delayMicroseconds(5);

  // Reinicializa o barramento I2C normalmente
  Wire.begin();
  Wire.setClock(100000);
}

// ================================================================
//   LEITURA — MPU6050 — vibração (X/Y/Z brutos) e temperatura (°C)
//   (idêntico à v18: até MPU_TENTATIVAS tentativas, depois bus
//   recovery; o chamador decide o que fazer se l.valida == false)
// ================================================================
LeituraMPU lerMPU() {
  LeituraMPU l;
  l.valida = false;

  for (int tentativa = 1; tentativa <= MPU_TENTATIVAS; tentativa++) {
    Wire.beginTransmission(ENDERECO_MPU);
    Wire.write(0x3B);
    uint8_t erroTx = Wire.endTransmission(false);

    if (erroTx != 0) {
      Serial.printf("  [MPU6050] Tentativa %d/%d: falha no endTransmission (erro=%u)\n",
                    tentativa, MPU_TENTATIVAS, erroTx);
      delay(5);
      continue; // tenta de novo sem gastar uma recuperação de barramento ainda
    }

    uint8_t bytesRecebidos = Wire.requestFrom(ENDERECO_MPU, 14, true);

    if (bytesRecebidos >= 14 && Wire.available() >= 14) {
      l.acelX = (Wire.read() << 8) | Wire.read();
      l.acelY = (Wire.read() << 8) | Wire.read();
      l.acelZ = (Wire.read() << 8) | Wire.read();

      int16_t temperaturaRaw = (Wire.read() << 8) | Wire.read();
      l.temperaturaC = (temperaturaRaw / 340.0) + 36.53;

      Wire.read(); Wire.read();
      Wire.read(); Wire.read();
      Wire.read(); Wire.read();

      l.valida = true;
      return l; // sucesso — não precisa das próximas tentativas
    }

    Serial.printf("  [MPU6050] Tentativa %d/%d: bytes insuficientes (%u/14)\n",
                  tentativa, MPU_TENTATIVAS, bytesRecebidos);
    delay(5);
  }

  // Todas as tentativas falharam — provável barramento travado.
  // Recupera o barramento agora pra não perpetuar a falha no próximo ciclo.
  recuperarBarramentoI2C();

  return l; // l.valida == false
}

// ================================================================
//   WI-FI
// ================================================================
void conectarWiFi() {
  Serial.print("Conectando ao Wi-Fi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 30) {
    delay(500); Serial.print("."); t++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi conectado! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nFalha no Wi-Fi! Reiniciando...");
    ESP.restart();
  }
}

// ================================================================
//   GOOGLE SHEETS — envia UMA leitura de vibração por vez
// ================================================================
void enviarLeituraVibracao(int16_t vibX, int16_t vibY, int16_t vibZ,
                            float temperatura, bool mpuValidaNesseCiclo,
                            int leituraAtual, unsigned long cicloAtual) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure clienteHTTPS;
  clienteHTTPS.setInsecure();

  HTTPClient http;
  http.begin(clienteHTTPS, GOOGLE_URL);
  http.addHeader("Content-Type", "application/json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000); // 15s — evita timeout quando o Apps Script demora

  StaticJsonDocument<288> doc;
  doc["irms"]           = 0.0;              // desativado neste modo de teste
  doc["tensao"]         = 0.0;              // desativado neste modo de teste
  doc["vibX"]           = vibX;
  doc["vibY"]           = vibY;
  doc["vibZ"]           = vibZ;
  doc["temperatura"]    = (float)round(temperatura * 100.0) / 100.0;
  doc["nivel"]          = "TESTE_VIBRACAO";
  doc["estado"]         = "Leitura " + String(leituraAtual) + "/" +
                           String(LEITURAS_POR_CICLO) + " do ciclo " + String(cicloAtual);
  doc["mpu_ok"]         = mpuValidaNesseCiclo;
  doc["leituraNoCiclo"] = leituraAtual;
  doc["ciclo"]          = cicloAtual;

  char payload[288];
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  Serial.printf("  [Sheets] Leitura %d/%d (ciclo %lu) -> HTTP %d\n",
                leituraAtual, LEITURAS_POR_CICLO, cicloAtual, httpCode);
  http.end();
}

// ================================================================
//   SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("=====================================");
  Serial.println("  Monitor Maquina — TCC FATEC (v21)");
  Serial.println("  MODO DE TESTE: so vibracao, 1 leitura/s");
  Serial.println("=====================================");

  // LEDs
  pinMode(led_vermelho_baixo, OUTPUT);
  pinMode(led_laranja_baixo,  OUTPUT);
  pinMode(led_amarelo_baixo,  OUTPUT);
  pinMode(led_verde,          OUTPUT);
  pinMode(led_amarelo_alto,   OUTPUT);
  pinMode(led_laranja_alto,   OUTPUT);
  pinMode(led_vermelho_alto,  OUTPUT);
  apagarLeds();

  Serial.println("Testando LEDs...");
  testarLedsBoot();

  // I2C — MPU6050 (clock fixado em 100kHz, mais tolerante a cabo
  // longo e ruído do que os 400kHz "fast mode")
  Wire.begin();
  Wire.setClock(100000);
  Wire.beginTransmission(ENDERECO_MPU);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission(true);
  delay(100);
  Serial.println("MPU6050 iniciado");

  // Conecta Wi-Fi (sem MQTT nesta versão)
  conectarWiFi();

  Serial.println("\nIniciando captura de vibracao (1 leitura/s, blocos de 20)...");
  Serial.println("Ciclo |  # | VibX  | VibY  | VibZ  | Temp(C)");
  Serial.println("------+----+-------+-------+-------+--------");
}

// ================================================================
//   LOOP
// ================================================================
void loop() {
  unsigned long agora = millis();
  if (agora - ultimaLeitura < INTERVALO_LEITURA && ultimaLeitura != 0) {
    return;
  }
  ultimaLeitura = agora;

  totalCiclosMPU++;
  LeituraMPU mpu = lerMPU();

  int16_t vibX, vibY, vibZ;
  float   temperatura;

  if (mpu.valida) {
    // Leitura fresca — atualiza o "último valor bom" pra usar se o
    // próximo ciclo falhar.
    vibX = mpu.acelX;
    vibY = mpu.acelY;
    vibZ = mpu.acelZ;
    temperatura = mpu.temperaturaC;

    ultimoVibXValido = vibX;
    ultimoVibYValido = vibY;
    ultimoVibZValido = vibZ;
    ultimaTemperaturaValida = temperatura;
    jaTeveLeituraValidaMPU = true;
  } else {
    // Falhou mesmo após as tentativas + bus recovery. Em vez de zerar
    // (o que fabricaria zeros falsos nos dados de vibração), reaproveita
    // o último valor válido conhecido.
    totalFalhasMPU++;
    float taxaFalha = (totalCiclosMPU > 0) ? (100.0 * totalFalhasMPU / totalCiclosMPU) : 0.0;
    Serial.printf("  [AVISO] MPU6050 nao respondeu nesse ciclo (falhas: %lu/%lu = %.1f%%). Reaproveitando ultimo valor valido.\n",
                  totalFalhasMPU, totalCiclosMPU, taxaFalha);

    if (jaTeveLeituraValidaMPU) {
      vibX = ultimoVibXValido;
      vibY = ultimoVibYValido;
      vibZ = ultimoVibZValido;
      temperatura = ultimaTemperaturaValida;
    } else {
      // Ainda não teve NENHUMA leitura válida desde o boot — não há
      // valor anterior pra reaproveitar, então usa 0 só nesse caso.
      vibX = 0; vibY = 0; vibZ = 0; temperatura = 0.0;
    }
  }

  // ── Contador do bloco de 20 leituras / 20 segundos ────────────
  leituraNoCiclo++;
  if (leituraNoCiclo > LEITURAS_POR_CICLO) {
    leituraNoCiclo = 1;
    numeroCiclo++;
  }

  // ── Serial ───────────────────────────────────────────────────
  Serial.printf("[%4lu] %2d/%2d | %6d | %6d | %6d | %.2f C%s\n",
                numeroCiclo, leituraNoCiclo, LEITURAS_POR_CICLO,
                vibX, vibY, vibZ, temperatura,
                mpu.valida ? "" : "  (MPU reaproveitado)");

  // ── Google Sheets — uma leitura por vez, em tempo real ────────
  enviarLeituraVibracao(vibX, vibY, vibZ, temperatura, mpu.valida,
                        leituraNoCiclo, numeroCiclo);
}
