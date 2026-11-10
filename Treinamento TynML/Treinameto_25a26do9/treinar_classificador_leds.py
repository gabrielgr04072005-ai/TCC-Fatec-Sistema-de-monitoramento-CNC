"""
Classificador multiclasse (gradativo) da tamboreadora/fresadora, seguindo a mesma
lógica dos 7 LEDs indicadores já montados no ESP32:

  vermelho(baixo) | laranja(baixo) | amarelo(baixo) | VERDE | amarelo(alto) | laranja(alto) | vermelho(alto)

Corrente sobe -> desbaste mais profundo -> LED mais "alto" acende.
Nos dados de 25-26/09/2026 a corrente nunca fica abaixo de 3,58A, então os 3 LEDs do
lado "baixo" (sub-corrente) NÃO têm nenhum exemplo real e foram deixados de fora do
treino por enquanto (não dá pra treinar uma classe com zero amostras). O modelo abaixo
cobre as 4 classes com dado disponível:

  0 = verde          (3,5 a 3,8 A — girando, sem usinar)
  1 = amarelo_alto    (3,8 a 3,9 A — desbaste leve)
  2 = laranja_alto     (3,9 a 4,1 A — desbaste médio)
  3 = vermelho_alto     (>= 4,1 A — desbaste mais pesado)

Os limiares acima vieram da distribuição real dos dados (tercis da faixa >3,8A),
não do firmware — ajustar depois se os limiares do firmware forem diferentes.
"""
import numpy as np
import pandas as pd
from sklearn.preprocessing import StandardScaler
from sklearn.model_selection import train_test_split
from sklearn.metrics import classification_report, confusion_matrix
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import Dense
from tensorflow.keras.utils import to_categorical
import tensorflow as tf
import matplotlib.pyplot as plt

np.random.seed(42)
tf.random.set_seed(42)

# ---------------------------------------------------------------------------
# Passo 1: Importar e preparar os dados
# ---------------------------------------------------------------------------
df = pd.read_csv("dados_limpos_25_26set.csv", parse_dates=["ts"])
df = df.sort_values("ts").reset_index(drop=True)

WINDOW = 10
def local_zscore(series, window):
    roll_mean = series.rolling(window, min_periods=window).mean()
    roll_std = series.rolling(window, min_periods=window).std(ddof=0)
    z = (series - roll_mean) / roll_std.replace(0, np.nan)
    z = z.fillna(0)
    z[roll_mean.isna()] = np.nan
    return z

for col, novo in [("corrente", "z_corrente"), ("potencia", "z_potencia"),
                   ("pf", "z_pf"), ("tensao", "z_tensao")]:
    df[novo] = local_zscore(df[col], WINDOW).clip(-8, 8)

df = df.dropna(subset=["z_corrente", "z_potencia", "z_pf", "z_tensao"]).reset_index(drop=True)

# ---------------------------------------------------------------------------
# Passo 2: Rotular pela escala gradativa dos LEDs (4 classes com dado disponível)
# ---------------------------------------------------------------------------
def rotular(corrente):
    if 3.5 <= corrente <= 3.8:
        return 0  # verde
    elif 3.8 < corrente <= 3.9:
        return 1  # amarelo_alto
    elif 3.9 < corrente <= 4.1:
        return 2  # laranja_alto
    elif corrente > 4.1:
        return 3  # vermelho_alto
    return -1  # abaixo de 3.5A -> sem classe treinada ainda, descartado

nomes_classes = ["verde", "amarelo_alto", "laranja_alto", "vermelho_alto"]

df["T"] = df["corrente"].apply(rotular)
df_rotulado = df[df["T"] >= 0].reset_index(drop=True)
print("Contagem por classe:")
print(df_rotulado["T"].map(dict(enumerate(nomes_classes))).value_counts())
print(f"Descartadas (corrente < 3,5A, fora de qualquer classe treinada): {len(df) - len(df_rotulado)}")

features = ["corrente", "z_corrente", "potencia", "z_potencia", "pf", "z_pf", "tensao", "z_tensao"]
P = df_rotulado[features].values
T = df_rotulado["T"].values
T_onehot = to_categorical(T, num_classes=4)
print(f"Tamanho de P: {P.shape} | Tamanho de T (one-hot): {T_onehot.shape}")

# ---------------------------------------------------------------------------
# Passo 3: Normalização
# ---------------------------------------------------------------------------
scaler = StandardScaler().fit(P)
Pest = scaler.transform(P)

# ---------------------------------------------------------------------------
# Passo 4: Dividir treino/teste (estratificado pela classe)
# ---------------------------------------------------------------------------
P_train, P_test, T_train, T_test = train_test_split(
    Pest, T_onehot, test_size=0.20, random_state=42, stratify=T
)

# ---------------------------------------------------------------------------
# Passo 5: Arquitetura (Dense+ReLU -> Dense+softmax, 4 saídas)
# ---------------------------------------------------------------------------
sizeInput = P_train.shape[1]   # 8 features
hiddenNodes = 12
numClasses = 4

model = Sequential([
    Dense(hiddenNodes, activation="relu", input_dim=sizeInput),
    Dense(numClasses, activation="softmax"),
])
model.summary()

# ---------------------------------------------------------------------------
# Passo 6/7: Perda, otimizador e treino
# ---------------------------------------------------------------------------
model.compile(loss="categorical_crossentropy", optimizer=tf.keras.optimizers.Adam(), metrics=["accuracy"])
history = model.fit(
    P_train, T_train,
    epochs=100,
    verbose=1,
    validation_data=(P_test, T_test),
)

# ---------------------------------------------------------------------------
# Passo 8: Visualizar loss + accuracy
# ---------------------------------------------------------------------------
plt.figure(figsize=(12, 4))

plt.subplot(1, 2, 1)
plt.plot(history.epoch, np.array(history.history["loss"]), "b", label="Train Loss")
plt.plot(history.epoch, np.array(history.history["val_loss"]), "r", label="Validation Loss")
plt.xlabel("Épocas"); plt.ylabel("Perda"); plt.legend(); plt.grid()

plt.subplot(1, 2, 2)
plt.plot(history.epoch, np.array(history.history["accuracy"]), "b", label="Train Accuracy")
plt.plot(history.epoch, np.array(history.history["val_accuracy"]), "r", label="Validation Accuracy")
plt.xlabel("Épocas"); plt.ylabel("Acurácia"); plt.legend(); plt.grid()

plt.tight_layout()
plt.savefig("treinamento_multiclasse_leds.png", dpi=130)
plt.show()

# ---------------------------------------------------------------------------
# Passo 9: Matriz de confusão por classe (base desbalanceada entre as 4 classes)
# ---------------------------------------------------------------------------
T_test_labels = np.argmax(T_test, axis=1)
T_pred_labels = np.argmax(model.predict(P_test, verbose=0), axis=1)

print("\nMatriz de confusão (linhas=real, colunas=previsto):")
print(confusion_matrix(T_test_labels, T_pred_labels))
print("\nRelatório de classificação:")
print(classification_report(T_test_labels, T_pred_labels, target_names=nomes_classes))
