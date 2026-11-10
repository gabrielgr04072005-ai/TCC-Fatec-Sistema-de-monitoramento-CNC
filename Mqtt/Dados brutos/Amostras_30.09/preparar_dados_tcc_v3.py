# -*- coding: utf-8 -*-
"""
preparar_dados_tcc_v3.py  (rodar no IDLE: F5)

ETAPA 1  Inspeciona os dados da planilha (exportada em CSV ou XLSX)
ETAPA 2  Gera os arquivos transformados, no padrao do documento do TCC:
         janela de 10 leituras -> rotulos por faixa de corrente
         -> remocao de outliers por MAD (dentro de cada classe)
         -> divisao treino/validacao/teste
         -> z-score (mu e sigma SO do treino) + limite de +-8

Saidas em ./saida_tcc/  (CSV com ; e virgula decimal, para o Excel BR)
Requer: pandas, numpy  (openpyxl se a planilha for .xlsx)
"""
import os
import re
import sys
import unicodedata
import numpy as np
import pandas as pd

# ----------------------------------------------------------------------
# CONFIGURACAO (ajuste aqui)
# ----------------------------------------------------------------------
# --- fonte dos dados ---
USAR_GOOGLE_SHEETS = True    # True = baixa direto da planilha; se falhar, abre janela p/ escolher arquivo
SHEET_ID = "1esfmA-kdfphSDEFigXBJj-vCAEmKxGErNw_JNbzc4zg"
GID = "1587900996"           # gid da aba (numero depois de gid= no link da aba)
DATA_INICIO = "2026-09-23"   # usa so leituras da maquina real a partir desta data
DATA_FIM = ""                # vazio = ate a ultima leitura da planilha (hoje)
ARQUIVO = ""                 # opcional: caminho fixo de um CSV/XLSX
PASTA_SAIDA = "saida_tcc"

JANELA = 10                  # leituras na janela movel
GAP_MAX_S = 60               # intervalo maior que isso quebra a janela (perda de comunicacao)

# faixas de corrente (A) - folha de dados WEG, 380 V (documento do TCC)
LIM_PARADA = 0.5             # abaixo disso = PARADA (~0 A)
LIM_VAZIO = 4.0              # ate 4,0 A = VAZIO
LIM_SOBRECARGA = 5.3         # acima de 5,3 A = SOBRECARGA
LIM_CRITICO = 7.2            # a partir de 7,2 A = CRITICO

# limites fisicos: acima disso a leitura e erro do PZEM (ex.: corrente de 302 mil A)
LIM_I_MAX = 15.0             # A   (motor de 4,78 A nominal)
LIM_P_MAX = 5000.0           # W
LIM_V_MAX = 300.0            # V

MAD_VARS = ("corrente", "potencia")   # Pf fica de fora: so tem 2 casas decimais (MAD=0,01) e o MAD
                                      # cortaria a variacao legitima do Pf perto da fronteira VAZIO/USINANDO
MAD_K = 3.5                  # limite do z-score modificado (Iglewicz-Hoaglin)
CLIP_Z = 8.0                 # z limitado a +-8
FRAC_TREINO, FRAC_VAL = 0.70, 0.15   # o resto e teste
BLOCO = 60                   # linhas seguidas por bloco (evita vazamento de janelas vizinhas)
SEMENTE = 42

VARS = ["tensao", "corrente", "potencia", "energia", "frequencia", "pf"]
FEATURES = ["corrente", "potencia", "pf",
            "delta_corrente", "media_corrente", "desvio_corrente"]   # 6 entradas
CLASSES_MLP = ["PARADA", "VAZIO", "USINANDO"]                        # saidas da rede

ALIASES = {
    "tensao": ["tensao", "voltage"],
    "corrente": ["corrente", "current"],
    "potencia": ["potencia", "power"],
    "energia": ["energia", "energy"],
    "frequencia": ["frequencia", "frequency"],
    "pf": ["pf", "fatordepotencia", "fp", "powerfactor"],
    "timestamp": ["timestamp", "datahora", "datetime"],
    "data": ["data", "date"],
    "hora": ["hora", "time"],
}


