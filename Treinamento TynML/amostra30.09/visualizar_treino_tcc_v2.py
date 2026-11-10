# -*- coding: utf-8 -*-
"""
visualizar_treino_tcc_v2.py  (rodar no IDLE: F5, DEPOIS de preparar_dados_tcc_v3.py)

Fundo branco, paleta de 4 cores. Treina o MLP 6 -> 8 -> 4 -> 3 do TCC passo a passo (backpropagation + Adam, feito em NumPy
para podermos guardar os pesos de cada epoca) e mostra, de forma visual:

  * a rede com seus neuronios: entradas, 2 camadas escondidas e saida;
    a cor do neuronio = quanto ele "acende" para uma leitura real; a linha = peso
  * as leituras reais em 3D (corrente x potencia x fator de potencia),
    coloridas pelo que o modelo preve naquela epoca (anel escuro = errou)
  * as curvas de erro (perda) e de acerto (acuracia)

Gera em ./saida_tcc/:
  treino_animado.gif               animacao do treino (as 3 visoes juntas)
  grafico_3d_variaveis.png         leituras em 3D por classe (no IDLE abre girando com o mouse)
  estrutura_rede_VAZIO.png         rede treinada processando uma leitura de VAZIO
  estrutura_rede_USINANDO.png      rede treinada processando uma leitura de USINANDO

O treino e real; a animacao reproduz as epocas gravadas em velocidade legivel
(o treino inteiro leva poucos segundos).  Use  --sem-janela  para so gerar os arquivos.
Requer: numpy, pandas, matplotlib, pillow
"""
import os
import sys
import numpy as np
import pandas as pd
import matplotlib
SEM_JANELA = "--sem-janela" in sys.argv
if SEM_JANELA:
    matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection
from matplotlib.lines import Line2D
from matplotlib.patches import Rectangle
from matplotlib.animation import FuncAnimation, PillowWriter

# ----------------------------------------------------------------------
# CONFIGURACAO
# ----------------------------------------------------------------------


def achar_pasta():
    """Procura a pasta com 02_treino_z.csv: ao lado do script, na pasta atual ou pergunta."""
    try:
        base = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        base = os.getcwd()
    for c in (os.path.join(base, "saida_tcc"), base, os.path.join(os.getcwd(), "saida_tcc"), os.getcwd()):
        if os.path.isfile(os.path.join(c, "02_treino_z.csv")):
            return c
    if not SEM_JANELA:
        import tkinter as tk
        from tkinter import filedialog, messagebox
        raiz = tk.Tk()
        raiz.withdraw()
        raiz.attributes("-topmost", True)
        messagebox.showinfo("Pasta dos dados",
                            "Nao achei a pasta saida_tcc ao lado do script.\n"
                            "Selecione a pasta que contem 02_treino_z.csv "
                            "(gerada pelo preparar_dados_tcc_v3.py).")
        c = filedialog.askdirectory(title="Selecione a pasta saida_tcc")
        raiz.destroy()
        if c and os.path.isfile(os.path.join(c, "02_treino_z.csv")):
            return c
    sys.exit("ERRO: pasta com 02_treino_z.csv nao encontrada. Rode antes o preparar_dados_tcc_v3.py "
             "(ou descompacte o saida_tcc.zip ao lado deste script).")


PASTA = achar_pasta()
print("Pasta de dados:", PASTA)
SEMENTE = 42
EPOCAS_MAX = 400
LOTE = 32
LR = 0.001
PACIENCIA = 25
SALVAR_GIF = True
FPS = 10
DPI_GIF = 70

FEATURES = ["corrente", "potencia", "pf", "delta_corrente", "media_corrente", "desvio_corrente"]
NOME_ENTRADA = ["Corrente", "Potência", "Fator de potência", "Variação da corrente",
                "Média (10 leituras)", "Oscilação (10 leituras)"]
