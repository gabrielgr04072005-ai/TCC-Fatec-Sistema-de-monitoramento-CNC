// ================================================================
//   Monitor de Máquina — TCC FATEC
//   ESP32 + PZEM-004T v3.0 — v25  (v24 + pesos do treino_mlp_v5.py)
//
//   O que mudou na v25 (em relação à v24):
//   - Entradas do MLP agora sao as do treino_mlp_v5.py:
//       corrente, tensao, variacao, media10, desvio10, zscore
//     (potencia e pf SAIRAM da rede; continuam sendo enviadas a planilha).
//     zscore = (corrente - media10) / desvio10, e 0 se desvio10 <= 1e-6.
//   - Pesos, bias, media e desvio do treino ja colados abaixo.
//   - Limites fisicos iguais ao v5: corrente 0 a 50 A, tensao 80 a 300 V.
//
//   O que mudou na v24 (em relação à v23):
//   - 7 LEDs (GPIO 32,33,25,26,27,14,12 — ativos em LOW) mostram o
//     nivel da maquina, um LED aceso por vez:
//        LED 1  PARADA      (modelo: classe PARADA, corrente ~0 A)
//        LED 2  VAZIO       (corrente < 3,0 A: partida / rampa)
//        LED 3  VAZIO       (corrente de 3,0 A ate 4,0 A: girando em vazio)
//        LED 4  USINANDO    (1o terco entre 4,0 A e 5,3 A)
//        LED 5  USINANDO    (2o terco: esforco maior)
//        LED 6  USINANDO    (3o terco: esforco forcado)
//        LED 7  SOBRECARGA  (corrente >= 5,3 A, limiar fixo fora da rede)
//     A CLASSE (PARADA / VAZIO / USINANDO) vem do MLP; a corrente so
//     escolhe qual LED dentro de VAZIO e de USINANDO.
//   - Filtro anti-oscilacao: o LED so muda depois que o novo nivel se
//     repete CONFIRMAR_NIVEL leituras seguidas (3 x 10 s = 30 s).
//     SOBRECARGA precisa de CONFIRMAR_SOBRECARGA leituras (2).
//     Ajuste as duas constantes na secao LOGICA se quiser.
//   - Leitura e MLP continuam a cada 10 s (mesmo ritmo do treino).
//     MQTT publica cada leitura na hora; a PLANILHA recebe em lote a
//     cada LOTE_N leituras (3 x 10 s = 30 s).
//   - Campos novos no envio: "nivel" (1 a 7), "estado", "confianca".
//
//   ENVIO PARA A PLANILHA — SHEETS_LOTE_UM_POST:
//     0 = modo compativel (padrao): a cada 30 s manda as 3 leituras em
//         3 POSTs seguidos, no mesmo formato da v22. Funciona com o
//         Apps Script atual, mas as 3 linhas chegam com quase a mesma
//         hora de recebimento.
//     1 = um unico POST {"lote":[{...},{...},{...}]}, cada leitura com
//         "atraso_s" (segundos antes do envio). EXIGE um Apps Script
//         novo (codigo_google_sheets_vN.gs) que aceite "lote".
//
//   COMO COLOCAR OS PESOS (quando treinar de novo com o treino_mlp_v5.py):
//     1) Feche o grafico para o script imprimir "PESOS E BIAS FINAIS".
//     2) Cole o 1o bloco impresso ("Variables Red Neuronal") no lugar
//        do bloco  PESOS_INICIO ... PESOS_FIM  logo abaixo, e o 2o
//        bloco ("Preprocesamiento") no lugar do bloco de normalizacao.
//     3) NAO cole o 3o bloco ("Estructura Red Neuronal"): ele ja esta
//        pronto na funcao inferir().
//     Enquanto os pesos estiverem zerados os LEDs ficam apagados
//     (estado SEM_PESOS), exceto SOBRECARGA.
//
//   Biblioteca necessária (Gerenciador de Bibliotecas do Arduino IDE):
//     - "PZEM004Tv30" (Olehs/Jakub Mandula)
//     - "PubSubClient" (Nick O'Leary) — pro MQTT
//     - "ArduinoJson"
//
//   Pinagem:
//     PZEM-004T -> Serial2 do ESP32 (RX=GPIO16, TX=GPIO17)
//     LEDs      -> GPIO 32, 33, 25, 26, 27, 14, 12 (acendem em LOW)
// ================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <PZEM004Tv30.h>
#include <math.h>