# ----------------------------------------------------------------------
# UTILITARIOS
# ----------------------------------------------------------------------
def titulo(txt):
    print("\n" + "=" * 70)
    print(txt)
    print("=" * 70)


def norm(txt):
    t = unicodedata.normalize("NFKD", str(txt)).encode("ascii", "ignore").decode()
    return re.sub(r"[^a-z0-9]", "", t.lower())


def baixar_google_sheets():
    """Baixa a aba (gid) como CSV. Exige a planilha compartilhada como
    'Qualquer pessoa com o link: Leitor'."""
    import urllib.request
    url = ("https://docs.google.com/spreadsheets/d/%s/export?format=csv&gid=%s"
           % (SHEET_ID, GID))
    os.makedirs(PASTA_SAIDA, exist_ok=True)
    destino = os.path.join(PASTA_SAIDA, "planilha_baixada.csv")
    with urllib.request.urlopen(url, timeout=60) as r:
        dados = r.read()
    if dados[:15].lstrip().lower().startswith(b"<!doctype") or b"<html" in dados[:300].lower():
        raise RuntimeError("o Google devolveu uma pagina de login (planilha nao esta compartilhada por link)")
    with open(destino, "wb") as f:
        f.write(dados)
    print("Baixado da planilha:", destino, "(%d bytes)" % len(dados))
    return destino


def escolher_arquivo():
    if len(sys.argv) > 1:
        return sys.argv[1]
    if ARQUIVO:
        return ARQUIVO
    if USAR_GOOGLE_SHEETS:
        try:
            return baixar_google_sheets()
        except Exception as e:
            print("Nao consegui baixar do Google Sheets:", e)
            print("Escolha o arquivo exportado manualmente.")
    import tkinter as tk
    from tkinter import filedialog
    raiz = tk.Tk()
    raiz.withdraw()
    raiz.attributes("-topmost", True)
    caminho = filedialog.askopenfilename(
        title="Escolha a planilha exportada (CSV ou XLSX)",
        filetypes=[("Planilhas", "*.csv *.xlsx *.xls"), ("Todos", "*.*")])
    raiz.destroy()
    if not caminho:
        sys.exit("Nenhum arquivo selecionado.")
    return caminho


def ler_planilha(caminho):
    ext = os.path.splitext(caminho)[1].lower()
    if ext in (".xlsx", ".xls"):
        xls = pd.ExcelFile(caminho)
        print("Abas encontradas:", xls.sheet_names)
        aba = next((a for a in xls.sheet_names if norm(a) == "historico"), xls.sheet_names[0])
        print("Usando a aba:", aba)
        return pd.read_excel(xls, sheet_name=aba)
    baixado = os.path.basename(caminho) == "planilha_baixada.csv"
    for enc in ("utf-8-sig", "latin-1"):
        try:
            if baixado:
                return pd.read_csv(caminho, sep=",", encoding=enc)
            return pd.read_csv(caminho, sep=None, engine="python", encoding=enc)
        except UnicodeDecodeError:
            continue
    raise ValueError("Nao consegui ler o CSV.")


def para_numero(s):
    """Converte coluna para float aceitando '3,8' e '1.234,5'."""
    if s.dtype.kind in "if":
        return s.astype(float)
    t = s.astype(str).str.strip().str.replace(" ", "", regex=False)
    tem_virgula = t.str.contains(",", regex=False)
    t = t.where(~tem_virgula,
                t.str.replace(".", "", regex=False).str.replace(",", ".", regex=False))
    return pd.to_numeric(t, errors="coerce")


def mapear_colunas(df):
    achadas = {}
    for chave, nomes in ALIASES.items():
        for c in df.columns:
            n = norm(c)
            if any(n.startswith(a) for a in nomes):
                achadas[chave] = c
                break
    return achadas


