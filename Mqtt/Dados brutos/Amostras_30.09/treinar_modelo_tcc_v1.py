# -*- coding: utf-8 -*-
"""
treinar_modelo_tcc_v1.py  (rodar no IDLE: F5, DEPOIS de preparar_dados_tcc_v3.py)

Le os arquivos z-score de ./saida_tcc/ e treina o MLP 6 -> 8 -> 4 -> 3
(ReLU, ReLU, softmax | entropia cruzada | Adam, eta = 0,001 | early stopping).
Gera em ./saida_tcc/:
  modelo_mlp_tcc.h         pesos em arrays float C + funcao de inferencia (sem TFLite Micro)
  modelo_mlp_tcc.keras     modelo completo
  relatorio_treino.txt     metricas
  treino_resultados.png    curvas e matriz de confusao (fundo escuro)
Requer: tensorflow, numpy, pandas, matplotlib
"""
import os
import numpy as np
import pandas as pd

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
import tensorflow as tf
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PASTA = "saida_tcc"
SEMENTE = 42
EPOCAS_MAX = 500
LOTE = 32
PACIENCIA = 25
FEATURES = ["corrente", "potencia", "pf",
            "delta_corrente", "media_corrente", "desvio_corrente"]
CLASSES = ["PARADA", "VAZIO", "USINANDO"]

np.random.seed(SEMENTE)
tf.random.set_seed(SEMENTE)


def ler(nome):
    d = pd.read_csv(os.path.join(PASTA, nome), sep=";", decimal=",", encoding="utf-8-sig")
    return d[FEATURES].values.astype("float32"), d["classe_id"].values.astype("int32")


Xtr, ytr = ler("02_treino_z.csv")
Xva, yva = ler("03_validacao_z.csv")
Xte, yte = ler("04_teste_z.csv")
print("treino %d | validacao %d | teste %d" % (len(ytr), len(yva), len(yte)))
for nome, y in (("treino", ytr), ("validacao", yva), ("teste", yte)):
    print("  %-9s" % nome, {CLASSES[i]: int((y == i).sum()) for i in range(3)})

# pesos de classe (inverso da frequencia, limitado a 10) - classes desbalanceadas
cont = np.bincount(ytr, minlength=3).astype(float)
pesos = {i: (min(10.0, len(ytr) / (3.0 * cont[i])) if cont[i] > 0 else 1.0) for i in range(3)}
print("pesos de classe:", {CLASSES[i]: round(v, 2) for i, v in pesos.items()})

modelo = tf.keras.Sequential([
    tf.keras.layers.Input(shape=(6,)),
    tf.keras.layers.Dense(8, activation="relu"),
    tf.keras.layers.Dense(4, activation="relu"),
    tf.keras.layers.Dense(3, activation="softmax"),
])
modelo.compile(optimizer=tf.keras.optimizers.Adam(learning_rate=0.001),
               loss="sparse_categorical_crossentropy", metrics=["accuracy"])
print("parametros treinaveis:", modelo.count_params())   # 107

parada = tf.keras.callbacks.EarlyStopping(monitor="val_loss", patience=PACIENCIA,
                                          restore_best_weights=True)
hist = modelo.fit(Xtr, ytr, validation_data=(Xva, yva), epochs=EPOCAS_MAX,
                  batch_size=LOTE, class_weight=pesos, callbacks=[parada], verbose=0)
epocas = len(hist.history["loss"])
print("epocas rodadas:", epocas, "| melhor val_loss: %.4f" % min(hist.history["val_loss"]))


# ----------------------------------------------------------------------
# avaliacao
# ----------------------------------------------------------------------
def avaliar(X, y):
    p = modelo.predict(X, verbose=0)
    pred = p.argmax(axis=1)
    cm = np.zeros((3, 3), dtype=int)
    for a, b in zip(y, pred):
        cm[a, b] += 1
    return pred, cm


linhas = []
for nome, X, y in (("TREINO", Xtr, ytr), ("VALIDACAO", Xva, yva), ("TESTE", Xte, yte)):
    pred, cm = avaliar(X, y)
    acc = (pred == y).mean()
    linhas.append("%s: acuracia = %.2f%% (%d amostras)" % (nome, 100 * acc, len(y)))
    for i, c in enumerate(CLASSES):
        n = cm[i].sum()
        rec = cm[i, i] / n if n else float("nan")
        prc = cm[i, i] / cm[:, i].sum() if cm[:, i].sum() else float("nan")
        linhas.append("   %-9s n=%4d  recall=%6.2f%%  precisao=%6.2f%%" % (c, n, 100 * rec, 100 * prc))
    if nome == "TESTE":
        cm_teste = cm
        linhas.append("   matriz de confusao (linhas = real, colunas = previsto %s):" % CLASSES)
        for i in range(3):
            linhas.append("     " + str(cm[i].tolist()))
rel = "\n".join(linhas)
print("\n" + rel)