UNID = ["A", "W", "", "A", "A", "A"]
CLASSES = ["PARADA", "VAZIO", "USINANDO"]
# Paleta (4 cores harmonicas): azul-petroleo #264653, verde-agua #2A9D8F, amarelo #E9C46A, coral #E76F51
C_PETROLEO, C_AGUA, C_AMARELO, C_CORAL = "#264653", "#2A9D8F", "#E9C46A", "#E76F51"
COR_CLASSE = [C_AMARELO, C_AGUA, C_CORAL]          # PARADA, VAZIO, USINANDO
COR_ERRO = C_PETROLEO
COR_POS, COR_NEG = (0.165, 0.616, 0.561), (0.906, 0.435, 0.318)
FUNDO = "#ffffff"
TEXTO = C_PETROLEO
SUB = "#6b7c85"                                     # cinza neutro para textos secundarios
GRADE = "#dfe4e8"
plt.rcParams.update({"figure.facecolor": FUNDO, "axes.facecolor": FUNDO, "savefig.facecolor": FUNDO,
                     "text.color": TEXTO, "axes.labelcolor": TEXTO, "xtick.color": SUB, "ytick.color": SUB,
                     "axes.edgecolor": "#c5cdd3", "legend.labelcolor": TEXTO})
from matplotlib.colors import LinearSegmentedColormap
CM_IN = LinearSegmentedColormap.from_list("entrada", [C_CORAL, "#ffffff", C_AGUA])
CM_ATIV = LinearSegmentedColormap.from_list("ativ", ["#ffffff", C_AGUA, C_PETROLEO])


def fmt(x, casas=2):
    return ("{:." + str(casas) + "f}").format(x).replace(".", ",")


# ----------------------------------------------------------------------
# DADOS
# ----------------------------------------------------------------------
def ler_z(nome):
    d = pd.read_csv(os.path.join(PASTA, nome), sep=";", decimal=",", encoding="utf-8-sig")
    return d[FEATURES].values.astype(float), d["classe_id"].values.astype(int)


Xtr, ytr = ler_z("02_treino_z.csv")
Xva, yva = ler_z("03_validacao_z.csv")
Xte, yte = ler_z("04_teste_z.csv")
par = pd.read_csv(os.path.join(PASTA, "parametros_zscore.csv"), sep=";", decimal=",", encoding="utf-8-sig")
MU, SIG = par["mu"].values, par["sigma"].values
limpos = pd.read_csv(os.path.join(PASTA, "01_dados_limpos_MAD.csv"), sep=";", decimal=",", encoding="utf-8-sig")
Xall = np.clip((limpos[FEATURES].values - MU) / SIG, -8, 8)
yall = limpos["classe_id"].values.astype(int)
print("treino %d | validacao %d | teste %d | total para o 3D %d" % (len(ytr), len(yva), len(yte), len(yall)))

cont = np.bincount(ytr, minlength=3).astype(float)
PESOS = np.array([min(10.0, len(ytr) / (3.0 * cont[i])) if cont[i] > 0 else 1.0 for i in range(3)])


# ----------------------------------------------------------------------
# MLP em NumPy (6-8-4-3)  - o mesmo calculo que o ESP32 faz
# ----------------------------------------------------------------------
def frente(W, b, X):
    a1 = np.maximum(0, X @ W[0] + b[0])
    a2 = np.maximum(0, a1 @ W[1] + b[1])
    z = a2 @ W[2] + b[2]
    z = z - z.max(axis=1, keepdims=True)
    e = np.exp(z)
    return a1, a2, e / e.sum(axis=1, keepdims=True)


def perda(p, y):
    return float(-np.mean(np.log(p[np.arange(len(y)), y] + 1e-12)))


def metricas(W, b, X, y):
    p = frente(W, b, X)[2]
    return perda(p, y), float((p.argmax(1) == y).mean())