// ── Wi-Fi ────────────────────────────────────────────────────────
#define WIFI_SSID  "NR-FAB"
#define WIFI_PASS  "Nrfab3@2@1@"

// ── Google Sheets ─────────────────────────────────────────────────
// Mesma implantação (Apps Script) já em uso nas versões anteriores.
#define GOOGLE_URL "https://script.google.com/macros/s/AKfycbz0OakPAa2PahjdD-9HPya2lyiDD0Lm1vzzqrnNXFlhzHMgoFkuHDtvfzDGSXoydlVB/exec"

// ── MQTT (HiveMQ Cloud) ───────────────────────────────────────────
#define MQTT_BROKER    "14fc5a355dec4054b8609b092c9653d7.s1.eu.hivemq.cloud"
#define MQTT_PORT      8883
#define MQTT_USER      "esp32"
#define MQTT_PASS      "Tccfatec1"
#define MQTT_TOPIC     "tamboreadora/corrente"
#define MQTT_CLIENT_ID "esp32_tamboreadora_01"

// ── Envio em lote para a planilha ─────────────────────────────────
#define LOTE_N               3   // leituras por envio (3 x 10 s = 30 s)
#define LOTE_MAX             12  // se o envio falhar, acumula ate aqui
#define SHEETS_LOTE_UM_POST  0   // 0 = compativel com o .gs atual | 1 = lote em 1 POST (precisa do .gs novo)

// Struct ANTES de qualquer funcao (regra do Arduino IDE)
struct Leitura {
  float tensao;
  float corrente;
  float potencia;
  float energia;
  float frequencia;
  float pf;
  float confianca;
  int nivel;
  const char* estado;
  unsigned long ms;
};

Leitura lote[LOTE_MAX];
int nLote = 0;

// ── LEDs (ativos em LOW / sink) ───────────────────────────────────
const int LED_PINS[7] = {32, 33, 25, 26, 27, 14, 12};
#define LED_LIGADO     LOW
#define LED_DESLIGADO  HIGH

// ── PZEM-004T (validado por Gabriel: Serial2, RX=16, TX=17) ──────
PZEM004Tv30 pzem(Serial2, 16, 17);

// ── Clientes de rede ──────────────────────────────────────────────
WiFiClientSecure wifiSheets;
WiFiClientSecure wifiMqtt;
PubSubClient mqttClient(wifiMqtt);

// ── Timing ────────────────────────────────────────────────────────
const unsigned long INTERVALO_LEITURA = 10000;  // 10s entre leituras
unsigned long ultimaLeitura = 0;

// ── Limites do treino (mantenha iguais ao treino_mlp_v5.py) ───────
const double CORRENTE_MAX_A = 50.0;
const double TENSAO_MIN_V   = 80.0;
const double TENSAO_MAX_V   = 300.0;
const unsigned long GAP_MS  = 300000UL;   // GAP_SEG = 300 s
unsigned long ultimaValidaMs = 0;
bool temUltimaValida = false;

