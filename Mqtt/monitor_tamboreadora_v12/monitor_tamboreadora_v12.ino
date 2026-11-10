// ================================================================
//   Monitor de Corrente — TCC FATEC
//   ESP32 + SCT-013-030 + EmonLib + MQTT + Google Sheets
//   v9
// ================================================================

#include "EmonLib.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

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
#define GOOGLE_URL "https://script.google.com/macros/s/AKfycbzgC0Bxy_ULVDlzlatfXKWTOAfKlur3ixTPPB1p_CsmrUIZ7E65uRO73QKWvif5pYGS/exec"

// ── Hardware ─────────────────────────────────────────────────────
const int PINO_SCT = 35;

// LEDs (lógica SINK: LOW = acende, HIGH = apaga)
const int led_vermelho_baixo = 32;
const int led_laranja_baixo  = 33;
const int led_amarelo_baixo  = 25;
const int led_verde          = 26;
const int led_amarelo_alto   = 27;
const int led_laranja_alto   = 14;
const int led_vermelho_alto  = 12;

// ── Calibração EmonLib ────────────────────────────────────────────
float fator_calibracao = 8.5;  // ajuste conforme medição real

// ── Objetos ───────────────────────────────────────────────────────
EnergyMonitor    emon1;
WiFiClientSecure wifiSecure;
PubSubClient     mqttClient(wifiSecure);

// ── Estatísticas ─────────────────────────────────────────────────
#define JANELA        10     // leituras por janela
#define MAX_HISTORICO 100    // leituras para correção geral

float  historico[MAX_HISTORICO];  // todas as leituras
float  janela[JANELA];            // leituras da janela atual
int    contJanela    = 0;         // contador da janela atual
int    contTotal     = 0;         // contador total de leituras
float  mediaGlobal   = 0.0;       // média das 100 leituras
float  fatorCorrecao = 0.0;       // diferença média100 - média10inicial
float  mediaJanela   = 0.0;       // média da janela atual
float  sigmaJanela   = 0.0;       // desvio padrão da janela atual
bool   correcaoAplicada = false;  // já calculou correção geral?
bool   primeiraLeitura  = true;   // ignora pico inicial

// ================================================================
//   FUNÇÕES AUXILIARES
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

void atualizarLeds(float irms) {
  apagarLeds();
  if (contTotal < JANELA + 1) {
    // Durante calibração pisca LED verde
    digitalWrite(led_verde, LOW);
    return;
  }
  float z = calcZScore(irms);
  if      (z < -3.0) digitalWrite(led_vermelho_baixo, LOW);
  else if (z < -2.0) digitalWrite(led_laranja_baixo,  LOW);
  else if (z < -1.0) digitalWrite(led_amarelo_baixo,  LOW);
  else if (z <=  1.0) digitalWrite(led_verde,          LOW);
  else if (z <=  2.0) digitalWrite(led_amarelo_alto,   LOW);
  else if (z <=  3.0) digitalWrite(led_laranja_alto,   LOW);
  else                digitalWrite(led_vermelho_alto,  LOW);
}

// ── Classificação por desvio padrão ─────────────────────────────
// Retorna quantos desvios padrão o valor está da média (com sinal)
float calcZScore(float irms) {
  if (sigmaJanela == 0.0) return 0.0;
  float mediaCorrigida = mediaJanela + fatorCorrecao;
  return (irms - mediaCorrigida) / sigmaJanela;
}

String obterNivel(float irms) {
  if (contTotal < JANELA + 1) return "CALIBRANDO";
  float z = calcZScore(irms);
  if      (z < -3.0) return "VERMELHO BAIXO";
  else if (z < -2.0) return "LARANJA BAIXO";
  else if (z < -1.0) return "AMARELO BAIXO";
  else if (z <=  1.0) return "VERDE - NORMAL";
  else if (z <=  2.0) return "AMARELO ALTO";
  else if (z <=  3.0) return "LARANJA ALTO";
  else                return "VERMELHO ALTO";
}