def montar_tabela(df, col):
    num = pd.DataFrame(index=df.index)
    for v in VARS:
        if v in col:
            num[v] = para_numero(df[col[v]])
    ts = None
    if "timestamp" in col:
        ts = pd.to_datetime(df[col["timestamp"]], dayfirst=True, errors="coerce")
    elif "data" in col and "hora" in col:
        d = pd.to_datetime(df[col["data"]], dayfirst=True, errors="coerce").dt.strftime("%Y-%m-%d")
        h = df[col["hora"]].astype(str).str.extract(r"(\d{1,2}:\d{2}(?::\d{2})?)")[0]
        ts = pd.to_datetime(d + " " + h, errors="coerce")
    num["ts"] = ts if ts is not None else pd.NaT
    return num, ts is not None


# ----------------------------------------------------------------------
# ETAPA 1 - INSPECAO
# ----------------------------------------------------------------------
def inspecionar(df, col, num, tem_ts):
    titulo("ETAPA 1 - INSPECAO DA PLANILHA")
    print("Linhas:", len(df), "| Colunas:", len(df.columns))
    print("Colunas:", list(df.columns))
    print("Mapeamento (padrao -> coluna da planilha):")
    for k, v in col.items():
        print("   ", k, "->", v)
    faltando = [v for v in ("corrente", "potencia", "pf") if v not in col]
    if faltando:
        sys.exit("ERRO: nao achei as colunas: " + ", ".join(faltando))

    print("\nPrimeiras linhas:")
    print(df.head(5).to_string())

    cols = [v for v in VARS if v in num.columns]
    print("\nResumo estatistico:")
    print(num[cols].describe().round(3).to_string())

    print("\nValores vazios/invalidos por variavel:")
    print(num[cols].isna().sum().to_string())

    print("\nCorrente = 0 :", int((num["corrente"] == 0).sum()),
          "| Corrente >= 4,0 A :", int((num["corrente"] >= LIM_VAZIO).sum()))

    print("\nCorrelacao com a corrente:")
    print(num[cols].corr()["corrente"].round(3).to_string())

    if tem_ts:
        t = num["ts"].dropna().sort_values()
        print("\nPeriodo:", t.min(), "ate", t.max())
        dt = t.diff().dt.total_seconds().dropna()
        print("Intervalo tipico entre leituras (mediana): %.1f s" % dt.median())
        print("Buracos > %d s: %d (maior: %.0f s)" % (GAP_MAX_S, int((dt > GAP_MAX_S).sum()), dt.max()))
        print("Leituras por dia:")
        print(t.dt.date.value_counts().sort_index().to_string())
        print("Linhas duplicadas no timestamp:", int(t.duplicated().sum()))
    else:
        print("\nAVISO: sem data/hora reconhecida; usando a ordem das linhas.")


# ----------------------------------------------------------------------
# ETAPA 2 - TRANSFORMACAO
# ----------------------------------------------------------------------
def rotular(i):
    return np.select(
        [i < LIM_PARADA, i <= LIM_VAZIO, i < LIM_SOBRECARGA, i < LIM_CRITICO],
        ["PARADA", "VAZIO", "USINANDO", "SOBRECARGA"], default="CRITICO")


def criar_janelas(num, tem_ts):
    d = num[["ts", "corrente", "potencia", "pf"]].copy()
    d = d.dropna(subset=["corrente", "potencia", "pf"])
    # leituras fisicamente impossiveis (ex.: Pf > 1, potencia negativa) = lixo do sensor/planilha
    validas = (d["pf"].between(0, 1) & (d["potencia"] >= 0) & (d["corrente"] >= 0))
    validas &= (d["corrente"] <= LIM_I_MAX) & (d["potencia"] <= LIM_P_MAX)
    if "tensao" in num.columns:
        validas &= ~(num.loc[d.index, "tensao"] > LIM_V_MAX)
    print("Leituras invalidas removidas (fora dos limites fisicos): %d" % int((~validas).sum()))
    d = d[validas]
    if tem_ts:
        d = d.dropna(subset=["ts"]).sort_values("ts").drop_duplicates("ts")
        if DATA_INICIO:
            d = d[d["ts"] >= pd.Timestamp(DATA_INICIO)]
        if DATA_FIM:
            d = d[d["ts"] < pd.Timestamp(DATA_FIM) + pd.Timedelta(days=1)]
        print("Periodo usado: %s ate %s | %d leituras validas" % (d["ts"].min(), d["ts"].max(), len(d)))
        print("Leituras por dia:")
        print(d["ts"].dt.date.value_counts().sort_index().to_string())
        d["seg"] = (d["ts"].diff().dt.total_seconds() > GAP_MAX_S).cumsum()
    else:
        d["seg"] = 0
    d = d.reset_index(drop=True)
    g = d.groupby("seg")["corrente"]
    d["delta_corrente"] = g.diff()
    d["media_corrente"] = g.transform(lambda s: s.rolling(JANELA, min_periods=JANELA).mean())
    # ddof=0: desvio-padrao populacional, mesma conta que o firmware faz
    # janela sem variacao da std=0 (nao NaN), entao nenhuma linha e perdida
    d["desvio_corrente"] = g.transform(lambda s: s.rolling(JANELA, min_periods=JANELA).std(ddof=0))
    antes = len(d)
    d = d.dropna(subset=["delta_corrente", "media_corrente", "desvio_corrente"]).reset_index(drop=True)
    print("Janela de %d leituras: %d linhas -> %d (perdidas so no inicio de cada trecho: %d trechos)"
          % (JANELA, antes, len(d), d["seg"].nunique()))
    d["estado"] = rotular(d["corrente"].values)
    return d