# ----------------------------------------------------------------------
# exporta pesos em C e confere a conta manual com a do Keras
# ----------------------------------------------------------------------
W1, b1, W2, b2, W3, b3 = [w for w in modelo.get_weights()]


def inferir_manual(x):
    a1 = np.maximum(0, x @ W1 + b1)
    a2 = np.maximum(0, a1 @ W2 + b2)
    z = a2 @ W3 + b3
    e = np.exp(z - z.max(axis=1, keepdims=True))
    return e / e.sum(axis=1, keepdims=True)


dif = np.abs(inferir_manual(Xte) - modelo.predict(Xte, verbose=0)).max()
print("\nmaior diferenca entre a conta manual (C) e o Keras no teste: %.2e" % dif)


def arr(nome, a):
    if a.ndim == 1:
        return "const float %s[%d] = {%s};\n" % (nome, a.shape[0], ", ".join("%.8ff" % v for v in a))
    lin = ",\n    ".join("{" + ", ".join("%.8ff" % v for v in r) + "}" for r in a)
    return "const float %s[%d][%d] = {\n    %s\n};\n" % (nome, a.shape[0], a.shape[1], lin)


with open(os.path.join(PASTA, "modelo_mlp_tcc.h"), "w", encoding="utf-8") as f:
    f.write("// MLP 6-8-4-3 (ReLU, ReLU, softmax) - %d parametros - gerado por treinar_modelo_tcc_v1.py\n" % modelo.count_params())
    f.write("// entradas ja em z-score (parametros_zscore.h), nesta ordem: " + ", ".join(FEATURES) + "\n")
    f.write("// saidas: 0 = PARADA, 1 = VAZIO, 2 = USINANDO\n")
    f.write("// SOBRECARGA (> 5,3 A) e CRITICO (>= 7,2 A) ficam por limiar fixo no firmware.\n")
    f.write("#pragma once\n#include <math.h>\n\n")
    f.write(arr("W1", W1) + arr("B1", b1) + arr("W2", W2) + arr("B2", b2) + arr("W3", W3) + arr("B3", b3))
    f.write('''
// x[6] em z-score (ja limitado a +-8). Devolve a classe e preenche p[3] com as probabilidades.
inline int mlp_inferir(const float x[6], float p[3]) {
  float a1[8], a2[4], z[3];
  for (int j = 0; j < 8; j++) { float s = B1[j]; for (int i = 0; i < 6; i++) s += x[i] * W1[i][j]; a1[j] = s > 0 ? s : 0; }
  for (int j = 0; j < 4; j++) { float s = B2[j]; for (int i = 0; i < 8; i++) s += a1[i] * W2[i][j]; a2[j] = s > 0 ? s : 0; }
  float zmax = -1e30f;
  for (int j = 0; j < 3; j++) { float s = B3[j]; for (int i = 0; i < 4; i++) s += a2[i] * W3[i][j]; z[j] = s; if (s > zmax) zmax = s; }
  float soma = 0;
  for (int j = 0; j < 3; j++) { p[j] = expf(z[j] - zmax); soma += p[j]; }
  int melhor = 0;
  for (int j = 0; j < 3; j++) { p[j] /= soma; if (p[j] > p[melhor]) melhor = j; }
  return melhor;
}
''')

modelo.save(os.path.join(PASTA, "modelo_mlp_tcc.keras"))
with open(os.path.join(PASTA, "relatorio_treino.txt"), "w", encoding="utf-8") as f:
    f.write("MLP 6-8-4-3 | parametros: %d | epocas: %d\n\n" % (modelo.count_params(), epocas))
    f.write(rel + "\n")

# ----------------------------------------------------------------------
# figura (fundo escuro)
# ----------------------------------------------------------------------
plt.style.use("dark_background")
fig, ax = plt.subplots(1, 3, figsize=(16, 4.6))
ax[0].plot(hist.history["loss"], label="treino")
ax[0].plot(hist.history["val_loss"], label="validacao")
ax[0].set_title("Perda (entropia cruzada)")
ax[0].set_xlabel("epoca")
ax[0].legend()
ax[1].plot(hist.history["accuracy"], label="treino")
ax[1].plot(hist.history["val_accuracy"], label="validacao")
ax[1].set_title("Acuracia")
ax[1].set_xlabel("epoca")
ax[1].legend()
ax[2].imshow(cm_teste, cmap="viridis")
ax[2].set_xticks(range(3))
ax[2].set_yticks(range(3))
ax[2].set_xticklabels(CLASSES)
ax[2].set_yticklabels(CLASSES)
ax[2].set_xlabel("previsto")
ax[2].set_ylabel("real")
ax[2].set_title("Matriz de confusao (teste)")
for i in range(3):
    for j in range(3):
        ax[2].text(j, i, cm_teste[i, j], ha="center", va="center", color="white")
plt.tight_layout()
plt.savefig(os.path.join(PASTA, "treino_resultados.png"), dpi=150)
print("\nArquivos salvos em:", os.path.abspath(PASTA))
