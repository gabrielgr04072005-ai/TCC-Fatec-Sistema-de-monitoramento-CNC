// ================================================================
//   Monitor de Máquina — TCC FATEC
//   ESP32 + SCT-013-030 + ZMPT101B + MPU6050 + Google Sheets + MQTT
//   v18 — robustez de leitura I2C (MPU6050)
//
//   O que mudou em relação à v17:
//   - lerMPU() agora tenta a leitura até 3 vezes antes de desistir,
//     em vez de aceitar a primeira falha como definitiva.
//   - Checagem do retorno de Wire.endTransmission(): se != 0, já
//     sabemos que a transação falhou sem precisar esperar o
//     Wire.available() para descobrir.
//   - Recuperação de barramento (bus recovery): se as tentativas
//     falharem, força um toggle manual do SCL pra liberar um SDA
//     que ficou "preso" baixo por um escravo travado no meio de uma
//     transação — situação comum com ruído elétrico de motor/inversor.
//   - Wire.setClock(100000): I2C a 100kHz em vez do padrão do ESP32
//     (geralmente 100kHz já, mas fixado explicitamente) — mais
//     tolerante a cabo longo e ruído do que 400kHz.
//   - Quando a leitura falha mesmo após as tentativas, NÃO zera mais
//     vibração/temperatura: mantém o último valor válido lido,
//     evitando contaminar o z-score da vibração com zeros falsos.
//   - Contador de falhas do MPU (total e taxa) — logado no Serial e
//     enviado como coluna extra pra planilha/MQTT. Isso também vira
//     um dado interessante pro TCC: taxa de falha do I2C em ambiente
//     industrial é uma limitação real e mensurável.
//
//   Tudo o mais (classificação combinada, LEDs, z-score, correção
//   geral, envio Sheets/MQTT) é idêntico à v17.
//
//   Pinagem:
//     SCT-013   -> GPIO35 (ADC1)
//     ZMPT101B  -> GPIO34 (ADC1)
//     MPU6050   -> I2C padrão (SDA=GPIO21, SCL=GPIO22)
//     LEDs      -> GPIO32,33,25,26,27,14,12 (SINK, LOW = acende)
//
//   Recomendação de hardware (não resolve por software sozinho):
//   - Confirmar pull-ups externos de 4,7kΩ no SDA e SCL.
//   - Encurtar o cabo do MPU6050 e afastá-lo do cabeamento de
//     potência da fresadora (fonte de EMI mais provável).
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
// IMPORTANTE: aponte para a implantação do Apps Script já atualizada
// (colunas de tensão/vibração/temperatura). Se criar uma implantação
// NOVA (não editar a existente), troque a URL abaixo pela nova.
#define GOOGLE_URL        "https://script.google.com/macros/s/AKfycbxowTRzRHfvT6_0x0U7oIz1T1sNKipHV-6jm3E76VSRFoP_0xHzvbpQd3B1j7cczjWe/exec"
#define GOOGLE_CONFIG_URL "https://script.google.com/macros/s/AKfycbxowTRzRHfvT6_0x0U7olz1T1sNKipHV-6jm3E76VSRFoP_0xHzvbpQd3B1j7cczjWe/exec?action=config"

// ── Hardware — pinos analógicos (ambos em ADC1, compatíveis com Wi-Fi) ──
const int PINO_SCT  = 35;   // SCT-013 (corrente)
const int PINO_ZMPT = 34;   // ZMPT101B (tensão)

// Pinos I2C (usados pela recuperação manual de barramento)
const int PINO_SDA = 21;
const int PINO_SCL = 22;

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
float       fator_calibracao_zmpt = 17;

// ── MPU6050 ───────────────────────────────────────────────────────
const int ENDERECO_MPU = 0x68;
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

// ── Objetos ───────────────────────────────────────────────────────
EnergyMonitor    emon1;
WiFiClientSecure wifiSecure;
PubSubClient     mqttClient(wifiSecure);

// ── Timing do ciclo de leitura ──────────────────────────────────
const unsigned long INTERVALO_CICLO = 5000;  // 5s entre leituras/envios
unsigned long ultimoCiclo = 0;

// ── Estatísticas — CORRENTE (idêntico à v15/v17) ─────────────────
#define JANELA           10
#define MAX_HISTORICO    100
#define INTERVALO_CONFIG 60000  // consulta referência da planilha a cada 1 min
#define TOLERANCIA       0.30   // ±30% do valor de referência (G7)