def treinar():
    rng = np.random.default_rng(SEMENTE)
    tam = [6, 8, 4, 3]
    W = [rng.normal(0, np.sqrt(2.0 / tam[i]), (tam[i], tam[i + 1])) for i in range(3)]
    b = [np.zeros(tam[i + 1]) for i in range(3)]
    params = W + b
    m = [np.zeros_like(p) for p in params]
    v = [np.zeros_like(p) for p in params]
    sw = PESOS[ytr]
    snaps, hist = [], []

    def registrar():
        snaps.append(([w.copy() for w in W], [x.copy() for x in b]))
        l1, a1 = metricas(W, b, Xtr, ytr)
        l2, a2 = metricas(W, b, Xva, yva)
        hist.append((l1, a1, l2, a2))

    registrar()
    melhor, melhor_ep, sem, t = 1e9, 0, 0, 0
    for ep in range(1, EPOCAS_MAX + 1):
        ordem = rng.permutation(len(ytr))
        for i in range(0, len(ordem), LOTE):
            idx = ordem[i:i + LOTE]
            X, y, B = Xtr[idx], ytr[idx], len(idx)
            a1, a2, p = frente(W, b, X)
            dz3 = p.copy()
            dz3[np.arange(B), y] -= 1
            dz3 *= (sw[idx] / B)[:, None]            # backpropagation: da saida para a entrada
            dz2 = (dz3 @ W[2].T) * (a2 > 0)
            dz1 = (dz2 @ W[1].T) * (a1 > 0)
            grads = [X.T @ dz1, a1.T @ dz2, a2.T @ dz3, dz1.sum(0), dz2.sum(0), dz3.sum(0)]
            t += 1
            for k in range(6):                       # Adam
                m[k] = 0.9 * m[k] + 0.1 * grads[k]
                v[k] = 0.999 * v[k] + 0.001 * grads[k] ** 2
                params[k] -= LR * (m[k] / (1 - 0.9 ** t)) / (np.sqrt(v[k] / (1 - 0.999 ** t)) + 1e-7)
        registrar()
        if hist[-1][2] < melhor - 1e-6:
            melhor, melhor_ep, sem = hist[-1][2], ep, 0
        else:
            sem += 1
            if sem >= PACIENCIA:
                break
    return snaps, hist, melhor_ep


print("Treinando...")
SNAPS, HIST, MELHOR = treinar()
HIST = np.array(HIST)
N_EP = len(SNAPS) - 1
Wb, bb = SNAPS[MELHOR]
l_te, a_te = metricas(Wb, bb, Xte, yte)
print("epocas rodadas: %d | melhores pesos: epoca %d" % (N_EP, MELHOR))
print("acuracia: treino %.2f%% | validacao %.2f%% | teste %.2f%%"
      % (100 * HIST[MELHOR, 1], 100 * HIST[MELHOR, 3], 100 * a_te))

# amostras de exemplo (leitura real tipica de cada classe) e pontos do 3D
EXEMPLO = {}
for c in (1, 2):
    idx = np.where(ytr == c)[0]
    cor = Xtr[idx, 0] * SIG[0] + MU[0]
    EXEMPLO[c] = Xtr[idx[np.argmin(np.abs(cor - np.median(cor)))]]
rng3 = np.random.default_rng(SEMENTE)
sel = np.concatenate([rng3.choice(np.where(yall == c)[0], min(int((yall == c).sum()), q), replace=False)
                      for c, q in ((1, 330), (2, 250)) if (yall == c).any()])   # PARADA (3 leituras em ~0 A) fica fora do 3D: esticaria a escala
P3 = limpos.iloc[sel][["corrente", "potencia", "pf"]].values
X3, y3 = Xall[sel], yall[sel]


# ----------------------------------------------------------------------
# DESENHO DA REDE
# ----------------------------------------------------------------------
XS = [0.0, 3.4, 6.1, 8.7]
YS = [5 + (np.arange(n) - (n - 1) / 2.0) * -s for n, s in ((6, 1.4), (8, 1.08), (4, 1.55), (3, 1.9))]
WMAX = max(np.abs(w).max() for s in SNAPS for w in s[0])


