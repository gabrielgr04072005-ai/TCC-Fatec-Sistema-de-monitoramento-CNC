// ================================================================
//   Monitor de Máquina — TCC FATEC
//   ESP32 + PZEM-004T v3.0 — v22
//
//   O que mudou em relação à v20/v21:
//   - Vibração (MPU6050) foi REMOVIDA de vez — Gabriel confirmou que
//     não é viável continuar com ela por enquanto.
//   - PZEM-004T agora usa a fiação e a lib validadas por Gabriel:
//     PZEM004Tv30, Serial2, RX=GPIO16, TX=GPIO17.
//     (a nota antiga de GPIO4/GPIO13 está desatualizada — essa
//     versão usa 16/17.)
//   - Sem classificação: nada de Nível/Estado/z-score/LEDs. Só
//     manda os valores brutos do PZEM (tensão, corrente, potência,
//     energia, frequência, fator de potência) pra planilha.
//   - Leitura/envio a cada 10 segundos.
//   - MQTT (HiveMQ Cloud) mantido, publicando o mesmo payload que
//     vai pra planilha.
//
//   PENDENTE — preencher antes de compilar:
//   - MQTT_BROKER / MQTT_USER / MQTT_PASS abaixo: não tenho essas
//     credenciais salvas (nunca foram me passadas), então estão
//     como placeholder. Troca pelos valores reais do seu cluster
//     HiveMQ Cloud.
//   - Se o Apps Script atual (.gs) esperava os campos "nivel" e
//     "estado" pra preencher as colunas Nível/Estado da planilha,
//     essas colunas devem ficar em branco/erro agora, já que esse
//     firmware não manda mais esses campos — isso é esperado, mas
//     me avisa se quiser que eu ajuste o script também.
//
//   Biblioteca necessária (Gerenciador de Bibliotecas do Arduino IDE):
//     - "PZEM004Tv30" (Olehs/Jakub Mandula)
//     - "PubSubClient" (Nick O'Leary) — pro MQTT
//     - "ArduinoJson"
//
//   Pinagem:
//     PZEM-004T -> Serial2 do ESP32 (RX=GPIO16, TX=GPIO17)
// ================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <PZEM004Tv30.h>

// ── Wi-Fi ────────────────────────────────────────────────────────
#define WIFI_SSID  "NR-FAB"
#define WIFI_PASS  "Nrfab3@2@1@"

// ── Google Sheets ─────────────────────────────────────────────────
// Mesma implantação (Apps Script) já em uso nas versões anteriores.
#define GOOGLE_URL "https://script.google.com/macros/s/AKfycbwxIhNpa8zA5ywH5d7IEnqmanzv73YThyrBfrb8Gm7FbEoFpE_-P7c8VGBIimIxknYW/exec"

// ── MQTT (HiveMQ Cloud) — PREENCHER com os dados reais do seu cluster ──
#define MQTT_BROKER    "14fc5a355dec4054b8609b092c9653d7.s1.eu.hivemq.cloud"
#define MQTT_PORT      8883
#define MQTT_USER      "esp32"
#define MQTT_PASS      "Tccfatec1"
#define MQTT_TOPIC     "tamboreadora/corrente"
#define MQTT_CLIENT_ID "esp32_tamboreadora_01"
// ── PZEM-004T (validado por Gabriel: Serial2, RX=16, TX=17) ──────
PZEM004Tv30 pzem(Serial2, 16, 17);

// ── Clientes de rede ──────────────────────────────────────────────
WiFiClientSecure wifiSheets;
WiFiClientSecure wifiMqtt;
PubSubClient mqttClient(wifiMqtt);

// ── Timing ────────────────────────────────────────────────────────
const unsigned long INTERVALO_LEITURA = 10000;  // 10s entre leituras/envios
unsigned long ultimaLeitura = 0;

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
  wifiMqtt.setInsecure(); // sem validar o certificado do broker (igual ao Sheets)
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  Serial.print("Conectando ao MQTT...");
  int tentativas = 0;
  while (!mqttClient.connected() && tentativas < 10) {
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      Serial.println(" conectado!");
    } else {
      Serial.printf(" falhou (rc=%d). Tentando de novo em 2s...\n", mqttClient.state());
      delay(2000);
      tentativas++;
    }
  }
}

// ================================================================
//   GOOGLE SHEETS — envia os valores brutos do PZEM
// ================================================================
void enviarGoogleSheets(float tensao, float corrente, float potencia,
                         float energia, float frequencia, float pf) {
  if (WiFi.status() != WL_CONNECTED) return;

  wifiSheets.setInsecure();

  HTTPClient http;
  http.begin(wifiSheets, GOOGLE_URL);
  http.addHeader("Content-Type", "application/json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  StaticJsonDocument<256> doc;
  doc["tensao"]     = tensao;
  doc["irms"]       = corrente;
  doc["potencia"]   = potencia;
  doc["energia"]    = energia;
  doc["frequencia"] = frequencia;
  doc["pf"]         = pf;

  char payload[256];
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  Serial.printf("  [Sheets] HTTP %d\n", httpCode);
  http.end();
}

// ================================================================
//   MQTT — publica os mesmos valores
// ================================================================
void publicarMQTT(float tensao, float corrente, float potencia,
                   float energia, float frequencia, float pf) {
  if (!mqttClient.connected()) {
    conectarMQTT();
  }
  mqttClient.loop();

  StaticJsonDocument<256> doc;
  doc["tensao"]     = tensao;
  doc["irms"]       = corrente;
  doc["potencia"]   = potencia;
  doc["energia"]    = energia;
  doc["frequencia"] = frequencia;
  doc["pf"]         = pf;

  char payload[256];
  serializeJson(doc, payload);

  bool ok = mqttClient.publish(MQTT_TOPIC, payload);
  Serial.printf("  [MQTT] publish %s -> %s\n", MQTT_TOPIC, ok ? "OK" : "FALHOU");
}

// ================================================================
//   SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("=====================================");
  Serial.println("  Monitor Maquina — TCC FATEC (v22)");
  Serial.println("  Somente PZEM (sem vibracao) -> Planilha + MQTT");
  Serial.println("=====================================");

  conectarWiFi();
  conectarMQTT();

  Serial.println("\nIniciando leituras do PZEM (a cada 10s)...");
  Serial.println("Tensao(V) | Corrente(A) | Potencia(W) | Energia(kWh) | Freq(Hz) | PF");
}

// ================================================================
//   LOOP
// ================================================================
void loop() {
  unsigned long agora = millis();
  if (agora - ultimaLeitura < INTERVALO_LEITURA && ultimaLeitura != 0) {
    mqttClient.loop(); // mantém a conexão MQTT viva entre os ciclos de 10s
    return;
  }
  ultimaLeitura = agora;

  float tensao     = pzem.voltage();
  float corrente   = pzem.current();
  float potencia   = pzem.power();
  float energia    = pzem.energy();
  float frequencia = pzem.frequency();
  float pf         = pzem.pf();

  if (isnan(tensao) || isnan(corrente) || isnan(potencia) ||
      isnan(energia) || isnan(frequencia) || isnan(pf)) {
    Serial.println("  [AVISO] Leitura do PZEM invalida (NaN) — pulando esse ciclo.");
    return;
  }

  Serial.printf("%9.2f | %11.4f | %11.2f | %12.3f | %8.1f | %.2f\n",
                tensao, corrente, potencia, energia, frequencia, pf);

  enviarGoogleSheets(tensao, corrente, potencia, energia, frequencia, pf);
  publicarMQTT(tensao, corrente, potencia, energia, frequencia, pf);
}