float  historico[MAX_HISTORICO];
float  janela[JANELA];
int    contJanela        = 0;
int    contTotal         = 0;
float  mediaGlobal       = 0.0;
float  fatorCorrecao     = 0.0;
float  mediaJanela       = 0.0;
float  sigmaJanela       = 0.0;
bool   correcaoAplicada  = false;
bool   primeiraLeitura   = true;
float  referenciaManual  = 0.0;   // valor digitado na planilha (G7)
bool   referenciaRecebida = false;
unsigned long ultimaConsulta = 0;

// ── Estatísticas — VIBRAÇÃO (mesma lógica, aplicada à magnitude) ──
float  historicoVib[MAX_HISTORICO];
float  janelaVib[JANELA];
float  mediaGlobalVib      = 0.0;
float  fatorCorrecaoVib    = 0.0;
float  mediaJanelaVib      = 0.0;
float  sigmaJanelaVib      = 0.0;
bool   correcaoAplicadaVib = false;

// ── Retenção do último valor válido do MPU6050 (v18) ─────────────
// Usado quando uma leitura falha mesmo após as tentativas: em vez de
// zerar vibração/temperatura (o que contaminava o z-score com zeros
// falsos), reaproveita a última leitura boa conhecida.
int16_t ultimoVibXValido = 0;
int16_t ultimoVibYValido = 0;
int16_t ultimoVibZValido = 0;
float   ultimaTemperaturaValida = 0.0;
bool    jaTeveLeituraValidaMPU  = false;

// ── Contadores de falha do MPU6050 (v18) ─────────────────────────
unsigned long totalCiclosMPU  = 0;
unsigned long totalFalhasMPU  = 0;

// ── Controle de LEDs (aceso fixo = confirmado | piscando = parcial) ──
enum ModoLed { LED_OFF, LED_STEADY, LED_BLINK };
ModoLed modoLedAtual = LED_OFF;
int     pinoLedAtual = -1;
bool    blinkAceso = false;
unsigned long ultimoToggleBlink = 0;
const unsigned long INTERVALO_BLINK = 300; // ms

// ================================================================
//   LEDS
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

// Define o pino correspondente a um "tier" de severidade (7 níveis)
int pinoPorTier(String tier) {
  if (tier == "VERMELHO BAIXO") return led_vermelho_baixo;
  if (tier == "LARANJA BAIXO")  return led_laranja_baixo;
  if (tier == "AMARELO BAIXO")  return led_amarelo_baixo;
  if (tier == "AMARELO ALTO")   return led_amarelo_alto;
  if (tier == "LARANJA ALTO")   return led_laranja_alto;
  if (tier == "VERMELHO ALTO")  return led_vermelho_alto;
  return led_verde; // "VERDE - NORMAL" ou desconhecido
}

// Aplica o modo do LED (chamado uma vez por ciclo de leitura)
void aplicarLed(ModoLed modo, int pino) {
  apagarLeds();
  modoLedAtual = modo;
  pinoLedAtual = pino;
  blinkAceso = false;
  if (modo == LED_STEADY) {
    digitalWrite(pino, LOW);
  }
  // LED_BLINK é tratado em atualizarBlinkLeds(), chamado a cada loop()
}

// Faz o LED piscar quando modoLedAtual == LED_BLINK. Precisa ser chamado
// em TODA passada do loop() (não só quando um novo ciclo de leitura
// acontece), senão o pisca-pisca trava durante os 5s de espera.
void atualizarBlinkLeds() {
  if (modoLedAtual != LED_BLINK) return;
  unsigned long agora = millis();
  if (agora - ultimoToggleBlink >= INTERVALO_BLINK) {
    ultimoToggleBlink = agora;
    blinkAceso = !blinkAceso;
    digitalWrite(pinoLedAtual, blinkAceso ? LOW : HIGH);
  }
}

// ================================================================
//   CLASSIFICAÇÃO ESTATÍSTICA (genérica — usada por corrente e vibração)
// ================================================================
float calcZScoreGenerico(float valor, float media, float sigma, float correcao) {
  if (sigma == 0.0) return 0.0;
  float centro = media + correcao;
  return (valor - centro) / sigma;
}

