import time
import re
import json
import xlwings as xw
import paho.mqtt.client as mqtt
import ssl
from datetime import datetime

# ══════════════════════════════════════════════════════════════
#   CONFIGURAÇÕES — MQTT HiveMQ Cloud
# ══════════════════════════════════════════════════════════════
MQTT_HOST     = "14fc5a355dec4054b8609b092c9653d7.s1.eu.hivemq.cloud"  # ← cole o host do seu cluster
MQTT_PORT     = 8883                               # TLS obrigatório no HiveMQ Cloud
MQTT_USER     = "esp32"                      # ← usuário criado no HiveMQ
MQTT_PASS     = "Tccfatec1"                        # ← senha criada no HiveMQ
MQTT_TOPIC    = "tamboreadora/corrente"            # tópico publicado pelo ESP32
MQTT_CLIENT_ID = "monitor_python_01"

# ══════════════════════════════════════════════════════════════
#   CONFIGURAÇÕES — EXCEL
# ══════════════════════════════════════════════════════════════
NOME_PLANILHA = 'Monitor_corrente1.xlsm'
MAX_LEITURAS  = 1000
LINHA_DADOS   = 16

# ══════════════════════════════════════════════════════════════
#   TABELA DOS 7 NÍVEIS (espelho do ESP32)
# ══════════════════════════════════════════════════════════════
def classificar(irms: float):
    """Retorna (nivel, estado, cor_bg_rgb, cor_fonte_hex)"""
    if irms < 0.250:
        return 'VERMELHO BAIXO', 'Abaixo do Normal', (192, 57, 43),  '922B21'
    elif irms <= 0.254:
        return 'LARANJA BAIXO',  'Abaixo — Atenção', (230, 126, 34), '784212'
    elif irms <= 0.259:
        return 'AMARELO BAIXO',  'Levemente Baixo',  (241, 196, 15), '7D6608'
    elif irms <= 0.264:
        return 'VERDE — NORMAL', 'Normal',            (39, 174, 96),  '145A32'
    elif irms <= 0.267:
        return 'AMARELO ALTO',   'Levemente Alto',   (243, 156, 18), '784212'
    elif irms <= 0.270:
        return 'LARANJA ALTO',   'Acima — Atenção',  (211, 84, 0),   '784212'
    else:
        return 'VERMELHO ALTO',  'Acima do Normal',  (146, 43, 33),  '922B21'


# ── Colunas dos LEDs no dashboard (A=1 … G=7) ────────────────
LED_COL = {
    'VERMELHO BAIXO': 1,
    'LARANJA BAIXO' : 2,
    'AMARELO BAIXO' : 3,
    'VERDE — NORMAL': 4,
    'AMARELO ALTO'  : 5,
    'LARANJA ALTO'  : 6,
    'VERMELHO ALTO' : 7,
}

LED_COR_APAGADA = {
    1: (100, 30, 25),
    2: (130, 70, 20),
    3: (140, 120, 10),
    4: (20, 90, 50),
    5: (140, 100, 10),
    6: (120, 50, 10),
    7: (90, 25, 20),
}

LED_COR_ACESA = {
    1: (192, 57, 43),
    2: (230, 126, 34),
    3: (241, 196, 15),
    4: (39, 174, 96),
    5: (243, 156, 18),
    6: (211, 84, 0),
    7: (146, 43, 33),
}

# ══════════════════════════════════════════════════════════════
#   ESTADO GLOBAL (compartilhado entre callbacks MQTT e loop)
# ══════════════════════════════════════════════════════════════
estado_global = {
    "ws"      : None,
    "tempo"   : 0,
    "leituras": 0,
    "conectado_mqtt": False,
}


# ══════════════════════════════════════════════════════════════
#   FUNÇÕES EXCEL
# ══════════════════════════════════════════════════════════════
def conectar_excel():
    try:
        app = xw.apps.active
        if app is None:
            raise ConnectionError("Excel não está aberto!")
        for wb in app.books:
            if 'Monitor_corrente' in wb.name:
                print(f"  Planilha encontrada: {wb.name}")
                return wb
        raise ConnectionError(f"'{NOME_PLANILHA}' não encontrado no Excel.")
    except Exception as e:
        raise ConnectionError(str(e))


