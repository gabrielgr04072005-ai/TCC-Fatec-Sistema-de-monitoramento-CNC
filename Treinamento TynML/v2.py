"""
treinar_autoencoder_corrente_v2_dados_reais.py

Autoencoder de deteccao de anomalia treinado com dados REAIS extraidos
da planilha "Monitoramento de maquinas" (aba Leituras) em 22/08/2026.

IMPORTANTE - limitacoes destes dados:
- Apenas 93 leituras foram recuperadas (linhas 8-100 da planilha visivel).
  Podem existir mais dados nas abas "Historico"/"Dashboard" nao acessadas.
- Ha uma mudanca de regime no meio da coleta: antes das 14:55 a corrente
  de referencia girava em torno de 0.259A; apos um bloco de recalibracao
  (linhas 65-73, estado "CALIBRANDO"), a referencia caiu para ~0.243A.
  Isso e esperado dado que o firmware faz polling periodico do valor de
  calibracao, mas significa que os dados NAO sao uma unica distribuicao
  estacionaria -- e uma limitacao real a documentar no TCC.
- Com apenas ~30 leituras "Normais" no regime mais recente, o numero de
  janelas de treino e pequeno. Isso e adequado para VALIDAR que o
  pipeline funciona com dados reais, mas nao para calibrar um limiar de
  producao definitivo.

Arquitetura: 10 -> 8 -> 4 -> 8 -> 10, ativacao tanh (mesma da v1).
"""

import numpy as np
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

np.random.seed(42)
tf.random.set_seed(42)

WINDOW_SIZE = 10

# ---------------------------------------------------------------------------
# 1) Dados reais extraidos da planilha (aba "Leituras", linhas 8-100)
#    Corrente (A) e Estado, na ordem cronologica original.
# ---------------------------------------------------------------------------
dados_planilha = [
    (0.253, "Normal"), (0.266, "Acima - Atencao"), (0.261, "Levemente Alto"),
    (0.258, "Levemente Alto"), (0.248, "Normal"), (0.260, "Levemente Alto"),
    (0.265, "Acima - Atencao"), (0.269, "Acima do Normal"), (0.259, "Levemente Alto"),
    (0.255, "Normal"), (0.263, "Levemente Alto"), (0.266, "Acima - Atencao"),
    (0.254, "Normal"), (0.249, "Normal"), (0.260, "Levemente Alto"),
    (0.276, "Acima do Normal"), (0.277, "Acima do Normal"), (0.255, "Normal"),
    (0.259, "Levemente Alto"), (0.255, "Normal"), (0.252, "Normal"),
    (0.253, "Normal"), (0.246, "Normal"), (0.258, "Normal"), (0.258, "Normal"),
    (0.259, "Normal"), (0.253, "Normal"), (0.255, "Normal"), (0.259, "Normal"),
    (0.253, "Normal"), (0.255, "Normal"), (0.254, "Normal"), (0.259, "Normal"),
    (0.256, "Levemente Baixo"), (0.255, "Abaixo - Atencao"), (0.251, "Abaixo do Normal"),
    (0.259, "Normal"), (0.269, "Acima do Normal"), (0.268, "Acima do Normal"),
    (0.270, "Acima do Normal"), (0.257, "Levemente Baixo"), (0.260, "Normal"),
    (0.258, "Levemente Baixo"), (0.262, "Normal"), (0.260, "Normal"),
    (0.260, "Normal"), (0.258, "Normal"), (0.253, "Levemente Baixo"),
    (0.261, "Normal"), (0.261, "Normal"), (0.263, "Normal"), (0.259, "Normal"),
    (0.256, "Normal"), (0.264, "Levemente Alto"), (0.253, "Abaixo - Atencao"),
    (0.260, "Normal"), (0.266, "Levemente Alto"),
    # --- bloco de recalibracao (referencia muda depois disso) ---
    (0.2409, "Calibrando"), (0.2521, "Calibrando"), (0.2373, "Calibrando"),
    (0.2439, "Calibrando"), (0.2332, "Calibrando"), (0.2411, "Calibrando"),
    (0.2449, "Calibrando"), (0.2362, "Calibrando"), (0.2411, "Calibrando"),
    # --- apos recalibracao, nova referencia (~0.243A) ---
    (0.2421, "Abaixo do Normal"), (0.2392, "Abaixo do Normal"), (0.2380, "Abaixo do Normal"),
    (0.2429, "Abaixo do Normal"), (0.2459, "Abaixo - Atencao"), (0.2436, "Abaixo do Normal"),
    (0.2456, "Abaixo - Atencao"), (0.2470, "Abaixo - Atencao"), (0.2410, "Abaixo do Normal"),
    (0.2461, "Abaixo - Atencao"), (0.2412, "Abaixo do Normal"), (0.2419, "Abaixo do Normal"),
    (0.2374, "Abaixo do Normal"), (0.2444, "Abaixo do Normal"), (0.2406, "Abaixo do Normal"),
    (0.2419, "Abaixo do Normal"), (0.2426, "Abaixo do Normal"), (0.2009, "Abaixo do Normal"),
    (0.2460, "Abaixo do Normal"), (0.2407, "Abaixo do Normal"), (0.2469, "Levemente Baixo"),
    (0.2448, "Levemente Baixo"), (0.2481, "Normal"), (0.2444, "Levemente Baixo"),
    (0.2424, "Levemente Baixo"), (0.2221, "Abaixo - Atencao"), (0.2477, "Normal"),
]