def desenhar_rede(ax, W, b, x, titulo_amostra=None):
    ax.clear()
    ax.set_facecolor(FUNDO)
    ax.set_xlim(-4.6, 13.9)
    ax.set_ylim(-0.2, 10.6)
    ax.axis("off")
    a1, a2, p = frente(W, b, x[None, :])
    a1, a2, p = a1[0], a2[0], p[0]

    segs, cores, larg = [], [], []
    for l in range(3):
        for i in range(W[l].shape[0]):
            for j in range(W[l].shape[1]):
                w = W[l][i, j]
                f = min(1.0, abs(w) / WMAX)
                segs.append([(XS[l], YS[l][i]), (XS[l + 1], YS[l + 1][j])])
                cores.append((*(COR_POS if w >= 0 else COR_NEG), 0.22 + 0.78 * f))
                larg.append(0.4 + 3.6 * f)
    ax.add_collection(LineCollection(segs, colors=cores, linewidths=larg, zorder=1))

    cm_in, cm_h, cm_o = CM_IN, CM_ATIV, CM_ATIV
    vals = [x, a1, a2, p]
    for l in range(4):
        if l == 0:
            fc = cm_in((np.clip(vals[l], -3, 3) + 3) / 6.0)
        elif l == 3:
            fc = cm_o(vals[l])
        else:
            fc = cm_h(np.clip(vals[l] / max(1.0, vals[l].max()), 0, 1))
        ax.scatter([XS[l]] * len(YS[l]), YS[l], s=520, c=fc, edgecolors=TEXTO, linewidths=1.4, zorder=3)

    # entradas: nome + valor real + z
    for i in range(6):
        real = x[i] * SIG[i] + MU[i]
        ax.text(XS[0] - 0.45, YS[0][i], "%s\n%s %s  (z = %s)" % (NOME_ENTRADA[i], fmt(real, 2), UNID[i],
                                                                  fmt(x[i], 1)),
                color=TEXTO, ha="right", va="center", fontsize=9.5, linespacing=1.15)
    # saidas: probabilidade
    venc = int(np.argmax(p))
    for j in range(3):
        y0 = YS[3][j]
        ax.text(XS[3] + 0.5, y0 + 0.30, CLASSES[j], color=TEXTO, fontsize=11,
                fontweight="bold" if j == venc else "normal", va="center")
        ax.add_patch(Rectangle((XS[3] + 0.5, y0 - 0.32), 3.2, 0.26, fc="#eef1f3", ec="none", zorder=2))
        ax.add_patch(Rectangle((XS[3] + 0.5, y0 - 0.32), 3.2 * p[j], 0.26, fc=COR_CLASSE[j], ec="none", zorder=3))
        ax.text(XS[3] + 3.8, y0 - 0.19, "%d%%" % round(100 * p[j]), color=TEXTO, fontsize=10, va="center")
    ax.text(XS[3] + 0.5, YS[3][0] + 1.05, "Decisão do modelo: %s" % CLASSES[venc], color=TEXTO,
            fontsize=12, fontweight="bold",
            bbox=dict(boxstyle="round,pad=0.35", fc=COR_CLASSE[venc], ec="none", alpha=0.55))

    # titulos das camadas
    tit = ["ENTRADAS", "ESCONDIDA 1", "ESCONDIDA 2", "SAÍDA"]
    sub = ["o que o sensor mede", "8 neurônios\nachando padrões simples", "4 neurônios\ncombinando os padrões",
           "probabilidade de\ncada estado"]
    for l in range(4):
        ax.text(XS[l], 9.85, tit[l], color=TEXTO, ha="center", fontsize=11, fontweight="bold")
        ax.text(XS[l], 9.35, sub[l], color=SUB, ha="center", va="top", fontsize=8.5, linespacing=1.15)

    leg = [Line2D([0], [0], color=COR_POS, lw=3, label="peso positivo: reforça o sinal"),
           Line2D([0], [0], color=COR_NEG, lw=3, label="peso negativo: enfraquece o sinal"),
           Line2D([0], [0], color=SUB, lw=1, label="linha mais grossa = conexão mais importante"),
           Line2D([0], [0], marker="o", color="none", markerfacecolor=C_PETROLEO, markeredgecolor=TEXTO,
                  markersize=10, label="neurônio mais escuro = mais ativado (branco = desligado)")]
    ax.legend(handles=leg, loc="lower left", frameon=False, fontsize=8.5, labelcolor=TEXTO, bbox_to_anchor=(0.0, -0.02))
    if titulo_amostra:
        ax.text(-4.4, 10.35, titulo_amostra, color=TEXTO, fontsize=11, va="center", style="italic")


# ----------------------------------------------------------------------
# 3D (como no graphFeatures.py)
# ----------------------------------------------------------------------
_nz = limpos[limpos["classe_id"] > 0][["corrente", "potencia", "pf"]].values
LIM3D = [(_nz[:, k].min(), _nz[:, k].max()) for k in range(3)]


