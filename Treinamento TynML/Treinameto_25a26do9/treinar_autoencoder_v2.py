"""
Autoencoder para detecção de estado da tamboreadora/fresadora (corrente + potência),
seguindo os mesmos passos do material de estudo (Testes.py) do Gabriel, adaptado para
um problema não-supervisionado (autoencoder reconstrói a entrada, não classifica com rótulo).

Dados: planilha "Monitoramento de máquinas" (Google Drive), leituras de 25 e 26/09/2026,
já limpas de glitches de leitura do Modbus/PZEM.
"""
import numpy as np
import pandas as pd
from sklearn.preprocessing import StandardScaler
from sklearn.model_selection import train_test_split
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import Dense
import tensorflow as tf
import matplotlib.pyplot as plt

# Estabelecer a mesma aleatoriedade para reprodução
np.random.seed(42)
tf.random.set_seed(42)

# ---------------------------------------------------------------------------
# Passo 1: Importar conjunto de dados
# (aqui, em vez de P1.npy/P2.npy, os dados vêm direto da planilha do projeto,
#  já mesclados e limpos de leituras com erro de comunicação Modbus)
# ---------------------------------------------------------------------------
df = pd.read_csv("dados_limpos_25_26set.csv", parse_dates=["ts"])
df = df.sort_values("ts").reset_index(drop=True)
print(f"Tamanho dos dados brutos: {df.shape}")

# ---------------------------------------------------------------------------
# Passo 2: Engenharia de features
# Corrente, Potência, Pf e Tensão brutas + z-score local (janela de 10
# leituras, mesma lógica usada no firmware para classificar a vibração no
# autoencoder v3c). Pf e Tensão entraram depois de checar a correlação com
# a corrente: Pf tem correlação forte (-0,93, cai de ~0,94 no idle pra
# ~0,82 usinando) e Tensão tem correlação moderada (-0,22, sag de carga).
# Frequência foi descartada (correlação ~0 com a corrente, não discrimina
# estado nenhum) e Energia (kWh) também (é um contador cumulativo, vaza
# "tempo decorrido" em vez de estado da máquina).
# ---------------------------------------------------------------------------
WINDOW = 10

def local_zscore(series, window):
    roll_mean = series.rolling(window, min_periods=window).mean()
    roll_std = series.rolling(window, min_periods=window).std(ddof=0)
    z = (series - roll_mean) / roll_std.replace(0, np.nan)
    z = z.fillna(0)              # janela sem variação (std=0) -> z-score 0, não NaN
    z[roll_mean.isna()] = np.nan  # mantém NaN só nas primeiras WINDOW-1 linhas (sem histórico)
    return z

for col, novo in [("corrente", "z_corrente"), ("potencia", "z_potencia"),
                   ("pf", "z_pf"), ("tensao", "z_tensao")]:
    df[novo] = local_zscore(df[col], WINDOW).clip(-8, 8)

df = df.dropna(subset=["z_corrente", "z_potencia", "z_pf", "z_tensao"]).reset_index(drop=True)

features = ["corrente", "z_corrente", "potencia", "z_potencia", "pf", "z_pf", "tensao", "z_tensao"]
P = df[features].values
print(f"Tamanho de P (após feature engineering): {P.shape}")

# ---------------------------------------------------------------------------
# Passo 3: Pré-processamento de dados (normalização)
# ---------------------------------------------------------------------------
scaler = StandardScaler().fit(P)
Pest = scaler.transform(P)

print(f"Dados de entrada brutos (5 primeiras linhas):\n{P[:5]}")
print(f"Dados de entrada estabilizados (5 primeiras linhas):\n{Pest[:5]}")

# ---------------------------------------------------------------------------
# Passo 4: Dividir o conjunto de dados em treinamento e teste
# Autoencoder: T_train/T_test = a própria entrada (o modelo aprende a
# reconstruir P, não existe rótulo de saída separado)
# ---------------------------------------------------------------------------
P_train, P_test = train_test_split(Pest, test_size=0.20, random_state=42)

# ---------------------------------------------------------------------------
# Passo 5: Definir a arquitetura da rede neural (autoencoder simétrico)
# ---------------------------------------------------------------------------
sizeInput = P_train.shape[1]  # 8 features (corrente, potencia, pf, tensao + seus z-scores)

model = Sequential([
    Dense(8, activation="tanh", input_dim=sizeInput),
    Dense(3, activation="tanh"),   # gargalo (bottleneck)
    Dense(8, activation="tanh"),
    Dense(sizeInput, activation="linear"),  # reconstrução da entrada
])

model.summary()

# ---------------------------------------------------------------------------
# Passo 6: Definir a função de perda e o otimizador
# ---------------------------------------------------------------------------
loss = "mse"  # erro de reconstrução (não é classificação, não usa binary_crossentropy)
optimizer = tf.keras.optimizers.Adam()

# ---------------------------------------------------------------------------
# Passo 7: Treinar o modelo (SEM AJUSTE DE LIMIAR AINDA — modelo cru)
# ---------------------------------------------------------------------------
model.compile(loss=loss, optimizer=optimizer)
history = model.fit(
    P_train, P_train,
    epochs=150,
    batch_size=16,
    verbose=0,
    validation_data=(P_test, P_test),
)

# ---------------------------------------------------------------------------
# Passo 8: Visualizar curvas de treinamento + distribuição do erro
# ---------------------------------------------------------------------------
recon = model.predict(Pest, verbose=0)
erro_reconstrucao = np.mean(np.square(Pest - recon), axis=1)

plt.figure(figsize=(12, 4))

plt.subplot(1, 2, 1)
plt.plot(history.epoch, np.array(history.history["loss"]), "b", label="Train Loss")
plt.plot(history.epoch, np.array(history.history["val_loss"]), "r", label="Validation Loss")
plt.xlabel("Épocas")
plt.ylabel("Perda (MSE)")
plt.legend()
plt.grid()

plt.subplot(1, 2, 2)
plt.hist(erro_reconstrucao, bins=60, color="steelblue")
plt.xlabel("Erro de reconstrução (MSE)")
plt.ylabel("Contagem de amostras")
plt.title("Distribuição do erro (modelo cru, sem limiar ajustado)")
plt.grid()

plt.tight_layout()
plt.savefig("treinamento_visual.png", dpi=130)
plt.show()

# ---------------------------------------------------------------------------
# Passo 9 (referência p/ depois): Exportar pesos, camadas e escala p/ ESP32
# Obs.: ainda não faço isso aqui de propósito — depende de confirmar com você
# se vamos usar o pacote "neuroMicro" (como no seu Testes.py) ou manter a
# exportação manual em C (como fizemos no autoencoder v3c).
# ---------------------------------------------------------------------------