correntes = np.array([c for c, _ in dados_planilha])
estados = [e for _, e in dados_planilha]

print("=" * 70)
print(f"1) Dados carregados: {len(correntes)} leituras reais da planilha")

# ---------------------------------------------------------------------------
# 2) Separar os DOIS regimes de calibracao e normalizar CADA UM pela sua
#    propria referencia (media/desvio das leituras "Normal" daquele regime).
#    Isso espelha o que o firmware ja faz na pratica (recalibra e passa a
#    usar uma nova referencia) e permite aproveitar os dados dos dois
#    regimes, em vez de descartar um deles.
# ---------------------------------------------------------------------------
IDX_INICIO_CALIBRACAO = 59   # indice onde comeca o bloco "Calibrando"
IDX_FIM_CALIBRACAO = 68      # indice onde termina o bloco "Calibrando"

correntes_regime1 = correntes[:IDX_INICIO_CALIBRACAO]
estados_regime1 = estados[:IDX_INICIO_CALIBRACAO]
correntes_regime2 = correntes[IDX_FIM_CALIBRACAO + 1:]
estados_regime2 = estados[IDX_FIM_CALIBRACAO + 1:]

print(f"\n2) Regime 1 (antes da recalibracao): {len(correntes_regime1)} leituras")
print(f"   Regime 2 (apos a recalibracao)  : {len(correntes_regime2)} leituras")
print(f"   Bloco de calibracao (descartado): {IDX_FIM_CALIBRACAO - IDX_INICIO_CALIBRACAO + 1} leituras")


def montar_janelas(valores, rotulos, window_size=WINDOW_SIZE, passo=1):
    janelas, labels_janela = [], []
    for i in range(0, len(valores) - window_size + 1, passo):
        janela = valores[i:i + window_size]
        rot_janela = rotulos[i:i + window_size]
        eh_normal = all(r == "Normal" for r in rot_janela)
        janelas.append(janela)
        labels_janela.append("Normal" if eh_normal else "Anomalo")
    return np.array(janelas), labels_janela


def normalizar_regime(correntes_r, estados_r):
    mask_normal = np.array([e == "Normal" for e in estados_r])
    media = correntes_r[mask_normal].mean()
    desvio = max(correntes_r[mask_normal].std(), 1e-4)
    return (correntes_r - media) / desvio, media, desvio


corr1_norm, media_ref1, desvio_ref1 = normalizar_regime(correntes_regime1, estados_regime1)
corr2_norm, media_ref2, desvio_ref2 = normalizar_regime(correntes_regime2, estados_regime2)

print(f"\n3) Normalizacao por regime:")
print(f"   Regime 1: media_ref={media_ref1:.6f}A  desvio_ref={desvio_ref1:.6f}A")
print(f"   Regime 2: media_ref={media_ref2:.6f}A  desvio_ref={desvio_ref2:.6f}A")
print(f"   -> Regime 2 e o mais recente: e o que deve ir para o firmware AGORA.")