// z-score da corrente: usa a referência manual da planilha (G7) como
// centro quando disponível, senão usa média da janela + correção geral.
float calcZScoreCorrente(float irms) {
  if (sigmaJanela == 0.0) return 0.0;
  float centro = referenciaRecebida ? referenciaManual : (mediaJanela + fatorCorrecao);
  return (irms - centro) / sigmaJanela;
}

float calcZScoreVibracao(float magnitude) {
  return calcZScoreGenerico(magnitude, mediaJanelaVib, sigmaJanelaVib, fatorCorrecaoVib);
}

// Classifica um z-score em um dos 7 "tiers" de severidade (mesmas faixas
// usadas desde a v15).
String classificarTier(float z) {
  if      (z < -3.0) return "VERMELHO BAIXO";
  else if (z < -2.0) return "LARANJA BAIXO";
  else if (z < -1.0) return "AMARELO BAIXO";
  else if (z <=  1.0) return "VERDE - NORMAL";
  else if (z <=  2.0) return "AMARELO ALTO";
  else if (z <=  3.0) return "LARANJA ALTO";
  else                return "VERMELHO ALTO";
}

String obterEstadoPorTier(String tier) {
  if (tier == "VERMELHO BAIXO") return "Abaixo do Normal";
  if (tier == "LARANJA BAIXO")  return "Abaixo - Atencao";
  if (tier == "AMARELO BAIXO")  return "Levemente Baixo";
  if (tier == "VERDE - NORMAL") return "Normal";
  if (tier == "AMARELO ALTO")   return "Levemente Alto";
  if (tier == "LARANJA ALTO")   return "Acima - Atencao";
  return "Acima do Normal"; // VERMELHO ALTO
}

// ── Estatísticas da janela (média e desvio padrão) — genérica ────
void calcularEstatisticasJanela(float* jan, float* media, float* sigma, const char* rotulo) {
  float soma = 0.0;
  for (int i = 0; i < JANELA; i++) soma += jan[i];
  *media = soma / JANELA;

  float somaSq = 0.0;
  for (int i = 0; i < JANELA; i++) {
    float diff = jan[i] - (*media);
    somaSq += diff * diff;
  }
  *sigma = sqrt(somaSq / JANELA);

  Serial.printf("  [Estatisticas %s] mu=%.4f  sigma=%.4f\n", rotulo, *media, *sigma);
}

// ── Ordena array (Bubble Sort simples) ───────────────────────────
void ordenar(float* arr, int n) {
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (arr[j] > arr[j+1]) {
        float tmp = arr[j]; arr[j] = arr[j+1]; arr[j+1] = tmp;
      }
}

// ── Correção geral após 100 leituras (Mediana + MAD) — genérica ──
void calcularCorrecaoGeral(float* hist, float* mediaGlobalOut, float* fatorCorrecaoOut, const char* rotulo) {
  float copia[MAX_HISTORICO];
  for (int i = 0; i < MAX_HISTORICO; i++) copia[i] = hist[i];
  ordenar(copia, MAX_HISTORICO);

  float mediana = (copia[49] + copia[50]) / 2.0;

  float desvios[MAX_HISTORICO];
  for (int i = 0; i < MAX_HISTORICO; i++)
    desvios[i] = abs(hist[i] - mediana);
  ordenar(desvios, MAX_HISTORICO);
  float mad = (desvios[49] + desvios[50]) / 2.0;
  float sigmaRobusto = mad * 1.4826;

  float somaValidos = 0.0;
  int   countValidos = 0;
  for (int i = 0; i < MAX_HISTORICO; i++) {
    float zRobusto = (sigmaRobusto > 0) ? abs(hist[i] - mediana) / sigmaRobusto : 0;
    if (zRobusto <= 2.0) {
      somaValidos += hist[i];
      countValidos++;
    }
  }

  *mediaGlobalOut = (countValidos > 0) ? somaValidos / countValidos : mediana;

  float somaIniciais = 0.0;
  int   countIniciais = 0;
  for (int i = 1; i <= JANELA; i++) {
    float zRobusto = (sigmaRobusto > 0) ? abs(hist[i] - mediana) / sigmaRobusto : 0;
    if (zRobusto <= 2.0) {
      somaIniciais += hist[i];
      countIniciais++;
    }
  }
  float mediaIniciais = (countIniciais > 0) ? somaIniciais / countIniciais : mediana;

  *fatorCorrecaoOut = *mediaGlobalOut - mediaIniciais;

  Serial.printf("  [Correcao Geral %s] mediaGlobal=%.4f (%d validos)  correcao=%.4f\n",
                rotulo, *mediaGlobalOut, countValidos, *fatorCorrecaoOut);
}