def mad_filtrar(d):
    """Remove outliers por MAD dentro de cada classe (variaveis em MAD_VARS)."""
    manter = pd.Series(True, index=d.index)
    relatorio = []
    for classe in CLASSES_MLP:
        idx = d.index[d["estado"] == classe]
        for v in MAD_VARS:
            x = d.loc[idx, v].values
            if len(x) == 0:
                continue
            med = np.median(x)
            mad = np.median(np.abs(x - med))
            if mad == 0:
                relatorio.append((classe, v, med, mad, 0, "MAD=0, nao aplicado"))
                continue
            mz = 0.6745 * (x - med) / mad
            fora = np.abs(mz) > MAD_K
            manter.loc[idx[fora]] = False
            relatorio.append((classe, v, med, mad, int(fora.sum()), ""))
    titulo("REMOCAO DE OUTLIERS POR MAD (|z modificado| > %.1f, por classe)" % MAD_K)
    print("%-10s %-9s %10s %10s %9s  %s" % ("classe", "variavel", "mediana", "MAD", "removidos", "obs"))
    for r in relatorio:
        print("%-10s %-9s %10.4f %10.4f %9d  %s" % r)
    print("\nSOBRECARGA e CRITICO nao passam pelo MAD (raros por definicao; alarme por limiar fixo).")
    return d[manter].reset_index(drop=True), d[~manter].reset_index(drop=True)