janelas1, labels1 = montar_janelas(corr1_norm, estados_regime1)
janelas2, labels2 = montar_janelas(corr2_norm, estados_regime2)

janelas = np.concatenate([janelas1, janelas2], axis=0)
labels_janela = labels1 + labels2

n_normais = sum(1 for l in labels_janela if l == "Normal")
n_anomalas = len(labels_janela) - n_normais

print(f"\n4) Janelas combinadas dos dois regimes (ja normalizadas): {len(janelas)} total")
print(f"   Janelas 100% Normais : {n_normais}")
print(f"   Janelas com alguma leitura fora do normal : {n_anomalas}")

if n_normais < 15:
    print("\n   ATENCAO: poucas janelas normais disponiveis para treino.")
    print("   Resultado deve ser tratado como PROVA DE CONCEITO, nao como")
    print("   modelo calibrado para producao. Mais dados sao necessarios.")

janelas_normais_norm = janelas[[l == "Normal" for l in labels_janela]]
janelas_anomalas_norm = janelas[[l == "Anomalo" for l in labels_janela]]

# referencia "oficial" para o firmware = regime mais recente (regime 2)
media_ref, desvio_ref = media_ref2, desvio_ref2

# ---------------------------------------------------------------------------
# 4) Treino / validacao
# ---------------------------------------------------------------------------
n = len(janelas_normais_norm)
n_treino = max(1, int(0.8 * n))
treino = janelas_normais_norm[:n_treino]
validacao = janelas_normais_norm[n_treino:] if n_treino < n else janelas_normais_norm[-2:]

print(f"\n5) Split treino/validacao: {len(treino)} treino / {len(validacao)} validacao")


def construir_autoencoder(input_dim=WINDOW_SIZE):
    modelo = keras.Sequential(name="autoencoder_corrente_real")
    modelo.add(layers.Input(shape=(input_dim,)))
    modelo.add(layers.Dense(8, activation="tanh", name="encoder_1"))
    modelo.add(layers.Dense(4, activation="tanh", name="bottleneck"))
    modelo.add(layers.Dense(8, activation="tanh", name="decoder_1"))
    modelo.add(layers.Dense(input_dim, activation="linear", name="saida"))
    modelo.compile(optimizer=keras.optimizers.Adam(learning_rate=0.01), loss="mse")
    return modelo


modelo = construir_autoencoder()
print("\n6) Arquitetura:")
modelo.summary()

print("\n7) Treinando com dados REAIS (apenas leituras Normais)...")
early_stop = keras.callbacks.EarlyStopping(
    monitor="val_loss", patience=20, restore_best_weights=True
)
historico = modelo.fit(
    treino, treino,
    validation_data=(validacao, validacao),
    epochs=300,
    batch_size=4,
    shuffle=True,
    callbacks=[early_stop],
    verbose=0,
)

n_epocas = len(historico.history["loss"])
melhor_epoca = int(np.argmin(historico.history["val_loss"]))
loss_melhor_treino = historico.history["loss"][melhor_epoca]
loss_melhor_val = historico.history["val_loss"][melhor_epoca]

print(f"   Epocas rodadas: {n_epocas}")
print(f"   Melhor epoca (restaurada pelo EarlyStopping): {melhor_epoca + 1}")
print(f"   loss (treino) na melhor epoca    = {loss_melhor_treino:.6f}")
print(f"   loss (validacao) na melhor epoca = {loss_melhor_val:.6f}")
print(f"   (loss na ULTIMA epoca rodada, so para referencia: "
      f"treino={historico.history['loss'][-1]:.6f}, "
      f"val={historico.history['val_loss'][-1]:.6f} -- "
      f"NAO e o modelo salvo, que usa os pesos da melhor epoca)")

# ---------------------------------------------------------------------------
# 5) Erro de reconstrucao
# ---------------------------------------------------------------------------
def mse_por_amostra(x, x_rec):
    return np.mean(np.square(x - x_rec), axis=1)