// ================================================================
//   CONSULTA CONFIGURAÇÃO NA PLANILHA (a cada 1 minuto)
// ================================================================
void consultarConfiguracao() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure clienteHTTPS;
  clienteHTTPS.setInsecure();

  HTTPClient http;
  http.begin(clienteHTTPS, GOOGLE_CONFIG_URL);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpCode = http.GET();

  if (httpCode == 200) {
    String resposta = http.getString();
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, resposta);
    if (!err && doc["status"] == "OK") {
      referenciaManual = doc["referencia"];
      referenciaRecebida = true;
      Serial.printf("  [Config] Referencia recebida: %.4f A\n", referenciaManual);
    }
  } else {
    Serial.printf("  [Config] Falha ao consultar (HTTP %d)\n", httpCode);
  }

  http.end();
}

// ── Verifica se leitura é válida comparando com referência ────────
// Retorna: 0 = válida, -1 = máquina desligada, 1 = anomalia alta
int verificarLeitura(float irms) {
  if (!referenciaRecebida || referenciaManual == 0.0) return 0;

  float limite_baixo = referenciaManual * (1.0 - TOLERANCIA);
  float limite_alto  = referenciaManual * (1.0 + TOLERANCIA);

  if (irms < limite_baixo) return -1;
  if (irms > limite_alto)  return  1;
  return 0;
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
// ================================================================
float lerTensao() {
  float soma = 0;
  for (int i = 0; i < AMOSTRAS_ZMPT; i++) {
    soma += analogRead(PINO_ZMPT);
  }
  float offset = soma / AMOSTRAS_ZMPT;

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
//   I2C — RECUPERAÇÃO DE BARRAMENTO (v18)
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
//
//   v18: tenta até MPU_TENTATIVAS vezes antes de desistir. Se todas
//   falharem, aciona a recuperação de barramento. O chamador (loop())
//   decide o que fazer quando l.valida == false — na v18 isso significa
//   "reaproveitar o último valor bom", não mais "zerar".
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
                   int16_t vibZ, float temperatura, String nivel, String estado,
                   bool mpuValidaNesseCiclo) {
  if (!mqttClient.connected()) conectarMQTT();
  mqttClient.loop();

  StaticJsonDocument<288> doc;
  doc["irms"]        = (float)round(irms * 10000.0) / 10000.0;
  doc["tensao"]       = (float)round(tensao * 100.0) / 100.0;
  doc["vibX"]         = vibX;
  doc["vibY"]         = vibY;
  doc["vibZ"]         = vibZ;
  doc["temperatura"]  = (float)round(temperatura * 100.0) / 100.0;
  doc["nivel"]        = nivel;
  doc["estado"]       = estado;
  doc["mpu_ok"]       = mpuValidaNesseCiclo; // v18: sinaliza se essa leitura de vib/temp é fresca ou reaproveitada

  char payload[288];
  serializeJson(doc, payload);

  bool ok = mqttClient.publish(MQTT_TOPIC, payload, false);
  Serial.printf("  [MQTT] %s\n", ok ? "OK" : "ERRO");
}

// ================================================================
//   GOOGLE SHEETS
// ================================================================
void enviarGoogleSheets(float irms, float tensao, int16_t vibX, int16_t vibY,
                         int16_t vibZ, float temperatura, String nivel, String estado,
                         bool mpuValidaNesseCiclo) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure clienteHTTPS;
  clienteHTTPS.setInsecure();

  HTTPClient http;
  http.begin(clienteHTTPS, GOOGLE_URL);
  http.addHeader("Content-Type", "application/json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000); // 15s — evita timeout quando o Apps Script demora

  StaticJsonDocument<288> doc;
  doc["irms"]        = (float)round(irms * 10000.0) / 10000.0;
  doc["tensao"]       = (float)round(tensao * 100.0) / 100.0;
  doc["vibX"]         = vibX;
  doc["vibY"]         = vibY;
  doc["vibZ"]         = vibZ;
  doc["temperatura"]  = (float)round(temperatura * 100.0) / 100.0;
  doc["nivel"]        = nivel;
  doc["estado"]       = estado;
  doc["mpu_ok"]       = mpuValidaNesseCiclo; // v18: coluna extra pro Apps Script, se quiser logar

  char payload[288];
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
  Serial.println("  Monitor Maquina — TCC FATEC (v18)");
  Serial.println("  Robustez de leitura I2C (MPU6050)");
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

  // I2C — MPU6050 (v18: clock fixado em 100kHz, mais tolerante a
  // cabo longo e ruído do que os 400kHz "fast mode")
  Wire.begin();
  Wire.setClock(100000);
  Wire.beginTransmission(ENDERECO_MPU);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission(true);
  delay(100);
  Serial.println("MPU6050 iniciado");

  // Conecta Wi-Fi e MQTT
  conectarWiFi();
  conectarMQTT();

  Serial.println("\nIniciando monitoramento...");
  Serial.println("Leit |  Corrente(A) | Tensao(V) | VibX  | VibY  | VibZ  | Temp(C) | Nivel");
  Serial.println("-----+--------------+-----------+-------+-------+-------+---------+------------------");
}

// ================================================================
//   LOOP
// ================================================================
void loop() {
  // Mantém o LED piscando mesmo enquanto espera o próximo ciclo de leitura.
  atualizarBlinkLeds();

  // Consulta referência da planilha a cada 1 minuto (independente do
  // ciclo de leitura dos sensores).
  unsigned long agoraConfig = millis();
  if (agoraConfig - ultimaConsulta >= INTERVALO_CONFIG || ultimaConsulta == 0) {
    ultimaConsulta = agoraConfig;
    consultarConfiguracao();
  }

  unsigned long agora = millis();
  if (agora - ultimoCiclo < INTERVALO_CICLO && ultimoCiclo != 0) {
    return;
  }
  ultimoCiclo = agora;
  contTotal++;

  // ── Leituras (mesmo ciclo, mesmo timestamp) ──────────────────
  float irms = lerCorrente();
  float tensao = lerTensao();
  LeituraMPU mpu = lerMPU();

  totalCiclosMPU++;

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
    // (o que fabricava anomalias falsas no z-score), reaproveita o
    // último valor válido conhecido.
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

  float vibMag = sqrt((float)vibX*vibX + (float)vibY*vibY + (float)vibZ*vibZ);

  // ── Fase 1: ignora primeira leitura (pico inicial) ──────────
  if (primeiraLeitura) {
    primeiraLeitura = false;
    Serial.printf("[  1]  %.4f A  | Ignorada (pico inicial)\n", irms);
    aplicarLed(LED_STEADY, led_verde);
    enviarGoogleSheets(irms, tensao, vibX, vibY, vibZ, temperatura, "CALIBRANDO", "Calibrando sistema...", mpu.valida);
    publicarMQTT(irms, tensao, vibX, vibY, vibZ, temperatura, "CALIBRANDO", "Calibrando sistema...", mpu.valida);
    return;
  }

  // ── Verifica leitura de corrente contra referência da planilha ──
  int statusLeitura = verificarLeitura(irms);

  if (statusLeitura == -1) {
    Serial.printf("[%4d]  %.4f A  | MAQUINA DESLIGADA\n", contTotal, irms);
    aplicarLed(LED_OFF, -1);
    publicarMQTT(irms, tensao, vibX, vibY, vibZ, temperatura, "MAQUINA DESLIGADA", "Maquina nao esta operando", mpu.valida);
    enviarGoogleSheets(irms, tensao, vibX, vibY, vibZ, temperatura, "MAQUINA DESLIGADA", "Maquina nao esta operando", mpu.valida);
    return;
  }

  if (statusLeitura == 1) {
    Serial.printf("[%4d]  %.4f A  | ANOMALIA ALTA (referencia) — descartado da calibracao\n", contTotal, irms);
    aplicarLed(LED_STEADY, led_vermelho_alto);
    publicarMQTT(irms, tensao, vibX, vibY, vibZ, temperatura, "VERMELHO ALTO", "Acima do Normal (referencia)", mpu.valida);
    enviarGoogleSheets(irms, tensao, vibX, vibY, vibZ, temperatura, "VERMELHO ALTO", "Acima do Normal (referencia)", mpu.valida);
    return;
  }

  // ── Salva no histórico geral (corrente e vibração) ───────────
  if (contTotal - 1 < MAX_HISTORICO) {
    historico[contTotal - 1]    = irms;
    historicoVib[contTotal - 1] = vibMag;
  }

  // ── Correção geral após 100 leituras (mediana + MAD) ─────────
  if (contTotal - 1 == MAX_HISTORICO) {
    if (!correcaoAplicada) {
      calcularCorrecaoGeral(historico, &mediaGlobal, &fatorCorrecao, "Corrente");
      correcaoAplicada = true;
    }
    if (!correcaoAplicadaVib) {
      calcularCorrecaoGeral(historicoVib, &mediaGlobalVib, &fatorCorrecaoVib, "Vibracao");
      correcaoAplicadaVib = true;
    }
  }

  // ── Adiciona à janela atual ────────────────────────────────────
  janela[contJanela]    = irms;
  janelaVib[contJanela] = vibMag;
  contJanela++;

  if (contJanela >= JANELA) {
    calcularEstatisticasJanela(janela,    &mediaJanela,    &sigmaJanela,    "Corrente");
    calcularEstatisticasJanela(janelaVib, &mediaJanelaVib, &sigmaJanelaVib, "Vibracao");
    contJanela = 0;
  }

  // ── Classificação individual ──────────────────────────────────
  String nivel, estado;
  ModoLed modoLed = LED_OFF;
  int pinoLed = led_verde;

  if (contTotal < JANELA + 1) {
    // Ainda não tem estatística suficiente pra classificar
    nivel  = "CALIBRANDO";
    estado = "Calibrando sistema...";
    modoLed = LED_STEADY;
    pinoLed = led_verde;
  } else {
    float zCorrente = calcZScoreCorrente(irms);
    float zVibracao = calcZScoreVibracao(vibMag);

    String tierCorrente = classificarTier(zCorrente);
    String tierVibracao = classificarTier(zVibracao);

    bool anomaloCorrente = (tierCorrente != "VERDE - NORMAL");
    bool anomaloVibracao = (tierVibracao != "VERDE - NORMAL");

    if (!anomaloCorrente && !anomaloVibracao) {
      // Tudo normal
      nivel  = "VERDE - NORMAL";
      estado = "Normal";
      modoLed = LED_STEADY;
      pinoLed = led_verde;

    } else if (anomaloCorrente && anomaloVibracao) {
      // CONFIRMADO — os dois sinais concordam. Usa a severidade da
      // corrente (mais calibrada historicamente) para o nível/LED.
      nivel  = tierCorrente;
      estado = obterEstadoPorTier(tierCorrente) + " (confirmado por vibracao)";
      modoLed = LED_STEADY;
      pinoLed = pinoPorTier(tierCorrente);

    } else if (anomaloCorrente) {
      // Só a corrente saiu do normal — parcial
      nivel  = "ATENCAO PARCIAL";
      estado = "Corrente indica " + tierCorrente + " - vibracao normal (verificar)";
      modoLed = LED_BLINK;
      pinoLed = pinoPorTier(tierCorrente);

    } else {
      // Só a vibração saiu do normal — parcial
      nivel  = "ATENCAO PARCIAL";
      estado = "Vibracao indica " + tierVibracao + " - corrente normal (verificar)";
      modoLed = LED_BLINK;
      pinoLed = pinoPorTier(tierVibracao);
    }

    Serial.printf("  [Z-score] Corrente=%.2f (%s)  Vibracao=%.2f (%s)\n",
                  zCorrente, tierCorrente.c_str(), zVibracao, tierVibracao.c_str());
  }

  aplicarLed(modoLed, pinoLed);

  // ── Serial ───────────────────────────────────────────────────
  Serial.printf("[%4d]  %.4f A | %.2f V | %6d | %6d | %6d | %.2f C | %s%s\n",
                contTotal, irms, tensao, vibX, vibY, vibZ, temperatura, nivel.c_str(),
                mpu.valida ? "" : "  (MPU reaproveitado)");

  // ── MQTT ─────────────────────────────────────────────────────
  publicarMQTT(irms, tensao, vibX, vibY, vibZ, temperatura, nivel, estado, mpu.valida);

  // ── Google Sheets ────────────────────────────────────────────
  enviarGoogleSheets(irms, tensao, vibX, vibY, vibZ, temperatura, nivel, estado, mpu.valida);
}