def atualizar_dashboard(ws, irms: float, nivel: str, estado: str, tempo: int):
    ws.range('B5').value = round(irms, 4)
    ws.range('D5').value = nivel
    ws.range('H5').value = tempo
    ws.range('F8').value = datetime.now().strftime('%H:%M:%S')

    bg, cor_fonte_hex = classificar(irms)[2], classificar(irms)[3]
    ws.range('D5').color = bg
    ws.range('D5').api.Font.Color = int(cor_fonte_hex, 16)
    ws.range('D5').api.Font.Bold  = True
    ws.range('D5').api.Font.Size  = 14

    col_ativa = LED_COL[nivel]
    for col, cor_apag in LED_COR_APAGADA.items():
        ws.range((11, col)).color = cor_apag
    ws.range((11, col_ativa)).color = LED_COR_ACESA[col_ativa]


def gravar_historico(ws, tempo: int, irms: float, nivel: str, estado: str):
    ultima = ws.range(f'C{ws.cells.last_cell.row}').end('up').row
    nova   = max(ultima + 1, LINHA_DADOS)

    ws.range(f'A{nova}').value = tempo
    ws.range(f'C{nova}').value = irms
    ws.range(f'C{nova}').number_format = '0.000'
    ws.range(f'E{nova}').value = nivel
    ws.range(f'G{nova}').value = estado

    for col in ['A', 'C', 'E', 'G']:
        ws.range(f'{col}{nova}').api.HorizontalAlignment = -4108

    _, _, bg, fc_hex = classificar(irms)
    ws.range(f'E{nova}').color = bg
    ws.range(f'E{nova}').api.Font.Color = int(fc_hex, 16)
    ws.range(f'E{nova}').api.Font.Bold  = True

    if nova % 2 == 0:
        for col in ['A', 'C', 'G']:
            ws.range(f'{col}{nova}').color = (214, 228, 240)


def resetar_historico(ws):
    ultima = ws.range(f'A{ws.cells.last_cell.row}').end('up').row
    if ultima >= LINHA_DADOS:
        ws.range(f'A{LINHA_DADOS}:J{ultima}').clear()
    print(">> Histórico resetado (1000 leituras)")


# ══════════════════════════════════════════════════════════════
#   CALLBACKS MQTT
# ══════════════════════════════════════════════════════════════
def on_connect(client, userdata, flags, rc):
    codigos = {
        0: "Conectado com sucesso!",
        1: "Protocolo recusado",
        2: "Client ID inválido",
        3: "Servidor indisponível",
        4: "Usuário/senha incorretos",
        5: "Não autorizado",
    }
    msg = codigos.get(rc, f"Código desconhecido: {rc}")
    if rc == 0:
        estado_global["conectado_mqtt"] = True
        print(f"  [MQTT] {msg}")
        client.subscribe(MQTT_TOPIC, qos=1)
        print(f"  [MQTT] Inscrito no tópico: {MQTT_TOPIC} (QoS 1)")
    else:
        print(f"  [MQTT] ERRO de conexão — {msg}")


def on_disconnect(client, userdata, rc):
    estado_global["conectado_mqtt"] = False
    if rc != 0:
        print(f"  [MQTT] Desconectado inesperadamente (rc={rc}). Reconectando...")