recon_normal = modelo.predict(janelas_normais_norm, verbose=0)
erro_normal = mse_por_amostra(janelas_normais_norm, recon_normal)

if len(janelas_anomalas_norm) > 0:
    recon_anomalo = modelo.predict(janelas_anomalas_norm, verbose=0)
    erro_anomalo = mse_por_amostra(janelas_anomalas_norm, recon_anomalo)
else:
    erro_anomalo = np.array([])

limiar = erro_normal.mean() + 3 * erro_normal.std()

print(f"\n8) Erro de reconstrucao:")
print(f"   Erro medio (normal)  = {erro_normal.mean():.6f} (+/- {erro_normal.std():.6f})")
if len(erro_anomalo) > 0:
    print(f"   Erro medio (anomalo) = {erro_anomalo.mean():.6f} (+/- {erro_anomalo.std():.6f})")
    detectadas = np.sum(erro_anomalo > limiar)
    print(f"   Anomalias detectadas: {detectadas}/{len(erro_anomalo)} "
          f"({100*detectadas/len(erro_anomalo):.1f}%)")
else:
    print("   (nenhuma janela anomala no regime pos-recalibracao para comparar)")
falsos_positivos = np.sum(erro_normal > limiar)
print(f"   Limiar (media_normal + 3*desvio) = {limiar:.6f}")
print(f"   Falsos positivos (normal acima do limiar): {falsos_positivos}/{len(erro_normal)}")

# ---------------------------------------------------------------------------
# 5b) Matriz de confusao e metricas de classificacao
#
# IMPORTANTE: as janelas "Normal" usadas aqui incluem as que o modelo VIU
# durante o treino (nao sao um conjunto de teste separado). Isso tende a
# inflar a acuracia sobre a classe Normal. Com tao poucos dados reais nao
# da pra separar um conjunto de teste de verdade sem esvaziar ainda mais
# o treino -- documentar essa limitacao no TCC e importante.
# ---------------------------------------------------------------------------
from sklearn.metrics import confusion_matrix, accuracy_score, precision_score, recall_score, f1_score

recon_todas = modelo.predict(janelas, verbose=0)
erro_todas = mse_por_amostra(janelas, recon_todas)

y_true = np.array([0 if l == "Normal" else 1 for l in labels_janela])  # 0=Normal, 1=Anomalo
y_pred = (erro_todas > limiar).astype(int)

cm = confusion_matrix(y_true, y_pred, labels=[0, 1])
acc = accuracy_score(y_true, y_pred)
prec = precision_score(y_true, y_pred, zero_division=0)
rec = recall_score(y_true, y_pred, zero_division=0)
f1 = f1_score(y_true, y_pred, zero_division=0)

print(f"\n9) Matriz de confusao (0=Normal, 1=Anomalo):")
print(f"   {cm}")
print(f"   Acuracia geral : {acc*100:.1f}%")
print(f"   Precisao (classe Anomalo) : {prec*100:.1f}%  (dos que o modelo marcou como anomalia, quantos eram de fato)")
print(f"   Recall (classe Anomalo)   : {rec*100:.1f}%  (das anomalias reais, quantas o modelo pegou)")
print(f"   F1-score (classe Anomalo) : {f1*100:.1f}%")
tn, fp, fn, tp = cm.ravel()
print(f"   TN={tn} (normal certo) | FP={fp} (normal marcado como anomalia -- falso alarme)")
print(f"   FN={fn} (anomalia que passou batido) | TP={tp} (anomalia detectada certo)")

# ---------------------------------------------------------------------------
# 6) Salvar modelo, parametros e graficos
# ---------------------------------------------------------------------------
modelo.save("/home/claude/autoencoder_corrente_v2_real.keras")
np.savez(
    "/home/claude/parametros_normalizacao_v2_real.npz",
    media_ref=media_ref, desvio_ref=desvio_ref, limiar=limiar,
)

plt.style.use("default")

fig1, ax1 = plt.subplots(figsize=(9, 5))
ax1.plot(historico.history["loss"], label="Treino", color="#4FC3F7")
ax1.plot(historico.history["val_loss"], label="Validacao", color="#FF6B6B")
ax1.axvline(melhor_epoca, color="#F9A825", linestyle="--", alpha=0.8,
            label=f"Melhor epoca ({melhor_epoca+1}) — pesos salvos")