String obterEstado(float irms) {
  if (contTotal < JANELA + 1) return "Calibrando sistema...";
  float z = calcZScore(irms);
  if      (z < -3.0) return "Abaixo do Normal";
  else if (z < -2.0) return "Abaixo - Atencao";
  else if (z < -1.0) return "Levemente Baixo";
  else if (z <=  1.0) return "Normal";
  else if (z <=  2.0) return "Levemente Alto";
  else if (z <=  3.0) return "Acima - Atencao";
  else                return "Acima do Normal";
}

// ── Cálculo de média e desvio padrão da janela ───────────────────
void calcularEstatisticasJanela() {
  float soma = 0.0;
  for (int i = 0; i < JANELA; i++) soma += janela[i];
  mediaJanela = soma / JANELA;

  float somaSq = 0.0;
  for (int i = 0; i < JANELA; i++) {
    float diff = janela[i] - mediaJanela;
    somaSq += diff * diff;
  }
  sigmaJanela = sqrt(somaSq / JANELA);

  Serial.printf("  [Estatísticas] µ=%.4f  σ=%.4f  Correção=%.4f\n",
                mediaJanela, sigmaJanela, fatorCorrecao);
}

// ── Ordena array (Bubble Sort simples) ───────────────────────────
void ordenar(float* arr, int n) {
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (arr[j] > arr[j+1]) {
        float tmp = arr[j]; arr[j] = arr[j+1]; arr[j+1] = tmp;
      }
}