// ================================================================
//   PESOS E BIAS  —  saida do treino_mlp_v5.py (ja colados)
//   (Enquanto estiver tudo zerado, o estado fica SEM_PESOS)
// ================================================================
// ==== PESOS_INICIO ====
///////////////////// Variables Red Neuronal (MLP 6-8-4-3) /////////////////////
// Entradas, nesta ordem: corrente, tensao, variacao, media10, desvio10, zscore.
//   variacao = corrente atual - corrente anterior
//   media10  = media das ultimas 10 correntes
//   desvio10 = desvio-padrao populacional das ultimas 10 correntes
//   zscore   = (corrente - media10) / desvio10, e 0 se desvio10 < 1e-06
// Saidas: 0 = PARADA, 1 = VAZIO, 2 = USINANDO
double a0[6];
double W1[8][6] = {{-1.1239,-0.1814,0.0643,-0.3354,0.2175,-0.1592},{-0.6019,-0.3132,0.1882,0.3507,0.1533,-0.3025},{-0.4417,0.4080,0.3313,-1.4692,-1.3293,-0.2393},{1.8534,-0.5599,0.5100,0.5076,0.7935,0.4515},{-1.2818,-0.1229,0.8417,-0.2339,-0.6958,-0.3056},{-0.6609,-0.0675,0.1255,0.0336,0.4657,0.7425},{0.0365,0.1919,-0.2498,-0.3536,-0.2133,-0.0839},{-1.3721,-0.3580,0.1166,-0.7767,0.3144,-0.7142}};
double a1[8];
double b1[8] = {1.4981,1.7172,0.9787,-0.8519,0.4437,2.0167,-0.2860,0.9659};
double W2[4][8] = {{-0.7426,-0.6747,0.0831,1.8118,-0.2456,-0.4057,-0.3115,-0.7789},{-1.1358,1.3128,-0.3644,-0.4196,0.2031,0.7552,-0.3081,-1.0834},{3.1144,-1.1229,1.6540,0.7178,-0.6560,-0.6283,0.0523,2.0762},{0.6496,1.0852,0.2588,-0.6164,1.1505,1.4212,-0.3165,0.9339}};
double a2[4];
double b2[4] = {-0.8322,1.4853,-1.0066,1.6525};
double W3[3][4] = {{-2.0776,-2.4664,0.9838,1.2247},{-0.1823,1.8490,-3.8916,2.7078},{1.0177,-1.7124,3.4172,-3.0184}};
double a3[3];
double b3[3] = {0.1983,0.7387,-1.3211};
double aux = 0.0;
// ==== PESOS_FIM ====

// ==== NORMALIZACAO_INICIO ====
///////////////////// Preprocesamiento (z-score do treino) /////////////////////
// depois do z-score, limitar cada entrada a [-8, +8]
double mean[6] = {0.6556,224.9126,-0.0085,0.6715,0.1652,-0.0113};
double dstd[6] = {1.4409,2.7783,0.4678,1.3707,0.4708,0.5663};
// ==== NORMALIZACAO_FIM ====

// ================================================================
//   LOGICA DO MLP + NIVEL DOS LEDs (so C++ padrao: da pra testar no PC)
// ================================================================
// ==== LOGICA_MLP_INICIO ====
#define JANELA 10
static const double CLIP_Z = 8.0;
static const double LIM_USINANDO_A     = 4.0;   // a partir daqui: USINANDO
static const double LIM_SOBRECARGA_A   = 5.3;   // a partir daqui: SOBRECARGA
static const double LIM_VAZIO_LED_A    = 3.0;   // VAZIO: abaixo = LED 2, acima = LED 3
static const int CONFIRMAR_NIVEL       = 3;     // leituras seguidas para trocar de LED
static const int CONFIRMAR_SOBRECARGA  = 2;     // leituras seguidas para acender o LED 7
static const char* NOMES_CLASSE[3] = {"PARADA", "VAZIO", "USINANDO"};

static double bufCorrente[JANELA];
static int    bufIdx = 0;
static int    bufN = 0;
static double correnteAnterior = 0.0;
static bool   temAnterior = false;

// filtro anti-oscilacao dos LEDs
static int nivelExibido = 0;      // 0 = nenhum LED
static int nivelCandidato = 0;
static int contCandidato = 0;

static double relu(double x) { return x > 0.0 ? x : 0.0; }

void resetarFiltro() {
  nivelExibido = 0;
  nivelCandidato = 0;
  contCandidato = 0;
}

void resetarJanela() {
  bufIdx = 0;
  bufN = 0;
  temAnterior = false;
  resetarFiltro();
}

