"""
treinar_autoencoder_corrente_v1.py

Autoencoder simples para deteccao de anomalias em corrente eletrica
(fresadora CNC) via sensor SCT-013.

Arquitetura: 10 -> 8 -> 4 -> 8 -> 10, ativacao tanh
Treinado apenas com dados "Normais" (abordagem nao supervisionada).
Features: janelas de Corrente_Normalizada (z-score).

Etapa 1: dados sinteticos, para validar o pipeline completo antes
de calibrar com dados reais da fresadora.
"""

import numpy as np
import tensorflow as tf0
from tensorflow import keras
from tensorflow.keras import layers
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Reprodutibilidade
np.random.seed(42)
tf.random.set_seed(42)

WINDOW_SIZE = 10  # 10 leituras de corrente normalizada por amostra (entrada do autoencoder)


# ---------------------------------------------------------------------------
# 1) Geracao de dados sinteticos
# ---------------------------------------------------------------------------
def gerar_corrente_normal(n_amostras, window_size=WINDOW_SIZE, media=8.0, ruido=0.4):
    """
    Simula leituras de corrente RMS (em Ampere) de uma fresadora operando
    em regime normal: valor medio estavel + ruido gaussiano + pequena
    variacao senoidal (textura de usinagem/vibracao da carga).
    """
    dados = []
    for _ in range(n_amostras):
        t = np.arange(window_size)
        variacao = 0.3 * np.sin(2 * np.pi * t / window_size + np.random.uniform(0, 2 * np.pi))
        ruido_gauss = np.random.normal(0, ruido, window_size)
        janela = media + variacao + ruido_gauss
        dados.append(janela)
    return np.array(dados)


def gerar_corrente_anomala(n_amostras, window_size=WINDOW_SIZE):
    """
    Simula alguns tipos de anomalia tipicos de fresadora CNC:
    - sobrecarga (corrente bem acima do normal)
    - subcarga / ferramenta quebrada (corrente bem abaixo do normal)
    - pico/instabilidade (ruido muito maior, tipo vibracao de rolamento)
    """
    dados = []
    tipos = []
    for i in range(n_amostras):
        tipo = np.random.choice(["sobrecarga", "subcarga", "instavel"])
        t = np.arange(window_size)
        if tipo == "sobrecarga":
            media = np.random.uniform(13.0, 16.0)
            ruido = 0.5
        elif tipo == "subcarga":
            media = np.random.uniform(1.5, 3.5)
            ruido = 0.3
        else:  # instavel
            media = np.random.uniform(7.0, 9.0)
            ruido = 2.5

        variacao = 0.3 * np.sin(2 * np.pi * t / window_size + np.random.uniform(0, 2 * np.pi))
        ruido_gauss = np.random.normal(0, ruido, window_size)
        janela = media + variacao + ruido_gauss
        dados.append(janela)
        tipos.append(tipo)
    return np.array(dados), tipos


# ---------------------------------------------------------------------------
# 2) Normalizacao z-score (mesma logica do firmware: calibracao por janela)
# ---------------------------------------------------------------------------
def normalizar_zscore(dados, media_ref=None, desvio_ref=None):
    """
    Normaliza usando media/desvio de referencia (calculados a partir do
    conjunto de calibracao 'Normal'), para poder aplicar a MESMA
    transformacao em dados novos -- inclusive no ESP32 em producao.
    """
    if media_ref is None:
        media_ref = dados.mean()
    if desvio_ref is None:
        desvio_ref = dados.std()
    return (dados - media_ref) / desvio_ref, media_ref, desvio_ref


# ---------------------------------------------------------------------------
# 3) Construcao do autoencoder (10 -> 8 -> 4 -> 8 -> 10)
# ---------------------------------------------------------------------------
def construir_autoencoder(input_dim=WINDOW_SIZE):
    modelo = keras.Sequential(name="autoencoder_corrente")
    modelo.add(layers.Input(shape=(input_dim,)))
    modelo.add(layers.Dense(8, activation="tanh", name="encoder_1"))
    modelo.add(layers.Dense(4, activation="tanh", name="bottleneck"))
    modelo.add(layers.Dense(8, activation="tanh", name="decoder_1"))
    modelo.add(layers.Dense(input_dim, activation="linear", name="saida"))
    modelo.compile(optimizer=keras.optimizers.Adam(learning_rate=0.01), loss="mse")
    return modelo


