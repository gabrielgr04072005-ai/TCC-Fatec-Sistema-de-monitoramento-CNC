// ================================================================
//   Monitor de Máquina — TCC FATEC
//   ESP32 + SCT-013-030 + ZMPT101B + MPU6050 + Google Sheets + MQTT
//   v16 — FASE DE COLETA: envia apenas dados BRUTOS (sem classificação)
//
//   Objetivo desta versão: coletar corrente, tensão, vibração (X/Y/Z)
//   e temperatura em cada ciclo, todos com o MESMO timestamp, sem
//   nenhuma lógica de z-score / MAD / correção. Essa lógica será
//   reaplicada depois, offline, em 3 cenários de análise:
//     1) dados brutos
//     2) dados filtrados (z-score + MAD)
//     3) saída do autoencoder (TinyML)
//
//   Pinagem:
//     SCT-013   -> GPIO35 (ADC1)
//     ZMPT101B  -> GPIO34 (ADC1)
//     MPU6050   -> I2C padrão (SDA=GPIO21, SCL=GPIO22)
//     LEDs      -> GPIO32,33,25,26,27,14,12 (SINK, LOW = acende)
// ================================================================

#include "EmonLib.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Wire.h>

// ── Wi-Fi ────────────────────────────────────────────────────────
#define WIFI_SSID  "Silva-2G"
#define WIFI_PASS  "04072005b"

// ── HiveMQ Cloud ─────────────────────────────────────────────────
#define MQTT_HOST      "14fc5a355dec4054b8609b092c9653d7.s1.eu.hivemq.cloud"
#define MQTT_PORT      8883
#define MQTT_USER      "esp32"
#define MQTT_PASS      "Tccfatec1"
#define MQTT_TOPIC     "tamboreadora/corrente"
#define MQTT_CLIENT_ID "esp32_tamboreadora_01"

// ── Google Sheets ─────────────────────────────────────────────────
// IMPORTANTE: essa URL precisa apontar para a implantação do Apps
// Script JÁ ATUALIZADO (doPost novo, com as colunas de tensão,
// vibração e temperatura). Se você fizer uma nova implantação do
// script, troque a URL abaixo pela nova.
#define GOOGLE_URL "https://script.google.com/macros/s/AKfycbzgC0Bxy_ULVDlzlatfXKWTOAfKlur3ixTPPB1p_CsmrUIZ7E65uRO73QKWvif5pYGS/exec"

// ── Hardware — pinos analógicos (ambos em ADC1, compatíveis com Wi-Fi) ──
const int PINO_SCT  = 35;   // SCT-013 (corrente)
const int PINO_ZMPT = 34;   // ZMPT101B (tensão)

// LEDs (lógica SINK: LOW = acende, HIGH = apaga)
const int led_vermelho_baixo = 32;
const int led_laranja_baixo  = 33;
const int led_amarelo_baixo  = 25;
const int led_verde          = 26;
const int led_amarelo_alto   = 27;
const int led_laranja_alto   = 14;
const int led_vermelho_alto  = 12;

// ── Calibração ────────────────────────────────────────────────────
float fator_calibracao_corrente = 8.5;   // EmonLib (SCT-013)

// ZMPT101B — ATENÇÃO: valor de partida ajustado proporcionalmente do
// Arduino UNO (10 bits) para o ESP32 (12 bits). PRECISA recalibrar
// medindo a tensão real da rede com um multímetro e reajustando.
const int   AMOSTRAS_ZMPT      = 1000;
float       fator_calibracao_zmpt = 0.92;

// ── MPU6050 ───────────────────────────────────────────────────────
const int ENDERECO_MPU = 0x68;

// ── Objetos ───────────────────────────────────────────────────────
EnergyMonitor    emon1;
WiFiClientSecure wifiSecure;
PubSubClient     mqttClient(wifiSecure);

// ── Timing ────────────────────────────────────────────────────────
const unsigned long INTERVALO_CICLO = 5000;  // 5s entre leituras/envios
unsigned long ultimoCiclo = 0;
unsigned long contador = 0;

// ================================================================
//   LEDS — apenas teste de boot nesta versão (sem classificação)
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
//   LEITURA — CORRENTE (SCT-013 via EmonLib) — valor bruto único
// ================================================================
float lerCorrente() {
  float irms = emon1.calcIrms(4000);
  if (irms < 0.05) irms = 0.0;
  return irms;
}

// ================================================================
//   LEITURA — TENSÃO (ZMPT101B) — Vrms bruto
//   (mesma lógica do tensao.ino: offset + RMS por amostragem direta)
// ================================================================
float lerTensao() {
  // 1 — offset DC
  float soma = 0;
  for (int i = 0; i < AMOSTRAS_ZMPT; i++) {
    soma += analogRead(PINO_ZMPT);
  }
  float offset = soma / AMOSTRAS_ZMPT;

  // 2 — RMS do sinal AC
  float somaQuadrados = 0;
  for (int i = 0; i < AMOSTRAS_ZMPT; i++) {
    int leitura = analogRead(PINO_ZMPT);
    float sinalAC = leitura - offset;
    somaQuadrados += sinalAC * sinalAC;
  }

  float rmsADC   = sqrt(somaQuadrados / AMOSTRAS_ZMPT);
  float tensaoRMS = rmsADC * fator_calibracao_zmpt;

  return tensaoRMS;
}

// ================================================================
//   LEITURA — MPU6050 — vibração (X/Y/Z brutos) e temperatura (°C)
// ================================================================
struct LeituraMPU {
  int16_t acelX;
  int16_t acelY;
  int16_t acelZ;
  float   temperaturaC;
  bool    valida;
};