ax1.set_xlabel("Epoca")
ax1.set_ylabel("Loss (MSE)")
ax1.set_title("Curva de treinamento — Autoencoder de Corrente (dados reais)")
ax1.legend()
fig1.tight_layout()
fig1.savefig("/home/claude/curva_treino_dados_reais.png", dpi=150)

fig2, ax2 = plt.subplots(figsize=(11, 5.5))
x_all = np.arange(len(correntes))
cores = ["#66BB6A" if e == "Normal" else ("#FFD54F" if e == "Calibrando" else "#FF6B6B")
         for e in estados]
ax2.scatter(x_all, correntes, c=cores, s=18)
ax2.axvline(IDX_INICIO_CALIBRACAO, color="#555555", linestyle="--", alpha=0.6)
ax2.axhline(media_ref, color="#4FC3F7", linestyle=":", label=f"media_ref (regime atual) = {media_ref:.4f}A")
ax2.set_xlabel("Ordem da leitura")
ax2.set_ylabel("Corrente (A)")
ax2.set_title("Leituras reais da planilha — verde=Normal, amarelo=Calibrando, vermelho=demais estados")
ax2.legend()
fig2.tight_layout()
fig2.savefig("/home/claude/leituras_reais_planilha.png", dpi=150)

fig3, ax3 = plt.subplots(figsize=(9, 5))
# escala log no eixo X: o erro anomalo tem outliers extremos que, em escala
# linear, esmagam visualmente a barra "Normal" (que fica perto de zero)
todos_erros = np.concatenate([erro_normal, erro_anomalo]) if len(erro_anomalo) > 0 else erro_normal
bins_log = np.logspace(np.log10(max(todos_erros.min(), 1e-4)), np.log10(todos_erros.max()), 20)
ax3.hist(erro_normal, bins=bins_log, alpha=0.75, label="Normal (real)", color="#4FC3F7")
if len(erro_anomalo) > 0:
    ax3.hist(erro_anomalo, bins=bins_log, alpha=0.75, label="Anomalo (real)", color="#FF6B6B")
ax3.axvline(limiar, color="#F9A825", linestyle="--", label=f"Limiar = {limiar:.5f}")
ax3.set_xscale("log")
ax3.set_xlabel("Erro de reconstrucao (MSE) — escala log")
ax3.set_ylabel("N de janelas")
ax3.set_title("Erro de reconstrucao — dados REAIS (regime pos-recalibracao)")
ax3.legend()
fig3.tight_layout()
fig3.savefig("/home/claude/erro_reconstrucao_dados_reais.png", dpi=150)

# grafico 4: matriz de confusao
fig4, ax4 = plt.subplots(figsize=(6, 5.5))
im = ax4.imshow(cm, cmap="Blues")
ax4.set_xticks([0, 1])
ax4.set_yticks([0, 1])
ax4.set_xticklabels(["Normal", "Anomalo"])
ax4.set_yticklabels(["Normal", "Anomalo"])
ax4.set_xlabel("Previsto pelo modelo")
ax4.set_ylabel("Real (rotulo da planilha)")
ax4.set_title(f"Matriz de Confusao — Acuracia: {acc*100:.1f}%")
for i in range(2):
    for j in range(2):
        cor_texto = "white" if cm[i, j] > cm.max() / 2 else "black"
        ax4.text(j, i, str(cm[i, j]), ha="center", va="center",
                  color=cor_texto, fontsize=20, fontweight="bold")
fig4.colorbar(im, ax=ax4, label="N de janelas")
fig4.tight_layout()
fig4.savefig("/home/claude/matriz_confusao_dados_reais.png", dpi=150)

print("\n" + "=" * 70)
print("Arquivos salvos:")
print("  autoencoder_corrente_v2_real.keras")
print("  parametros_normalizacao_v2_real.npz")
print("  curva_treino_dados_reais.png")
print("  leituras_reais_planilha.png")
print("  erro_reconstrucao_dados_reais.png")
print("=" * 70)