// Atualiza a janela com a leitura atual e monta as 6 entradas (cruas, antes
// do z-score da normalizacao): corrente, tensao, variacao, media10, desvio10,
// zscore. Retorna false enquanto nao houver JANELA leituras.
bool montarEntradas(double corrente, double tensao, double *ent) {
  double variacao = temAnterior ? (corrente - correnteAnterior) : 0.0;
  correnteAnterior = corrente;
  temAnterior = true;

  bufCorrente[bufIdx] = corrente;
  bufIdx = (bufIdx + 1) % JANELA;
  if (bufN < JANELA) bufN++;
  if (bufN < JANELA) return false;

  double soma = 0.0;
  for (int i = 0; i < JANELA; i++) soma += bufCorrente[i];
  double media = soma / JANELA;

  double sq = 0.0;
  for (int i = 0; i < JANELA; i++) {
    double d = bufCorrente[i] - media;
    sq += d * d;
  }
  double desvio = sqrt(sq / JANELA);   // populacional (ddof=0), como no treino

  // z-score local da corrente; janela sem variacao => 0 (EPS_DESVIO = 1e-6 do treino)
  double zscore = (desvio > 1e-6) ? (corrente - media) / desvio : 0.0;

  ent[0] = corrente;
  ent[1] = tensao;
  ent[2] = variacao;
  ent[3] = media;
  ent[4] = desvio;
  ent[5] = zscore;
  return true;
}

// Falso enquanto os pesos forem os zeros de exemplo.
bool pesosCarregados() {
  double s = 0.0;
  for (int i = 0; i < 8; i++)
    for (int j = 0; j < 6; j++) s += fabs(W1[i][j]);
  return s > 0.0;
}

// Roda o MLP. Devolve o indice da classe (0..2) e preenche probs[3].
int inferir(const double *entradas, double *probs) {
  for (int j = 0; j < 6; j++) {
    double z = (entradas[j] - mean[j]) / dstd[j];
    if (z > CLIP_Z) z = CLIP_Z;
    if (z < -CLIP_Z) z = -CLIP_Z;
    a0[j] = z;
  }

  for (int i = 0; i < 8; i++) { aux = 0.0; for (int j = 0; j < 6; j++) { aux = aux + W1[i][j] * a0[j]; } a1[i] = relu(aux + b1[i]); }
  for (int i = 0; i < 4; i++) { aux = 0.0; for (int j = 0; j < 8; j++) { aux = aux + W2[i][j] * a1[j]; } a2[i] = relu(aux + b2[i]); }
  for (int i = 0; i < 3; i++) { aux = 0.0; for (int j = 0; j < 4; j++) { aux = aux + W3[i][j] * a2[j]; } a3[i] = aux + b3[i]; }

  // softmax (subtrai o maior valor so por estabilidade numerica)
  double maximo = a3[0];
  for (int i = 1; i < 3; i++) if (a3[i] > maximo) maximo = a3[i];
  double soma = 0.0;
  for (int i = 0; i < 3; i++) { a3[i] = exp(a3[i] - maximo); soma += a3[i]; }

  int classe = 0;
  double melhor = -1.0;
  for (int i = 0; i < 3; i++) {
    probs[i] = a3[i] / soma;
    if (probs[i] > melhor) { melhor = probs[i]; classe = i; }
  }
  return classe;
}

// Nivel INSTANTANEO (1 a 7) a partir da classe do MLP e da corrente.
int nivelDaClasse(int classe, double corrente) {
  if (corrente >= LIM_SOBRECARGA_A) return 7;
  if (classe == 0) return 1;
  if (classe == 1) return (corrente < LIM_VAZIO_LED_A) ? 2 : 3;
  double passo = (LIM_SOBRECARGA_A - LIM_USINANDO_A) / 3.0;
  if (corrente < LIM_USINANDO_A + passo) return 4;
  if (corrente < LIM_USINANDO_A + 2.0 * passo) return 5;
  return 6;
}

// Filtro anti-oscilacao: devolve o nivel que deve ficar nos LEDs.
// O novo nivel so assume depois de se repetir N leituras seguidas.
int filtrarNivel(int inst) {
  if (nivelExibido == 0) {          // primeira leitura valida: mostra direto
    nivelExibido = inst;
    nivelCandidato = 0;
    contCandidato = 0;
    return nivelExibido;
  }
  if (inst == nivelExibido) {
    nivelCandidato = 0;
    contCandidato = 0;
    return nivelExibido;
  }
  if (inst == nivelCandidato) {
    contCandidato++;
  } else {
    nivelCandidato = inst;
    contCandidato = 1;
  }
  int necessario = (inst == 7) ? CONFIRMAR_SOBRECARGA : CONFIRMAR_NIVEL;
  if (contCandidato >= necessario) {
    nivelExibido = inst;
    nivelCandidato = 0;
    contCandidato = 0;
  }
  return nivelExibido;
}