# ---------------------------------------------------------------------------
# 4) Pipeline principal
# ---------------------------------------------------------------------------
def main():
    print("=" * 60)
    print("1) Gerando dados sinteticos de corrente...")
    dados_normais = gerar_corrente_normal(n_amostras=2000)
    dados_anomalos, tipos_anomalos = gerar_corrente_anomala(n_amostras=300)
    print(f"   Amostras normais : {dados_normais.shape}")
    print(f"   Amostras anomalas: {dados_anomalos.shape}")

    print("\n2) Normalizando (z-score, referencia = conjunto Normal)...")
    dados_normais_norm, media_ref, desvio_ref = normalizar_zscore(dados_normais)
    dados_anomalos_norm, _, _ = normalizar_zscore(dados_anomalos, media_ref, desvio_ref)
    print(f"   media_ref={media_ref:.4f}  desvio_ref={desvio_ref:.4f}")
    print("   (guarde esses dois valores: sao os mesmos que o firmware usa")
    print("    na calibracao para normalizar a leitura antes do forward pass)")

    # separa treino/validacao apenas com dados normais (nao supervisionado)
    n_treino = int(0.8 * len(dados_normais_norm))
    treino = dados_normais_norm[:n_treino]
    validacao = dados_normais_norm[n_treino:]

    print("\n3) Construindo autoencoder (10 -> 8 -> 4 -> 8 -> 10, tanh)...")
    modelo = construir_autoencoder()
    modelo.summary()

    print("\n4) Treinando (somente com dados NORMAIS)...")
    early_stop = keras.callbacks.EarlyStopping(
        monitor="val_loss", patience=15, restore_best_weights=True
    )
    historico = modelo.fit(
        treino, treino,
        validation_data=(validacao, validacao),
        epochs=200,
        batch_size=32,
        shuffle=True,
        callbacks=[early_stop],
        verbose=0,
    )
    print(f"   Treino finalizado em {len(historico.history['loss'])} epocas")
    print(f"   loss final (treino)    = {historico.history['loss'][-1]:.6f}")
    print(f"   loss final (validacao) = {historico.history['val_loss'][-1]:.6f}")

    print("\n5) Calculando erro de reconstrucao (MSE) por amostra...")
    def mse_por_amostra(x, x_reconstruido):
        return np.mean(np.square(x - x_reconstruido), axis=1)

    recon_normal = modelo.predict(dados_normais_norm, verbose=0)
    erro_normal = mse_por_amostra(dados_normais_norm, recon_normal)

    recon_anomalo = modelo.predict(dados_anomalos_norm, verbose=0)
    erro_anomalo = mse_por_amostra(dados_anomalos_norm, recon_anomalo)

    # limiar provisorio: media + 3 desvios do erro no conjunto normal
    limiar = erro_normal.mean() + 3 * erro_normal.std()
    print(f"   Erro medio (normal)  = {erro_normal.mean():.6f} (+/- {erro_normal.std():.6f})")
    print(f"   Erro medio (anomalo) = {erro_anomalo.mean():.6f} (+/- {erro_anomalo.std():.6f})")
    print(f"   Limiar provisorio (media_normal + 3*desvio) = {limiar:.6f}")

    detectadas = np.sum(erro_anomalo > limiar)
    falsos_positivos = np.sum(erro_normal > limiar)
    print(f"\n   Anomalias detectadas corretamente: {detectadas}/{len(erro_anomalo)} "
          f"({100 * detectadas / len(erro_anomalo):.1f}%)")
    print(f"   Falsos positivos (normal acima do limiar): {falsos_positivos}/{len(erro_normal)} "
          f"({100 * falsos_positivos / len(erro_normal):.1f}%)")

    print("\n6) Salvando modelo e parametros de normalizacao...")
    modelo.save("/home/claude/autoencoder_corrente_v1.keras")
    np.savez(
        "/home/claude/parametros_normalizacao_v1.npz",
        media_ref=media_ref,
        desvio_ref=desvio_ref,
        limiar=limiar,
    )
    print("   Salvo: autoencoder_corrente_v1.keras")
    print("   Salvo: parametros_normalizacao_v1.npz (media_ref, desvio_ref, limiar)")

    print("\n7) Gerando grafico de distribuicao do erro de reconstrucao...")
    plt.style.use("dark_background")
    fig, ax = plt.subplots(figsize=(9, 5.5))
    bins = 40
    ax.hist(erro_normal, bins=bins, alpha=0.7, label="Normal", color="#4FC3F7")
    ax.hist(erro_anomalo, bins=bins, alpha=0.7, label="Anomalo (sintetico)", color="#FF6B6B")
    ax.axvline(limiar, color="yellow", linestyle="--", linewidth=2,
               label=f"Limiar = {limiar:.4f}")
    ax.set_xlabel("Erro de reconstrucao (MSE)")
    ax.set_ylabel("Numero de amostras")
    ax.set_title("Distribuicao do erro de reconstrucao — Autoencoder de Corrente (dados sinteticos)")
    ax.legend()
    fig.tight_layout()
    fig.savefig("/home/claude/distribuicao_erro_reconstrucao_v1.png", dpi=150)
    print("   Salvo: distribuicao_erro_reconstrucao_v1.png")

    print("\n" + "=" * 60)
    print("CONCLUIDO. Proxima etapa: exportar os pesos deste modelo")
    print("para arrays C (.h) e escrever o forward pass em C++ no ESP32.")
    print("=" * 60)


if __name__ == "__main__":
    main()