def estilo_3d(ax):
    ax.set_facecolor(FUNDO)
    for eixo in (ax.xaxis, ax.yaxis, ax.zaxis):
        eixo.set_pane_color((0.96, 0.97, 0.98, 1.0))
        eixo.label.set_color(TEXTO)
        eixo._axinfo["grid"].update(color=GRADE, linewidth=0.6)
    ax.tick_params(colors=SUB, labelsize=8)
    ax.set_xlabel("Corrente (A)")
    ax.set_ylabel("Potência (W)")
    ax.set_zlabel("Fator de potência")
    ax.set_xlim(LIM3D[0][0] - 0.1, LIM3D[0][1] + 0.1)
    ax.set_ylim(LIM3D[1][0] - 15, LIM3D[1][1] + 15)
    ax.set_zlim(max(0, LIM3D[2][0] - 0.02), min(1.0, LIM3D[2][1] + 0.02))


def grafico_3d_estatico():
    pts = limpos[["corrente", "potencia", "pf"]].values
    fig = plt.figure(figsize=(10, 9), facecolor=FUNDO)
    ax = fig.add_subplot(111, projection="3d")
    for c in (1, 2):
        m = yall == c
        if m.any():
            ax.scatter(pts[m, 0], pts[m, 1], pts[m, 2], s=11, c=COR_CLASSE[c], alpha=0.8,
                       label="%s (%d)" % (CLASSES[c], m.sum()))
    estilo_3d(ax)
    ax.set_title("Leituras reais da máquina em 3D, por estado\n(PARADA: só 3 leituras em ~0 A, fora de escala)", color=TEXTO)
    ax.legend(facecolor="white", edgecolor="#c5cdd3", labelcolor=TEXTO)
    ax.view_init(elev=22, azim=-58)
    fig.savefig(os.path.join(PASTA, "grafico_3d_variaveis.png"), dpi=140, facecolor=FUNDO)
    return fig


# ----------------------------------------------------------------------
# FIGURAS ESTATICAS DA ESTRUTURA
# ----------------------------------------------------------------------
def figura_estrutura(c):
    fig = plt.figure(figsize=(16, 8.5), facecolor=FUNDO)
    ax = fig.add_axes([0.01, 0.02, 0.98, 0.9])
    desenhar_rede(ax, Wb, bb, EXEMPLO[c], "Leitura real de exemplo: estado %s (modelo já treinado)" % CLASSES[c])
    fig.suptitle("Como o modelo decide: 6 números entram, 2 camadas de neurônios processam, 1 estado sai",
                 color=TEXTO, fontsize=14)
    fig.savefig(os.path.join(PASTA, "estrutura_rede_%s.png" % CLASSES[c]), dpi=120, facecolor=FUNDO)
    return fig


# ----------------------------------------------------------------------
# ANIMACAO DO TREINO
# ----------------------------------------------------------------------
def montar_figura():
    fig = plt.figure(figsize=(18, 9.5), facecolor=FUNDO)
    gs = fig.add_gridspec(2, 2, width_ratios=[1.55, 1.0], height_ratios=[1.25, 1.0],
                          left=0.01, right=0.985, top=0.9, bottom=0.11, wspace=0.04, hspace=0.18)
    ax_rede = fig.add_subplot(gs[:, 0])
    ax3 = fig.add_subplot(gs[0, 1], projection="3d")
    sub = gs[1, 1].subgridspec(1, 2, wspace=0.28)
    ax_l = fig.add_subplot(sub[0])
    ax_a = fig.add_subplot(sub[1])
    for a, t, yl in ((ax_l, "Erro (perda)", (0, max(HIST[:, 0].max(), HIST[:, 2].max()) * 1.05)),
                     (ax_a, "Acerto (acurácia)", (0, 1.02))):
        a.set_facecolor("#ffffff")
        a.set_title(t, color=TEXTO, fontsize=11)
        a.set_xlim(0, N_EP + 1)
        a.set_ylim(*yl)
        a.set_xlabel("época", color=TEXTO)
        a.tick_params(colors=SUB)
        a.grid(color=GRADE)
        for s in a.spines.values():
            s.set_color("#c5cdd3")
    l_tr, = ax_l.plot([], [], color=C_PETROLEO, lw=2, label="treino")
    l_va, = ax_l.plot([], [], color=C_CORAL, lw=2, label="validação")
    a_tr, = ax_a.plot([], [], color=C_PETROLEO, lw=2, label="treino")
    a_va, = ax_a.plot([], [], color=C_CORAL, lw=2, label="validação")
    ax_l.legend(facecolor="white", edgecolor="#c5cdd3", labelcolor=TEXTO, fontsize=9)
    fig.suptitle("Treinando o modelo (MLP 6→8→4→3) com os dados reais da máquina", color=TEXTO, fontsize=16, y=0.97)
    info = fig.text(0.5, 0.025, "", color=TEXTO, fontsize=13, ha="center", fontweight="bold")
    return fig, ax_rede, ax3, (l_tr, l_va, a_tr, a_va), info