int nivelAtual() { return nivelExibido; }

const char* estadoDoNivel(int nivel) {
  if (nivel == 1) return "PARADA";
  if (nivel == 2 || nivel == 3) return "VAZIO";
  if (nivel >= 4 && nivel <= 6) return "USINANDO";
  if (nivel == 7) return "SOBRECARGA";
  return "";
}
// ==== LOGICA_MLP_FIM ====

// ================================================================
//   LEDs
// ================================================================
void atualizarLeds(int nivel) {
  for (int i = 0; i < 7; i++) {
    digitalWrite(LED_PINS[i], (i == nivel - 1) ? LED_LIGADO : LED_DESLIGADO);
  }
}

void testeLeds() {   // varredura rapida no boot para conferir a fiacao
  for (int i = 0; i < 7; i++) {
    atualizarLeds(i + 1);
    delay(150);
  }
  atualizarLeds(0);
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
//   GOOGLE SHEETS — envio em lote
// ================================================================
// Preenche os campos de uma leitura (mesmos nomes da v22 + nivel/estado/confianca)
void preencherItem(JsonObject o, const Leitura &l) {
  o["tensao"]     = l.tensao;
  o["irms"]       = l.corrente;
  o["potencia"]   = l.potencia;
  o["energia"]    = l.energia;
  o["frequencia"] = l.frequencia;
  o["pf"]         = l.pf;
  o["nivel"]      = l.nivel;
  o["estado"]     = l.estado;
  o["confianca"]  = l.confianca;
}

bool postarJson(const char* payload) {
  wifiSheets.setInsecure();

  HTTPClient http;
  http.begin(wifiSheets, GOOGLE_URL);
  http.addHeader("Content-Type", "application/json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int httpCode = http.POST((uint8_t*)payload, strlen(payload));
  Serial.printf("  [Sheets] HTTP %d\n", httpCode);
  http.end();
  return httpCode >= 200 && httpCode < 400;
}

// Modo compativel: uma leitura por POST, igual a v22.
bool enviarUmaLeitura(const Leitura &l) {
  StaticJsonDocument<384> doc;
  preencherItem(doc.to<JsonObject>(), l);
  char payload[384];
  serializeJson(doc, payload, sizeof(payload));
  return postarJson(payload);
}

// Modo lote: todas as leituras guardadas em um unico POST.
bool enviarLoteUmPost() {
  static StaticJsonDocument<3072> doc;   // static: nao pesa na pilha
  static char payload[2560];
  doc.clear();
  JsonArray arr = doc.createNestedArray("lote");
  unsigned long agora = millis();
  for (int i = 0; i < nLote; i++) {
    JsonObject o = arr.createNestedObject();
    preencherItem(o, lote[i]);
    o["atraso_s"] = (agora - lote[i].ms) / 1000UL;   // segundos antes do envio
  }
  serializeJson(doc, payload, sizeof(payload));
  return postarJson(payload);
}

void guardarNoLote(const Leitura &l) {
  if (nLote >= LOTE_MAX) {   // buffer cheio (rede fora do ar): descarta a mais antiga
    for (int i = 1; i < LOTE_MAX; i++) lote[i - 1] = lote[i];
    nLote = LOTE_MAX - 1;
  }
  lote[nLote++] = l;
}

void enviarLoteSheets() {
  if (nLote < LOTE_N) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("  [Sheets] sem Wi-Fi — %d leituras guardadas\n", nLote);
    return;
  }
#if SHEETS_LOTE_UM_POST
  Serial.printf("  [Sheets] enviando lote de %d leituras (1 POST)\n", nLote);
  if (enviarLoteUmPost()) {
    nLote = 0;
  } else {
    Serial.printf("  [Sheets] falhou — %d leituras guardadas para a proxima tentativa\n", nLote);
  }
#else
  Serial.printf("  [Sheets] enviando %d leituras (modo compativel)\n", nLote);
  for (int i = 0; i < nLote; i++) enviarUmaLeitura(lote[i]);
  nLote = 0;
#endif
}

// ================================================================
//   MQTT — publica cada leitura na hora
// ================================================================
void publicarMQTT(const Leitura &l) {
  if (!mqttClient.connected()) {
    conectarMQTT();
  }
  mqttClient.loop();

  StaticJsonDocument<384> doc;
  preencherItem(doc.to<JsonObject>(), l);
  char payload[384];
  serializeJson(doc, payload, sizeof(payload));

  bool ok = mqttClient.publish(MQTT_TOPIC, payload);
  Serial.printf("  [MQTT] publish %s -> %s\n", MQTT_TOPIC, ok ? "OK" : "FALHOU");
}

// ================================================================
//   SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  for (int i = 0; i < 7; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LED_DESLIGADO);
  }
  testeLeds();

  Serial.println("=====================================");
  Serial.println("  Monitor Maquina — TCC FATEC (v25)");
  Serial.println("  PZEM + MLP 6-8-4-3 + LEDs -> Planilha (lote) + MQTT");
  Serial.println("=====================================");

  if (!pesosCarregados()) {
    Serial.println("  [AVISO] Pesos do MLP ainda nao colados — LEDs apagados (SEM_PESOS).");
  }
