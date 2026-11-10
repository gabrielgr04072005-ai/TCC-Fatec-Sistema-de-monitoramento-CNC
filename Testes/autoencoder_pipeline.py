"""
Pipeline de deteccao de anomalias por autoencoder - TCC
Entrada: monitor_corrente_dados.csv (Tempo, Corrente, Nivel, Estado)
Saida: CSV simplificado (Tempo, Corrente), graficos, metricas de validacao
"""
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from sklearn.neural_network import MLPRegressor

# -----------------------------------------------------------------
# 1. Carregar dados e gerar CSV simplificado (Tempo, Corrente)
# -----------------------------------------------------------------
df = pd.read_csv('monitor_corrente_dados.csv')
df_simples = df[['Tempo (s)', 'Corrente (A)']].copy()
df_simples.to_csv('/mnt/user-data/outputs/tempo_corrente.csv', index=False)
print(f"CSV simplificado salvo com {len(df_simples)} linhas.")

# -----------------------------------------------------------------
# 2. Normalizacao estatistica usando SOMENTE as leituras normais
#    (media e desvio calculados sem depender de manual da maquina)
# -----------------------------------------------------------------
normal_mask = df['Estado'] == 'Normal'
mu = df.loc[normal_mask, 'Corrente (A)'].mean()
sigma = df.loc[normal_mask, 'Corrente (A)'].std()
print(f"mu (normal) = {mu:.5f} A | sigma (normal) = {sigma:.5f} A")

df['z'] = (df['Corrente (A)'] - mu) / sigma

# -----------------------------------------------------------------
# 3. Construcao das janelas deslizantes (tamanho 10)
# -----------------------------------------------------------------
W = 10
z = df['z'].values
estados = df['Estado'].values
niveis = df['Nivel'].values
tempos = df['Tempo (s)'].values

X_all, idx_fim = [], []
for i in range(len(z) - W + 1):
    X_all.append(z[i:i+W])
    idx_fim.append(i + W - 1)  # indice do ultimo ponto da janela
X_all = np.array(X_all)
idx_fim = np.array(idx_fim)

# janelas de treino: só as que sao 100% normais (sem nenhum ponto anomalo)
estados_janela = np.array([estados[i:i+W] for i in range(len(z) - W + 1)])
treino_mask = np.all(estados_janela == 'Normal', axis=1)
X_treino = X_all[treino_mask]
print(f"Total de janelas: {len(X_all)} | janelas de treino (100% normais): {len(X_treino)}")

# -----------------------------------------------------------------
# 4. Treinamento do autoencoder (10 -> 8 -> 4 -> 8 -> 10)
# -----------------------------------------------------------------
autoencoder = MLPRegressor(
    hidden_layer_sizes=(8, 4, 8),
    activation='relu',
    solver='adam',
    max_iter=3000,
    random_state=42,
    learning_rate_init=0.005,
)
autoencoder.fit(X_treino, X_treino)

# -----------------------------------------------------------------
# 5. Erro de reconstrucao (MSE) para TODAS as janelas
# -----------------------------------------------------------------
X_recon = autoencoder.predict(X_all)
erro_recon = np.mean((X_all - X_recon) ** 2, axis=1)

# direcao (acima/abaixo do normal) a partir do sinal medio da janela
direcao = np.sign(np.mean(X_all, axis=1))
direcao[direcao == 0] = 1
score_sinalizado = direcao * erro_recon

# -----------------------------------------------------------------
# 6. Classificacao em 7 niveis via percentis do score sinalizado
#    (calculado a partir das janelas de treino, sem manual da maquina)
# -----------------------------------------------------------------
score_treino = score_sinalizado[treino_mask]
percentis = [0, 5, 20, 40, 60, 80, 95, 100]
limiares = np.percentile(score_treino, percentis)
limiares[0], limiares[-1] = -np.inf, np.inf

led_previsto = np.digitize(score_sinalizado, limiares[1:-1]) + 1  # 1 a 7