def on_message(client, userdata, msg):
    """
    Recebe mensagem do ESP32 e atualiza Excel.

    O ESP32 deve publicar um JSON no tópico 'tamboreadora/corrente':
        {"irms": 0.261}
    ou simplesmente o valor numérico como string:
        "0.261"
    """
    ws = estado_global["ws"]
    if ws is None:
        return

    try:
        payload = msg.payload.decode('utf-8').strip()

        # ── Tenta JSON primeiro ────────────────────────────
        try:
            dados = json.loads(payload)
            irms  = float(dados.get("irms", dados.get("corrente", 0)))
        except (json.JSONDecodeError, ValueError):
            # Fallback: payload é só o número ("0.261")
            nums = re.findall(r'\d+\.\d+', payload)
            if not nums:
                return
            irms = float(nums[0])

        # ── Ignora nan/inf do ESP32 sem SCT-013 conectado ──
        import math
        if math.isnan(irms) or math.isinf(irms):
            print("  [aviso] Leitura inválida (nan) — aguardando SCT-013...")
            return

        nivel, estado, _, _ = classificar(irms)
        estado_global["tempo"]    += 1
        estado_global["leituras"] += 1
        tempo    = estado_global["tempo"]
        leituras = estado_global["leituras"]

        # ── Atualiza Excel ─────────────────────────────────
        try:
            atualizar_dashboard(ws, irms, nivel, estado, tempo)
            gravar_historico(ws, tempo, irms, nivel, estado)
            print(f"[{leituras:4d}]  {tempo:5d}s  {irms:8.4f} A  {nivel:<20}  {estado}")
        except Exception as e:
            print(f"  [erro Excel] {e}")

        # ── Reset a cada MAX_LEITURAS ──────────────────────
        if leituras >= MAX_LEITURAS:
            resetar_historico(ws)
            estado_global["leituras"] = 0
            estado_global["tempo"]    = 0

    except Exception as e:
        print(f"  [on_message] Erro ao processar mensagem: {e}")


# ══════════════════════════════════════════════════════════════
#   CONFIGURAÇÃO DO CLIENTE MQTT (TLS para HiveMQ Cloud)
# ══════════════════════════════════════════════════════════════
def criar_cliente_mqtt():
    client = mqtt.Client(client_id=MQTT_CLIENT_ID, protocol=mqtt.MQTTv311)

    # Autenticação
    client.username_pw_set(MQTT_USER, MQTT_PASS)

    # TLS — HiveMQ Cloud exige porta 8883 com TLS
    client.tls_set(
        ca_certs=None,                         # usa CAs do sistema operacional
        certfile=None,
        keyfile=None,
        tls_version=ssl.PROTOCOL_TLS,
        cert_reqs=ssl.CERT_REQUIRED,
    )
    client.tls_insecure_set(False)             # valida certificado do servidor

    # Callbacks
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    return client


# ══════════════════════════════════════════════════════════════
#   MAIN
# ══════════════════════════════════════════════════════════════
def main():
    print("=" * 60)
    print("   Monitor de Corrente — TCC FATEC")
    print("   ESP32 + SCT-013  |  7 Níveis  |  MQTT HiveMQ Cloud")
    print("=" * 60)
    print(f"\n  1. Abra '{NOME_PLANILHA}' no Excel")
    print(  "  2. Certifique-se que o ESP32 está publicando no HiveMQ")
    input(  "  3. Pressione ENTER para iniciar...\n")

    # ── Excel ──────────────────────────────────────────────────
    print("Procurando planilha no Excel...")
    try:
        wb = conectar_excel()
        ws = wb.sheets[0]
        estado_global["ws"] = ws
    except ConnectionError as e:
        print(f"\nERRO: {e}")
        return

    # ── MQTT ───────────────────────────────────────────────────
    print(f"\nConectando ao HiveMQ Cloud em {MQTT_HOST}:{MQTT_PORT}...")
    client = criar_cliente_mqtt()

    try:
        client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    except Exception as e:
        print(f"ERRO ao conectar no broker: {e}")
        return

    print("\nAguardando mensagens do ESP32...")
    print(f"{'Leit':>5}  {'Tempo':>6}  {'Corrente':>10}  {'Nível':<20}  Estado")
    print("-" * 65)

    # loop_forever() bloqueia e gerencia reconexão automática
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n\nMonitoramento encerrado.")
    finally:
        client.disconnect()
        try:
            wb.save()
            print("Planilha salva!")
        except Exception:
            print("Salve a planilha manualmente.")


if __name__ == "__main__":
    main()