// ── Correção geral após 100 leituras (Mediana + Z-score) ─────────
void calcularCorrecaoGeral() {
  // 1. Copia histórico para ordenar sem modificar o original
  float copia[MAX_HISTORICO];
  for (int i = 0; i < MAX_HISTORICO; i++) copia[i] = historico[i];
  ordenar(copia, MAX_HISTORICO);

  // 2. Mediana das 100 leituras
  float mediana = (copia[49] + copia[50]) / 2.0;

  // 3. Desvio absoluto mediano (MAD) — robusto a outliers
  float desvios[MAX_HISTORICO];
  for (int i = 0; i < MAX_HISTORICO; i++)
    desvios[i] = abs(historico[i] - mediana);
  ordenar(desvios, MAX_HISTORICO);
  float mad = (desvios[49] + desvios[50]) / 2.0;
  float sigmaRobusto = mad * 1.4826;  // fator para distribuição normal

  // 4. Descarta outliers (z-score robusto > 2)
  float somaValidos = 0.0;
  int   countValidos = 0;
  Serial.printf("  [Outliers] Mediana=%.4f  σRobusto=%.4f\n", mediana, sigmaRobusto);
  for (int i = 0; i < MAX_HISTORICO; i++) {
    float zRobusto = (sigmaRobusto > 0) ? abs(historico[i] - mediana) / sigmaRobusto : 0;
    if (zRobusto <= 2.0) {
      somaValidos += historico[i];
      countValidos++;
    } else {
      Serial.printf("  [Descartado] %.4f A  z=%.2f\n", historico[i], zRobusto);
    }
  }

  // 5. Média global apenas com valores válidos
  mediaGlobal = (countValidos > 0) ? somaValidos / countValidos : mediana;

  // 6. Média das primeiras 10 leituras válidas (ignora índice 0)
  float somaIniciais = 0.0;
  int   countIniciais = 0;
  for (int i = 1; i <= JANELA; i++) {
    float zRobusto = (sigmaRobusto > 0) ? abs(historico[i] - mediana) / sigmaRobusto : 0;
    if (zRobusto <= 2.0) {
      somaIniciais += historico[i];
      countIniciais++;
    }
  }
  float mediaIniciais = (countIniciais > 0) ? somaIniciais / countIniciais : mediana;

  // 7. Fator de correção
  fatorCorrecao = mediaGlobal - mediaIniciais;

  Serial.printf("  [Correção Geral] µGlobal=%.4f (%d válidos)  µIniciais=%.4f  Correção=%.4f\n",
                mediaGlobal, countValidos, mediaIniciais, fatorCorrecao);
  correcaoAplicada = true;
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

void publicarMQTT(double irms, String nivel, String estado) {
  if (!mqttClient.connected()) conectarMQTT();
  mqttClient.loop();

  StaticJsonDocument<128> doc;
  doc["irms"]   = (float)round(irms * 10000.0) / 10000.0;
  doc["nivel"]  = nivel;
  doc["estado"] = estado;

  char payload[128];
  serializeJson(doc, payload);

  bool ok = mqttClient.publish(MQTT_TOPIC, payload, false);
  Serial.printf("  [MQTT] %s\n", ok ? "OK" : "ERRO");
}

// ================================================================
//   GOOGLE SHEETS
// ================================================================
void enviarGoogleSheets(double irms, String nivel, String estado) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure clienteHTTPS;
  clienteHTTPS.setInsecure();

  HTTPClient http;
  http.begin(clienteHTTPS, GOOGLE_URL);
  http.addHeader("Content-Type", "application/json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  StaticJsonDocument<128> doc;
  doc["irms"]   = (float)round(irms * 10000.0) / 10000.0;
  doc["nivel"]  = nivel;
  doc["estado"] = estado;

  char payload[128];
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
  Serial.println("  Monitor Tamboreadora — TCC FATEC");
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

  // Teste sequencial dos LEDs
  Serial.println("Testando LEDs...");
  int leds[] = {led_vermelho_baixo, led_laranja_baixo, led_amarelo_baixo,
                led_verde, led_amarelo_alto, led_laranja_alto, led_vermelho_alto};
  for (int i = 0; i < 7; i++) {
    digitalWrite(leds[i], LOW);
    delay(150);
    digitalWrite(leds[i], HIGH);
  }

  // ADC e EmonLib
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  emon1.current(PINO_SCT, fator_calibracao);

  // Conecta Wi-Fi e MQTT
  conectarWiFi();
  conectarMQTT();

  Serial.println("\nIniciando monitoramento...");
  Serial.println("Leit  |  Corrente (A)  |  Nivel");
  Serial.println("------+----------------+--------------------");
}

// ================================================================
//   LOOP
// ================================================================
void loop() {
  // ── Leitura estável (média de 20 amostras) ──────────────────
  double soma = 0;
  for (int i = 0; i < 20; i++) {
    soma += emon1.calcIrms(4000);
    delay(50);
  }
  float irms = (float)(soma / 20.0);
  if (irms < 0.05) irms = 0.0;

  contTotal++;

  // ── Fase 1: ignora primeira leitura (pico inicial) ──────────
  if (primeiraLeitura) {
    primeiraLeitura = false;
    Serial.printf("[  1]  %.4f A  | Ignorada (pico inicial)\n", irms);
    return;
  }

  // ── Salva no histórico geral ─────────────────────────────────
  if (contTotal - 1 < MAX_HISTORICO) {
    historico[contTotal - 1] = irms;
  }

  // ── Calcula correção geral após 100 leituras ─────────────────
  if (contTotal - 1 == MAX_HISTORICO && !correcaoAplicada) {
    calcularCorrecaoGeral();
  }

  // ── Adiciona à janela atual ──────────────────────────────────
  janela[contJanela] = irms;
  contJanela++;

  // ── A cada 10 leituras recalcula estatísticas ────────────────
  if (contJanela >= JANELA) {
    calcularEstatisticasJanela();
    contJanela = 0;
  }

  // ── Classificação ────────────────────────────────────────────
  String nivel  = obterNivel(irms);
  String estado = obterEstado(irms);

  // ── LEDs ─────────────────────────────────────────────────────
  atualizarLeds(irms);

  // ── Serial ───────────────────────────────────────────────────
  float z = (sigmaJanela > 0) ? calcZScore(irms) : 0.0;
  Serial.printf("[%4d]  %.4f A  z=%.2f  |  %s\n",
                contTotal, irms, z, nivel.c_str());

  // ── MQTT ─────────────────────────────────────────────────────
  publicarMQTT(irms, nivel, estado);

  // ── Google Sheets ────────────────────────────────────────────
  enviarGoogleSheets(irms, nivel, estado);
}
