# -*- coding: utf-8 -*-
# ================================================================
#   TCC FATEC - Treino do MLP 6 -> 8 -> 4 -> 3  (v5)
#   Estados: PARADA / VAZIO / USINANDO
#
#   O que mudou em relacao a v4:
#     - O PZEM agora le no disjuntor geral: potencia e Pf deixaram de
#       acompanhar a maquina (viraram ruido) e a frequencia e fixa.
#       Por isso saem as entradas potencia e pf.
#     - Entram a TENSAO e o z-score local da corrente.
#     - Dados: so do padrao novo de ligacao (a partir de 05/10/2026).
#
#   Entradas (6): corrente, tensao e quatro derivados da corrente
#   na janela movel de 10 leituras (variacao, media, desvio, z-score).
#
#   Como usar (IDLE):
#     1) Na planilha, baixe cada aba como CSV
#        (Arquivo > Fazer download > Valores separados por virgula):
#          aba "Historico" -> Historico.csv
#          aba "Leituras"  -> Leituras.csv   (se tiver dados)
#     2) Coloque os CSV na MESMA PASTA deste arquivo.
#     3) Abra este arquivo no IDLE e aperte F5.
#
#   O que aparece, na ordem:
#     - resumo dos dados (so contagens)
#     - model.summary(), total de parametros e bytes
#     - cada epoca do treino
#     - acuracia no teste e matriz de confusao
#     - o grafico (perda e acuracia por epoca)
#     - AO FECHAR O GRAFICO: pesos, bias e normalizacao em C
#       para colar no Arduino IDE (ESP32)
# ================================================================

import os
import sys
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import tensorflow as tf
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import Dense, Input
from tensorflow.keras.callbacks import EarlyStopping
import unicodedata

# ---------------------------------------------------------------
#   CONFIGURACAO (mexa so aqui)
# ---------------------------------------------------------------
ARQUIVOS_CSV = ["Historico.csv", "Leituras.csv"]
DATA_INICIO = "2026-10-05"   # padrao novo de ligacao (PZEM no disjuntor geral)
DATA_FIM    = "2026-10-12"   # inclui os dias 08 e 09/10 quando estiverem na planilha

# Limites dos rotulos (funcao de classificacao do documento do TCC)
LIM_PARADA_A      = 0.5   # abaixo disso: PARADA (~0 A)
LIM_USINANDO_A    = 4.0   # a partir daqui: USINANDO (abaixo: VAZIO)
LIM_SOBRECARGA_A  = 5.3   # acima disso: SOBRECARGA (fica FORA do treino:
                          # no firmware e tratada por limiar fixo)

# Limites fisicos: fora disso a leitura e erro do medidor
CORRENTE_MAX_A = 50.0
TENSAO_MIN_V   = 80.0
TENSAO_MAX_V   = 300.0

JANELA     = 10      # leituras da janela movel (media / desvio / z-score)
GAP_SEG    = 300     # pausa maior que isso (s) abre uma nova sequencia
EPS_DESVIO = 1e-6    # desvio menor que isso => z-score = 0 (sem variacao)
BLOCO      = 50      # leituras seguidas que andam juntas no split
SEED       = 42
CLIP_Z     = 8.0

# Para testar sem a tensao, tire "tensao" da lista (a rede se ajusta sozinha)
ENTRADAS   = ["corrente", "tensao", "variacao", "media10", "desvio10", "zscore"]
OCULTAS    = (8, 4)
TAXA       = 0.001
LOTE       = 32
MAX_EPOCAS = 500
PACIENCIA  = 25
PESO_CLASSES = False  # True: da mais peso a classe rara (USINANDO) no treino

CLASSES = ["PARADA", "VAZIO", "USINANDO"]
N_IN = len(ENTRADAS)
H1, H2 = OCULTAS

np.random.seed(SEED)
tf.random.set_seed(SEED)


# ---------------------------------------------------------------
#   LEITURA DOS CSV
# ---------------------------------------------------------------
def sem_acento(t):
    t = unicodedata.normalize("NFKD", str(t))
    return "".join(c for c in t if not unicodedata.combining(c)).strip().lower()


def para_numero(s):
    # "4,24" -> 4.24 | "1.234,5" -> 1234.5 | "4.24" fica
    s = s.astype(str).str.strip()
    tem_virgula = s.str.contains(",", regex=False)
    s = s.where(~tem_virgula,
                s.str.replace(".", "", regex=False).str.replace(",", ".", regex=False))
    return pd.to_numeric(s, errors="coerce")


def achar_coluna(df, nome):
    for c in df.columns:
        if sem_acento(c).startswith(nome):
            return c
    raise KeyError(f"Coluna '{nome}' nao encontrada. Colunas: {list(df.columns)}")