def criar_animacao():
    fig, ax_rede, ax3, (l_tr, l_va, a_tr, a_va), info = montar_figura()
    idx = list(range(0, min(N_EP, 40) + 1)) + list(range(42, min(N_EP, 120) + 1, 2)) + list(range(125, N_EP + 1, 5))
    idx = [0] * 4 + idx + [N_EP] + [MELHOR] * 14
    arts = []

    def atualizar(k):
        i = idx[k]
        W, b = SNAPS[i]
        final = (k >= len(idx) - 14)
        if final:
            W, b = SNAPS[MELHOR]
            i = MELHOR
        desenhar_rede(ax_rede, W, b, EXEMPLO[2], "Leitura real de exemplo: estado USINANDO (o modelo tenta acertar)")
        for a in arts:
            a.remove()
        arts.clear()
        pred = frente(W, b, X3)[2].argmax(1)
        arts.append(ax3.scatter(P3[:, 0], P3[:, 1], P3[:, 2], s=9, c=[COR_CLASSE[c] for c in pred], alpha=0.9))
        err = pred != y3
        if err.any():
            arts.append(ax3.scatter(P3[err, 0], P3[err, 1], P3[err, 2], s=46, facecolors="none",
                                    edgecolors=COR_ERRO, linewidths=1.1))
        estilo_3d(ax3)
        ax3.set_title("Leituras reais: cor = o que o modelo prevê (anel escuro = errou)", color=TEXTO, fontsize=10)
        ax3.view_init(elev=22, azim=-58 + 0.55 * k)
        ep = range(i + 1)
        l_tr.set_data(ep, HIST[:i + 1, 0])
        l_va.set_data(ep, HIST[:i + 1, 2])
        a_tr.set_data(ep, HIST[:i + 1, 1])
        a_va.set_data(ep, HIST[:i + 1, 3])
        if i == 0:
            txt = "ANTES DE TREINAR: pesos aleatórios, o modelo chuta | acerto %d%%" % round(100 * HIST[0, 3])
        elif final:
            txt = ("FIM: melhores pesos (época %d, parada antecipada) | erro %s | acerto %s%% treino, %s%% validação"
                   % (MELHOR, fmt(HIST[i, 2], 3), fmt(100 * HIST[i, 1], 1), fmt(100 * HIST[i, 3], 1)))
        else:
            txt = ("Época %d | erro de treino %s, validação %s | acerto %s%% treino, %s%% validação"
                   % (i, fmt(HIST[i, 0], 3), fmt(HIST[i, 2], 3), fmt(100 * HIST[i, 1], 1), fmt(100 * HIST[i, 3], 1)))
        info.set_text(txt)

    return FuncAnimation(fig, atualizar, frames=len(idx), interval=1000 // FPS, repeat=True), fig


# ----------------------------------------------------------------------
# EXECUCAO
# ----------------------------------------------------------------------
fig3d = grafico_3d_estatico()
figs_rede = [figura_estrutura(1), figura_estrutura(2)]
print("PNGs salvos em", os.path.abspath(PASTA))

if SALVAR_GIF:
    print("Gerando o GIF do treino (pode levar 1-2 minutos)...")
    ani_gif, fig_gif = criar_animacao()
    ani_gif.save(os.path.join(PASTA, "treino_animado.gif"), writer=PillowWriter(fps=FPS), dpi=DPI_GIF,
                 savefig_kwargs={"facecolor": FUNDO})
    print("GIF salvo.")
    plt.close(fig_gif)

if not SEM_JANELA:
    ANI_VIVA, FIG_VIVA = criar_animacao()
    plt.show()