def dividir(estados):
    """Divide em blocos de linhas seguidas, separadamente dentro de cada classe,
    para que todos os conjuntos tenham todas as classes e janelas vizinhas
    nao vazem do treino para o teste."""
    rng = np.random.default_rng(SEMENTE)
    conjunto = np.empty(len(estados), dtype=object)
    for classe in np.unique(estados):
        pos = np.where(estados == classe)[0]
        tam = BLOCO if len(pos) >= 6 * BLOCO else max(5, len(pos) // 10)
        bloco = np.arange(len(pos)) // tam
        ids = np.unique(bloco)
        rng.shuffle(ids)
        n_tr = int(round(len(ids) * FRAC_TREINO))
        n_va = int(round(len(ids) * FRAC_VAL))
        mapa = {}
        for k, b in enumerate(ids):
            mapa[b] = "treino" if k < n_tr else ("validacao" if k < n_tr + n_va else "teste")
        conjunto[pos] = [mapa[b] for b in bloco]
    return conjunto


def salvar(df, nome, pasta):
    caminho = os.path.join(pasta, nome)
    df.to_csv(caminho, sep=";", decimal=",", index=False, encoding="utf-8-sig")
    print("  salvo:", caminho, "(%d linhas)" % len(df))


def zscore(df, mu, sigma):
    return ((df[FEATURES] - mu) / sigma).clip(-CLIP_Z, CLIP_Z)


def transformar(num, tem_ts):
    titulo("ETAPA 2 - TRANSFORMACAO")
    os.makedirs(PASTA_SAIDA, exist_ok=True)

    d = criar_janelas(num, tem_ts)
    print("\nDistribuicao dos estados (antes do MAD):")
    print(d["estado"].value_counts().to_string())

    mlp = d[d["estado"].isin(CLASSES_MLP)].reset_index(drop=True)
    alarme = d[~d["estado"].isin(CLASSES_MLP)].reset_index(drop=True)

    mlp, removidos = mad_filtrar(mlp)
    print("\nOutliers removidos:", len(removidos), "| restaram:", len(mlp))

    mlp["classe_id"] = mlp["estado"].map({c: i for i, c in enumerate(CLASSES_MLP)})
    mlp["conjunto"] = dividir(mlp["estado"].values)

    tr = mlp[mlp["conjunto"] == "treino"]
    va = mlp[mlp["conjunto"] == "validacao"]
    te = mlp[mlp["conjunto"] == "teste"]

    print("\nClasses por conjunto:")
    print(pd.crosstab(mlp["conjunto"], mlp["estado"]).to_string())
    for nome, parte in (("treino", tr), ("validacao", va), ("teste", te)):
        falta = [c for c in CLASSES_MLP if (parte["estado"] == c).sum() == 0]
        if falta:
            print("AVISO: conjunto de %s sem exemplos de %s (colete mais dados)" % (nome, falta))

    # mu e sigma calculados APENAS no treino
    mu = tr[FEATURES].mean()
    sigma = tr[FEATURES].std(ddof=0).replace(0, 1.0)

    titulo("PARAMETROS Z-SCORE (so do treino) -> vao para o firmware")
    par = pd.DataFrame({"variavel": FEATURES, "mu": mu.values, "sigma": sigma.values})
    print(par.round(6).to_string(index=False))

    print("\nArquivos gerados:")
    salvar(mlp.drop(columns=["seg", "ts"]).assign(ts=mlp["ts"]), "01_dados_limpos_MAD.csv", PASTA_SAIDA)
    for nome, parte in (("02_treino_z.csv", tr), ("03_validacao_z.csv", va), ("04_teste_z.csv", te)):
        z = zscore(parte, mu, sigma)
        z["estado"] = parte["estado"].values
        z["classe_id"] = parte["classe_id"].values
        salvar(z, nome, PASTA_SAIDA)

    if len(alarme):
        a = zscore(alarme, mu, sigma)
        a["estado"] = alarme["estado"].values
        a["corrente_A"] = alarme["corrente"].values
        salvar(a, "05_alarme_limiar_z.csv", PASTA_SAIDA)
    salvar(removidos.drop(columns=["seg"]), "06_outliers_removidos.csv", PASTA_SAIDA)
    salvar(par, "parametros_zscore.csv", PASTA_SAIDA)

    # constantes em C para colar no firmware
    caminho_h = os.path.join(PASTA_SAIDA, "parametros_zscore.h")
    with open(caminho_h, "w", encoding="utf-8") as f:
        f.write("// gerado por preparar_dados_tcc_v3.py - mu e sigma do conjunto de treino\n")
        f.write("// ordem: " + ", ".join(FEATURES) + "\n")
        f.write("#define ZS_N %d\n#define ZS_CLIP %.1ff\n" % (len(FEATURES), CLIP_Z))
        f.write("const float ZS_MU[ZS_N] = {" + ", ".join("%.8ff" % v for v in mu.values) + "};\n")
        f.write("const float ZS_SIGMA[ZS_N] = {" + ", ".join("%.8ff" % v for v in sigma.values) + "};\n")
    print("  salvo:", caminho_h)


# ----------------------------------------------------------------------
# PRINCIPAL
# ----------------------------------------------------------------------
def main():
    caminho = escolher_arquivo()
    print("Arquivo:", caminho)
    df = ler_planilha(caminho)
    col = mapear_colunas(df)
    num, tem_ts = montar_tabela(df, col)
    inspecionar(df, col, num, tem_ts)
    transformar(num, tem_ts)
    titulo("PRONTO - confira os arquivos na pasta: " + os.path.abspath(PASTA_SAIDA))


main()