def ler_csv(caminho):
    df = pd.read_csv(caminho, sep=None, engine="python", dtype=str,
                     encoding="utf-8-sig")
    c_data = achar_coluna(df, "data")
    c_hora = achar_coluna(df, "hora")
    out = pd.DataFrame({
        "dt": pd.to_datetime(df[c_data].astype(str).str.strip() + " " +
                             df[c_hora].astype(str).str.strip(),
                             format="%d/%m/%Y %H:%M:%S", errors="coerce"),
        "corrente": para_numero(df[achar_coluna(df, "corrente")]),
        "tensao":   para_numero(df[achar_coluna(df, "tensao")]),
    })
    return out.dropna()


# Pasta dos CSV: a do script (IDLE/F5) ou a passada na linha de comando
if len(sys.argv) > 1 and sys.argv[1]:
    pasta = sys.argv[1]
else:
    pasta = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()

partes = []
print("=" * 60)
print("  DADOS")
print("=" * 60)
for nome in ARQUIVOS_CSV:
    caminho = os.path.join(pasta, nome)
    if not os.path.exists(caminho):
        print(f"  [!] {nome}: nao encontrado em {pasta} (pulando)")
        continue
    d = ler_csv(caminho)
    print(f"  {nome}: {len(d)} leituras validas "
          f"({d['dt'].min():%d/%m %H:%M} a {d['dt'].max():%d/%m %H:%M})")
    partes.append(d)
if not partes:
    raise SystemExit("Nenhum CSV encontrado. Coloque os arquivos na pasta do script.")

df = pd.concat(partes, ignore_index=True)
df = df.drop_duplicates(subset=["dt", "corrente", "tensao"]).sort_values("dt")
df = df[(df["dt"] >= pd.Timestamp(DATA_INICIO)) &
        (df["dt"] < pd.Timestamp(DATA_FIM) + pd.Timedelta(days=1))].reset_index(drop=True)
print(f"  Periodo {DATA_INICIO} a {DATA_FIM}: {len(df)} leituras")
if len(df) == 0:
    raise SystemExit("Nenhuma leitura no periodo. Confira DATA_INICIO / DATA_FIM.")
print(f"  Ultima leitura no periodo: {df['dt'].max():%d/%m/%Y %H:%M:%S}")

# ---------------------------------------------------------------
#   PASSO 1 - tirar os erros do sensor
# ---------------------------------------------------------------
ok = ((df["corrente"] >= 0) & (df["corrente"] <= CORRENTE_MAX_A) &
      (df["tensao"] >= TENSAO_MIN_V) & (df["tensao"] <= TENSAO_MAX_V))
print(f"  Leituras impossiveis removidas: {int((~ok).sum())}")
df = df[ok].reset_index(drop=True)

# ---------------------------------------------------------------
#   Entradas derivadas (dentro de cada sequencia de leituras seguidas)
# ---------------------------------------------------------------
df["seq"] = (df["dt"].diff().dt.total_seconds() > GAP_SEG).cumsum()
g = df.groupby("seq")["corrente"]
df["variacao"] = g.diff()
df["media10"]  = g.transform(lambda s: s.rolling(JANELA).mean())
df["desvio10"] = g.transform(lambda s: s.rolling(JANELA).std(ddof=0))
# z-score local: quantos desvios a corrente atual esta da media da janela.
# Janela sem variacao (desvio ~ 0) => z = 0, nunca NaN (nao perde linhas).
com_var = df["desvio10"] > EPS_DESVIO
df["zscore"] = 0.0
df.loc[com_var, "zscore"] = ((df["corrente"] - df["media10"]) / df["desvio10"])[com_var]
df = df.dropna(subset=["variacao", "media10", "desvio10"]).reset_index(drop=True)

# ---------------------------------------------------------------
#   Rotulos (pela corrente, como no documento do TCC)
# ---------------------------------------------------------------
i = df["corrente"].values
df["classe"] = np.select(
    [i < LIM_PARADA_A, i < LIM_USINANDO_A, i < LIM_SOBRECARGA_A],
    [0, 1, 2], default=3)

n_sobre = int((df["classe"] == 3).sum())
df = df[df["classe"] < 3].reset_index(drop=True)

# ---------------------------------------------------------------
#   MAD dentro de cada classe (VAZIO e USINANDO), so corrente e tensao
# ---------------------------------------------------------------
fora = np.zeros(len(df), dtype=bool)
for cl in (1, 2):
    for col in ("corrente", "tensao"):
        idx = df.index[df["classe"] == cl]
        x = df.loc[idx, col].values
        if len(x) == 0:
            continue
        med = np.median(x)
        mad = np.median(np.abs(x - med))
        if mad > 0:
            mz = 0.6745 * np.abs(x - med) / mad
            fora[idx[mz > 3.5]] = True
n_mad = int(fora.sum())
df = df[~fora].reset_index(drop=True)