#if SHEETS_LOTE_UM_POST
  Serial.println("  Planilha: lote em 1 POST (exige .gs novo)");
#else
  Serial.println("  Planilha: modo compativel (POSTs seguidos, .gs atual)");
#endif

  conectarWiFi();
  conectarMQTT();

  Serial.println("\nIniciando leituras do PZEM (a cada 10s)...");
  Serial.println("Tensao(V) | Corrente(A) | Potencia(W) | Energia(kWh) | Freq(Hz) | PF | LED | Estado (modelo, confianca)");
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

  // Mesmos limites fisicos do treino: acima disso e erro do medidor.
  if (corrente < 0 || corrente > CORRENTE_MAX_A ||
      tensao < TENSAO_MIN_V || tensao > TENSAO_MAX_V) {
    Serial.println("  [AVISO] Leitura impossivel (fora dos limites) — pulando esse ciclo.");
    return;
  }

  // Pausa longa sem leitura valida: nova sequencia (zera janela e filtro dos LEDs).
  if (temUltimaValida && (agora - ultimaValidaMs) > GAP_MS) {
    resetarJanela();
    Serial.println("  [INFO] Pausa longa sem leitura valida — janela reiniciada.");
  }
  ultimaValidaMs = agora;
  temUltimaValida = true;

  // ── Estado (MLP) e nivel dos LEDs ──
  double ent[6];
  double probs[3] = {0, 0, 0};
  bool pronto = montarEntradas(corrente, tensao, ent);  // atualiza a janela sempre

  int nivelInst = 0;                 // 0 = ainda sem nivel (aquecendo / sem pesos)
  const char* estadoModelo = "Máquina desligada";
  float confianca = 0.0;
  if (corrente >= LIM_SOBRECARGA_A) {
    nivelInst = 7;
    estadoModelo = "SOBRECARGA";     // limiar fixo, fora da rede (igual ao treino)
  } else if (!pronto) {
    estadoModelo = "";      // menos de 10 leituras na janela
  } else if (!pesosCarregados()) {
    estadoModelo = "SEM_PESOS";
  } else {
    int c = inferir(ent, probs);
    estadoModelo = NOMES_CLASSE[c];
    confianca = (float)probs[c];
    nivelInst = nivelDaClasse(c, corrente);
  }

  int nivel = (nivelInst > 0) ? filtrarNivel(nivelInst) : nivelAtual();
  atualizarLeds(nivel);
  const char* estado = (nivel > 0) ? estadoDoNivel(nivel) : estadoModelo;

  Serial.printf("%9.2f | %11.4f | %11.2f | %12.3f | %8.1f | %.2f | %d | %s (%s, %.2f)\n",
                tensao, corrente, potencia, energia, frequencia, pf,
                nivel, estado, estadoModelo, confianca);

  Leitura l;
  l.tensao = tensao;
  l.corrente = corrente;
  l.potencia = potencia;
  l.energia = energia;
  l.frequencia = frequencia;
  l.pf = pf;
  l.confianca = confianca;
  l.nivel = nivel;
  l.estado = estado;
  l.ms = agora;

  publicarMQTT(l);       // MQTT: cada leitura, na hora
  guardarNoLote(l);      // Planilha: em lote
  enviarLoteSheets();
}