# -----------------------------------------------------------------
# 7. Comparacao com o rotulo real (Nivel) para validacao
# -----------------------------------------------------------------
ordem_niveis = {
    'VERMELHO BAIXO': 1, 'LARANJA BAIXO': 2, 'AMARELO BAIXO': 3,
    'VERDE - NORMAL': 4,
    'AMARELO ALTO': 5, 'LARANJA ALTO': 6, 'VERMELHO ALTO': 7,
}
nivel_real_janela = niveis[idx_fim]
led_real = np.array([ordem_niveis[n] for n in nivel_real_janela])

acerto_exato = np.mean(led_previsto == led_real) * 100
acerto_tolerancia = np.mean(np.abs(led_previsto - led_real) <= 1) * 100
correlacao = np.corrcoef(led_previsto, led_real)[0, 1]

print(f"\n--- Validacao contra o rotulo real (Nivel) ---")
print(f"Acerto exato (mesmo LED):        {acerto_exato:.1f}%")
print(f"Acerto com tolerancia de 1 LED:  {acerto_tolerancia:.1f}%")
print(f"Correlacao (previsto vs real):   {correlacao:.3f}")

# -----------------------------------------------------------------
# 8. Graficos
# -----------------------------------------------------------------
tempo_janela = tempos[idx_fim]
cores_nivel = {
    'VERMELHO BAIXO': '#A32D2D', 'LARANJA BAIXO': '#D85A30', 'AMARELO BAIXO': '#BA7517',
    'VERDE - NORMAL': '#3B6D11',
    'AMARELO ALTO': '#BA7517', 'LARANJA ALTO': '#D85A30', 'VERMELHO ALTO': '#A32D2D',
}

fig, axs = plt.subplots(3, 1, figsize=(11, 12), sharex=True)

# (a) corrente bruta ao longo do tempo, colorida pelo nivel real
cores_pontos = [cores_nivel[n] for n in df['Nivel']]
axs[0].scatter(df['Tempo (s)'], df['Corrente (A)'], c=cores_pontos, s=6)
axs[0].axhline(mu, color='black', linestyle='--', linewidth=1, label=f'media normal ({mu:.4f} A)')
axs[0].set_ylabel('Corrente (A)')
axs[0].set_title('Corrente medida ao longo do tempo (cor = nivel real registrado)')
axs[0].legend(loc='upper right')

# (b) erro de reconstrucao com as faixas dos 7 niveis
axs[1].plot(tempo_janela, erro_recon, color='#185FA5', linewidth=1)
for lim in limiares[1:-1]:
    axs[1].axhline(abs(lim), color='gray', linestyle=':', linewidth=0.7)
axs[1].set_ylabel('Erro de reconstrucao (MSE)')
axs[1].set_title('Erro de reconstrucao do autoencoder por janela (linhas pontilhadas = limiares dos 7 niveis)')

# (c) LED previsto vs LED real ao longo do tempo
axs[2].plot(tempo_janela, led_real, label='Nivel real (Nivel)', color='#639922', linewidth=1.5)
axs[2].plot(tempo_janela, led_previsto, label='LED previsto (autoencoder)', color='#D85A30',
            linewidth=1, linestyle='--')
axs[2].set_ylabel('Nivel (1-7)')
axs[2].set_xlabel('Tempo (s)')
axs[2].set_yticks(range(1, 8))
axs[2].set_title(f'Comparacao: LED previsto vs nivel real (acerto exato {acerto_exato:.1f}%, tolerancia 1 LED {acerto_tolerancia:.1f}%)')
axs[2].legend(loc='upper right')

plt.tight_layout()
plt.savefig('/mnt/user-data/outputs/graficos_autoencoder.png', dpi=140)
print("\nGraficos salvos em graficos_autoencoder.png")

# histograma separado da distribuicao do erro de reconstrucao
plt.figure(figsize=(8, 4.5))
plt.hist(erro_recon[treino_mask], bins=40, color='#5DCAA5', alpha=0.7, label='janelas normais (treino)')
plt.hist(erro_recon[~treino_mask], bins=40, color='#E24B4A', alpha=0.7, label='janelas com anomalia')
plt.xlabel('Erro de reconstrucao (MSE)')
plt.ylabel('Quantidade de janelas')
plt.title('Distribuicao do erro de reconstrucao: normal vs anomalo')
plt.legend()
plt.tight_layout()
plt.savefig('/mnt/user-data/outputs/histograma_erro.png', dpi=140)
print("Histograma salvo em histograma_erro.png")