print(f"  Fora do treino por SOBRECARGA (> {LIM_SOBRECARGA_A} A): {n_sobre}")
print(f"  Removidas pelo MAD: {n_mad}")
print(f"  Leituras usadas: {len(df)}")
print()
print("  Leituras por classe e por dia:")
tab = pd.crosstab(df["dt"].dt.strftime("%d/%m"), df["classe"].map(dict(enumerate(CLASSES))))
tab = tab.reindex(columns=CLASSES, fill_value=0)
print(tab.to_string())
print("  Total:", {CLASSES[k]: int((df['classe'] == k).sum()) for k in range(3)})
if (df["classe"] == 2).sum() < 300:
    print("  [!] Poucos exemplos de USINANDO: o modelo pode quase nao aprender essa classe.")

# ---------------------------------------------------------------
#   Split em blocos de leituras seguidas: 70% treino / 15% val / 15% teste
# ---------------------------------------------------------------
df["bloco"] = np.arange(len(df)) // BLOCO
blocos = df["bloco"].unique()
rng = np.random.RandomState(SEED)
rng.shuffle(blocos)
n = len(blocos)
b_tr = set(blocos[:int(0.70 * n)])
b_va = set(blocos[int(0.70 * n):int(0.85 * n)])
b_te = set(blocos[int(0.85 * n):])

X = df[ENTRADAS].values.astype("float64")
y = df["classe"].values.astype("int32")
m_tr = df["bloco"].isin(b_tr).values
m_va = df["bloco"].isin(b_va).values
m_te = df["bloco"].isin(b_te).values

# z-score: media e desvio SO do treino
mu = X[m_tr].mean(axis=0)
sd = X[m_tr].std(axis=0)
sd[sd == 0] = 1.0
Xz = np.clip((X - mu) / sd, -CLIP_Z, CLIP_Z)

X_tr, y_tr = Xz[m_tr], y[m_tr]
X_va, y_va = Xz[m_va], y[m_va]
X_te, y_te = Xz[m_te], y[m_te]
print(f"\n  Treino: {len(X_tr)} | Validacao: {len(X_va)} | Teste: {len(X_te)}")

# ---------------------------------------------------------------
#   Modelo 6 -> 8 -> 4 -> 3
# ---------------------------------------------------------------
model = Sequential([
    Input(shape=(N_IN,)),
    Dense(H1, activation="relu"),
    Dense(H2, activation="relu"),
    Dense(3, activation="softmax"),
])
print("\n" + "=" * 60)
print("  MODELO")
print("=" * 60)
model.summary()
params = model.count_params()
print(f"\n  Parametros (pesos + bias): {params}")
print(f"  Memoria dos parametros: {params * 4} bytes (float32)"
      f" | {params * 8} bytes (double, como no .ino)")

model.compile(loss="sparse_categorical_crossentropy",
              optimizer=tf.keras.optimizers.Adam(TAXA),
              metrics=["accuracy"])

# ---------------------------------------------------------------
#   Treino (uma linha por epoca)
# ---------------------------------------------------------------
print("\n" + "=" * 60)
print("  TREINO")
print("=" * 60)
pesos_cl = None
if PESO_CLASSES:
    cont_tr = np.bincount(y_tr, minlength=3).astype(float)
    cont_tr[cont_tr == 0] = 1.0
    pesos_cl = {k: float(len(y_tr) / (3 * cont_tr[k])) for k in range(3)}
    print("  Peso por classe:", {CLASSES[k]: round(v, 2) for k, v in pesos_cl.items()})

parada = EarlyStopping(monitor="val_loss", patience=PACIENCIA,
                       restore_best_weights=True)
history = model.fit(X_tr, y_tr, validation_data=(X_va, y_va),
                    epochs=MAX_EPOCAS, batch_size=LOTE,
                    class_weight=pesos_cl,
                    callbacks=[parada], verbose=2)

h = history.history
melhor = int(np.argmin(h["val_loss"])) + 1
print(f"\n  Treino rodou {len(h['loss'])} epocas; melhores pesos: epoca {melhor}")

# ---------------------------------------------------------------
#   Teste
# ---------------------------------------------------------------
loss_te, acc_te = model.evaluate(X_te, y_te, verbose=0)
pred = np.argmax(model.predict(X_te, verbose=0), axis=1)
cont = np.bincount(y_te, minlength=3)
chute = cont.max() / max(len(y_te), 1)
print(f"\n  Acuracia no teste: {acc_te * 100:.1f}%  "
      f"(chutar sempre a classe mais comum: {chute * 100:.1f}%)")
print("  Matriz de confusao (linha = real, coluna = previsto):")
print("             " + "".join(f"{c:>10}" for c in CLASSES))
for k in range(3):
    linha = [int(((y_te == k) & (pred == j)).sum()) for j in range(3)]
    print(f"  {CLASSES[k]:>10} " + "".join(f"{v:>10}" for v in linha))