LeituraMPU lerMPU() {
  LeituraMPU l;
  l.valida = false;

  Wire.beginTransmission(ENDERECO_MPU);
  Wire.write(0x3B);
  Wire.endTransmission(false);

  Wire.requestFrom(ENDERECO_MPU, 14, true);

  if (Wire.available() >= 14) {
    l.acelX = (Wire.read() << 8) | Wire.read();
    l.acelY = (Wire.read() << 8) | Wire.read();
    l.acelZ = (Wire.read() << 8) | Wire.read();

    int16_t temperaturaRaw = (Wire.read() << 8) | Wire.read();
    l.temperaturaC = (temperaturaRaw / 340.0) + 36.53;

    // Giroscópio não é usado nesta versão (colunas da planilha só
    // preveem vibração/temperatura), mas os bytes precisam ser lidos
    // para esvaziar o buffer corretamente.
    Wire.read(); Wire.read();
    Wire.read(); Wire.read();
    Wire.read(); Wire.read();

    l.valida = true;
  }

  return l;
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
//   MQTT
// ================================================================
void conectarMQTT() {
  wifiSecure.setInsecure();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setKeepAlive(60);
  Serial.print("Conectando ao HiveMQ...");
  int t = 0;
  while (!mqttClient.connected() && t < 10) {
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      Serial.println(" Conectado!");
    } else {
      Serial.printf(" Falha rc=%d, tentando...\n", mqttClient.state());
      delay(3000); t++;
    }
  }
}

void publicarMQTT(float irms, float tensao, int16_t vibX, int16_t vibY,
                   int16_t vibZ, float temperatura) {
  if (!mqttClient.connected()) conectarMQTT();
  mqttClient.loop();

  StaticJsonDocument<256> doc;
  doc["irms"]        = (float)round(irms * 10000.0) / 10000.0;
  doc["tensao"]       = (float)round(tensao * 100.0) / 100.0;
  doc["vibX"]         = vibX;
  doc["vibY"]         = vibY;
  doc["vibZ"]         = vibZ;
  doc["temperatura"]  = (float)round(temperatura * 100.0) / 100.0;
  doc["nivel"]        = "BRUTO";
  doc["estado"]       = "Leitura bruta - fase de coleta";

  char payload[256];
  serializeJson(doc, payload);

  bool ok = mqttClient.publish(MQTT_TOPIC, payload, false);
  Serial.printf("  [MQTT] %s\n", ok ? "OK" : "ERRO");
}

// ================================================================
//   GOOGLE SHEETS
// ================================================================
void enviarGoogleSheets(float irms, float tensao, int16_t vibX, int16_t vibY,
                         int16_t vibZ, float temperatura) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure clienteHTTPS;
  clienteHTTPS.setInsecure();

  HTTPClient http;
  http.begin(clienteHTTPS, GOOGLE_URL);
  http.addHeader("Content-Type", "application/json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  StaticJsonDocument<256> doc;
  doc["irms"]        = (float)round(irms * 10000.0) / 10000.0;
  doc["tensao"]       = (float)round(tensao * 100.0) / 100.0;
  doc["vibX"]         = vibX;
  doc["vibY"]         = vibY;
  doc["vibZ"]         = vibZ;
  doc["temperatura"]  = (float)round(temperatura * 100.0) / 100.0;
  doc["nivel"]        = "BRUTO";
  doc["estado"]       = "Leitura bruta - fase de coleta";

  char payload[256];
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  Serial.printf("  [Sheets] HTTP %d\n", httpCode);
  http.end();
}

// ================================================================
//   SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("=====================================");
  Serial.println("  Monitor Maquina — TCC FATEC (v16)");
  Serial.println("  Fase de coleta - dados brutos");
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

  // ADC — corrente e tensão (ambos ADC1, compatíveis com Wi-Fi)
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  emon1.current(PINO_SCT, fator_calibracao_corrente);

  // I2C — MPU6050
  Wire.begin();
  Wire.beginTransmission(ENDERECO_MPU);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission(true);
  delay(100);
  Serial.println("MPU6050 iniciado");

  // Conecta Wi-Fi e MQTT
  conectarWiFi();
  conectarMQTT();

  Serial.println("\nIniciando coleta...");
  Serial.println("Leit |  Corrente(A) | Tensao(V) | VibX | VibY | VibZ | Temp(C)");
  Serial.println("-----+--------------+-----------+------+------+------+--------");
}

// ================================================================
//   LOOP
// ================================================================
void loop() {
  unsigned long agora = millis();
  if (agora - ultimoCiclo < INTERVALO_CICLO && ultimoCiclo != 0) {
    return;
  }
  ultimoCiclo = agora;
  contador++;

  // ── Leituras (mesmo ciclo, mesmo timestamp) ──────────────────
  float irms = lerCorrente();
  float tensao = lerTensao();
  LeituraMPU mpu = lerMPU();

  int16_t vibX = mpu.valida ? mpu.acelX : 0;
  int16_t vibY = mpu.valida ? mpu.acelY : 0;
  int16_t vibZ = mpu.valida ? mpu.acelZ : 0;
  float temperatura = mpu.valida ? mpu.temperaturaC : 0.0;

  if (!mpu.valida) {
    Serial.println("  [AVISO] MPU6050 nao respondeu nesse ciclo.");
  }

  // ── Serial ────────────────────────────────────────────────────
  Serial.printf("[%4lu] %.4f A | %.2f V | %d | %d | %d | %.2f C\n",
                contador, irms, tensao, vibX, vibY, vibZ, temperatura);

  // ── MQTT ──────────────────────────────────────────────────────
  publicarMQTT(irms, tensao, vibX, vibY, vibZ, temperatura);

  // ── Google Sheets ─────────────────────────────────────────────
  enviarGoogleSheets(irms, tensao, vibX, vibY, vibZ, temperatura);
}