# ---------------------------------------------------------------
#   Grafico: perda e acuracia por epoca (feche a janela para ver os pesos)
# ---------------------------------------------------------------
ep = np.arange(1, len(h["loss"]) + 1)
plt.figure(figsize=(12, 4.5), facecolor="white")

plt.subplot(1, 2, 1)
plt.plot(ep, h["loss"], "b", label="Treino")
plt.plot(ep, h["val_loss"], "r", label="Validacao")
plt.axvline(melhor, color="gray", linestyle="--", linewidth=1, label=f"Melhor epoca ({melhor})")
plt.xlabel("Epocas"); plt.ylabel("Perda")
plt.title("Perda por epoca"); plt.legend(); plt.grid(alpha=0.4)

plt.subplot(1, 2, 2)
plt.plot(ep, h["accuracy"], "b", label="Treino")
plt.plot(ep, h["val_accuracy"], "r", label="Validacao")
plt.axvline(melhor, color="gray", linestyle="--", linewidth=1)
plt.xlabel("Epocas"); plt.ylabel("Acuracia")
plt.title("Acuracia por epoca"); plt.legend(); plt.grid(alpha=0.4)

plt.tight_layout()
print("\n  >> Feche a janela do grafico para ver os pesos e os bias. <<")
plt.show()

# ---------------------------------------------------------------
#   Pesos e bias em C (para colar no Arduino IDE)
# ---------------------------------------------------------------
def vetor_c(v):
    return "{" + ",".join(f"{x:.4f}" for x in np.ravel(v)) + "}"


def matriz_c(m):
    return "{" + ",".join(vetor_c(l) for l in m) + "}"


# Keras guarda cada camada como (entradas x neuronios); o .ino usa [neuronios][entradas]
W1, b1, W2, b2, W3, b3 = [np.array(p) for p in model.get_weights()]
W1, W2, W3 = W1.T, W2.T, W3.T

print("\n" + "=" * 60)
print("  PESOS E BIAS FINAIS (cole no Arduino IDE)")
print("=" * 60)
print(f"""
///////////////////// Variables Red Neuronal (MLP {N_IN}-{H1}-{H2}-3) /////////////////////
// Entradas, nesta ordem: {", ".join(ENTRADAS)}.
//   variacao = corrente atual - corrente anterior
//   media10  = media das ultimas {JANELA} correntes
//   desvio10 = desvio-padrao populacional das ultimas {JANELA} correntes
//   zscore   = (corrente - media10) / desvio10, e 0 se desvio10 < {EPS_DESVIO:g}
// Saidas: 0 = PARADA, 1 = VAZIO, 2 = USINANDO
double a0[{N_IN}];
double W1[{H1}][{N_IN}] = {matriz_c(W1)};
double a1[{H1}];
double b1[{H1}] = {vetor_c(b1)};
double W2[{H2}][{H1}] = {matriz_c(W2)};
double a2[{H2}];
double b2[{H2}] = {vetor_c(b2)};
double W3[3][{H2}] = {matriz_c(W3)};
double a3[3];
double b3[3] = {vetor_c(b3)};
double aux = 0.0;

///////////////////// Preprocesamiento (z-score do treino) /////////////////////
// depois do z-score, limitar cada entrada a [-{CLIP_Z:.0f}, +{CLIP_Z:.0f}]
double mean[{N_IN}] = {vetor_c(mu)};
double dstd[{N_IN}] = {vetor_c(sd)};

///////////////////// Estructura Red Neuronal /////////////////////
for(int i = 0; i < {H1}; i++) {{ aux = 0.0; for(int j = 0; j < {N_IN}; j++) {{ aux = aux + W1[i][j]*a0[j]; }} a1[i] = relu(aux + b1[i]); }}
for(int i = 0; i < {H2}; i++) {{ aux = 0.0; for(int j = 0; j < {H1}; j++) {{ aux = aux + W2[i][j]*a1[j]; }} a2[i] = relu(aux + b2[i]); }}
for(int i = 0; i < 3; i++) {{ aux = 0.0; for(int j = 0; j < {H2}; j++) {{ aux = aux + W3[i][j]*a2[j]; }} a3[i] = aux + b3[i]; }}
// softmax (a classe e o indice do maior valor de a3):
double soma = 0.0; for(int i = 0; i < 3; i++) {{ a3[i] = exp(a3[i]); soma = soma + a3[i]; }}
int classe = 0; double melhor = 0.0;
for(int i = 0; i < 3; i++) {{ a3[i] = a3[i]/soma; if(a3[i] > melhor) {{ melhor = a3[i]; classe = i; }} }}
////////////////////////////////////////////////////////////////////
""")
