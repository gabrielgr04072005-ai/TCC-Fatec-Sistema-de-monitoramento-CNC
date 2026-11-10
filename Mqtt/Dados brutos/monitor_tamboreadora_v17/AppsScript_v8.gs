// ================================================================
//   Monitor de Máquina — TCC FATEC
//   Google Apps Script v8
//   - doPost : recebe dados do ESP32 (corrente, tensão, vibração, temp)
//   - doGet  : retorna configuração (G7) [mantido, sem uso no ESP32 nesta fase]
//   - Histórico : arquiva a cada 1000 leituras OU quando muda o dia
//   - Dashboard : gráficos e métricas automáticas
//
//   NOVO NA v8: colunas para Tensão, Vibração X/Y/Z e Temperatura,
//   alinhadas com a planilha (ordem: Data, Hora, Corrente, Tensão,
//   VibX, VibY, VibZ, Temperatura, Nível, Estado).
// ================================================================

var LINHA_INICIO   = 7;
var MAX_LEITURAS   = 1000;
var ABA_LEITURAS   = "Leituras";
var PREFIXO_HISTORICO = "Histórico ";  // vira "Histórico Julho", "Histórico Agosto", etc.
var ABA_DASHBOARD  = "Dashboard";

// Quantidade de colunas de dados (sem contar "Origem", usada só no histórico)
var NUM_COLUNAS_DADOS = 10; // Data,Hora,Corrente,Tensao,VibX,VibY,VibZ,Temperatura,Nivel,Estado

var NOMES_MESES = ["Janeiro","Fevereiro","Março","Abril","Maio","Junho",
                    "Julho","Agosto","Setembro","Outubro","Novembro","Dezembro"];

// ================================================================
//   NOME DA ABA DE HISTORICO PARA UMA DATA "dd/MM/yyyy"
// ================================================================
function obterNomeAbaHistorico(dataStr) {
  var partes = String(dataStr).split("/");
  var mesIndex = parseInt(partes[1], 10) - 1; // "MM" -> indice 0-based
  var nomeMes = NOMES_MESES[mesIndex] || "Desconhecido";
  return PREFIXO_HISTORICO + nomeMes;
}

// ================================================================
//   RECEBE DADOS DO ESP32 (POST)
// ================================================================
function doPost(e) {
  try {
    var dados;
    try {
      dados = JSON.parse(e.postData.contents);
    } catch(err) {
      dados = e.parameter;
    }

    var irms        = parseFloat(dados.irms)        || 0.0;
    var tensao      = parseFloat(dados.tensao)      || 0.0;
    var vibX        = parseFloat(dados.vibX)        || 0.0;
    var vibY        = parseFloat(dados.vibY)        || 0.0;
    var vibZ        = parseFloat(dados.vibZ)        || 0.0;
    var temperatura = parseFloat(dados.temperatura) || 0.0;
    var nivel       = (dados.nivel  !== undefined) ? String(dados.nivel)  : "BRUTO";
    var estado      = (dados.estado !== undefined) ? String(dados.estado) : "Leitura bruta";

    var ss  = SpreadsheetApp.getActiveSpreadsheet();
    var aba = ss.getSheetByName(ABA_LEITURAS);

    var agora = new Date();
    var data  = Utilities.formatDate(agora, "America/Sao_Paulo", "dd/MM/yyyy");
    var hora  = Utilities.formatDate(agora, "America/Sao_Paulo", "HH:mm:ss");

    // -----------------------------------------------------------
    // Se a data mudou desde a ultima leitura gravada, arquiva tudo
    // que esta na aba "Leituras" para o "Historico" ANTES de gravar
    // a leitura de hoje. Isso garante que a aba "Leituras" sempre
    // mostra so o dia atual.
    // -----------------------------------------------------------
    verificarMudancaDeDia(ss, aba, data);

    var ultimaLinha = aba.getLastRow();
    var novaLinha   = Math.max(ultimaLinha + 1, LINHA_INICIO);

    // Data,Hora,Corrente,Tensao,VibX,VibY,VibZ,Temperatura,Nivel,Estado
    aba.getRange(novaLinha, 1).setValue(data);
    aba.getRange(novaLinha, 2).setValue(hora);
    aba.getRange(novaLinha, 3).setValue(irms);
    aba.getRange(novaLinha, 4).setValue(tensao);
    aba.getRange(novaLinha, 5).setValue(vibX);
    aba.getRange(novaLinha, 6).setValue(vibY);
    aba.getRange(novaLinha, 7).setValue(vibZ);
    aba.getRange(novaLinha, 8).setValue(temperatura);
    aba.getRange(novaLinha, 9).setValue(nivel);
    aba.getRange(novaLinha, 10).setValue(estado);

    aba.getRange(novaLinha, 3).setNumberFormat("0.0000");  // Corrente
    aba.getRange(novaLinha, 4).setNumberFormat("0.00");    // Tensao
    aba.getRange(novaLinha, 8).setNumberFormat("0.00");    // Temperatura

    var cor = obterCor(nivel);
    aba.getRange(novaLinha, 1, 1, NUM_COLUNAS_DADOS).setBackground(cor);
    aba.getRange(novaLinha, 1, 1, NUM_COLUNAS_DADOS).setFontColor(obterCorFonte(nivel));

    // Arquiva tambem se bater 1000 leituras no mesmo dia (seguranca)
    var totalLeituras = novaLinha - LINHA_INICIO + 1;
    if (totalLeituras >= MAX_LEITURAS) {
      arquivarHistorico(ss, aba);
    }

    // Atualiza dashboard
    atualizarDashboard(ss);

    return ContentService
      .createTextOutput(JSON.stringify({status: "OK", linha: novaLinha}))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (erro) {
    return ContentService
      .createTextOutput(JSON.stringify({status: "ERRO", msg: erro.toString()}))
      .setMimeType(ContentService.MimeType.JSON);
  }
}

// ================================================================
//   VERIFICA SE O DIA MUDOU E ARQUIVA O DIA ANTERIOR
// ================================================================
function verificarMudancaDeDia(ss, aba, dataAtualStr) {
  var ultimaLinha = aba.getLastRow();

  // Aba vazia (so cabecalho) -> nao ha nada pra arquivar
  if (ultimaLinha < LINHA_INICIO) return;

  var valorDataUltima = aba.getRange(ultimaLinha, 1).getValue();
  var dataUltimaStr;

  if (valorDataUltima instanceof Date) {
    dataUltimaStr = Utilities.formatDate(valorDataUltima, "America/Sao_Paulo", "dd/MM/yyyy");
  } else {
    dataUltimaStr = String(valorDataUltima).trim();
  }

  // Linha em branco (ex: aba recem-criada) -> nao arquiva
  if (dataUltimaStr === "") return;

  if (dataUltimaStr !== dataAtualStr) {
    Logger.log("Mudanca de dia detectada: " + dataUltimaStr + " -> " + dataAtualStr + ". Arquivando dia anterior...");
    arquivarHistorico(ss, aba);
  }
}

// ================================================================
//   RETORNA CONFIGURAÇÃO PARA O ESP32 (GET)
//   Mantido por compatibilidade — não usado pelo firmware nesta fase.
// ================================================================
function doGet(e) {
  try {
    var acao = e.parameter.action || "";

    if (acao == "config") {
      var ss  = SpreadsheetApp.getActiveSpreadsheet();
      var aba = ss.getSheetByName(ABA_LEITURAS);
      var referencia = parseFloat(aba.getRange("G7").getValue()) || 0.0;

      return ContentService
        .createTextOutput(JSON.stringify({status: "OK", referencia: referencia}))
        .setMimeType(ContentService.MimeType.JSON);
    }

    return ContentService
      .createTextOutput(JSON.stringify({status: "online"}))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (erro) {
    return ContentService
      .createTextOutput(JSON.stringify({status: "ERRO", msg: erro.toString()}))
      .setMimeType(ContentService.MimeType.JSON);
  }
}

// ================================================================
//   ARQUIVA HISTÓRICO (chamado a cada 1000 leituras OU virada de dia)
//   Separa automaticamente por mes, em abas
//   "Histórico Janeiro", "Histórico Fevereiro", etc.
// ================================================================
function arquivarHistorico(ss, abaLeituras) {
  var ultimaLinha  = abaLeituras.getLastRow();
  var totalLinhas  = ultimaLinha - LINHA_INICIO + 1;
  if (totalLinhas <= 0) return;

  var dados = abaLeituras.getRange(LINHA_INICIO, 1, totalLinhas, NUM_COLUNAS_DADOS).getValues();
  var agora = new Date();
  var origem = Utilities.formatDate(agora, "America/Sao_Paulo", "dd/MM/yyyy HH:mm");

  // Agrupa as linhas por mes (normalmente sera so 1 grupo, mas cobre
  // o caso raro de um arquivamento acontecer bem na virada do mes)
  var gruposPorMes = {};
  dados.forEach(function(row) {
    var dataStr = row[0] instanceof Date
      ? Utilities.formatDate(row[0], "America/Sao_Paulo", "dd/MM/yyyy")
      : String(row[0]);
    var nomeAba = obterNomeAbaHistorico(dataStr);
    if (!gruposPorMes[nomeAba]) gruposPorMes[nomeAba] = [];
    // row tem 10 colunas (Data..Estado); adiciona Origem no final -> 11
    gruposPorMes[nomeAba].push(row.concat([origem]));
  });

  Object.keys(gruposPorMes).forEach(function(nomeAba) {
    var abaHist = ss.getSheetByName(nomeAba);
    if (!abaHist) {
      abaHist = ss.insertSheet(nomeAba);
      var cabecalho = ["Data", "Hora", "Corrente (A)", "Tensão (V)",
                        "Vibração X", "Vibração Y", "Vibração Z",
                        "Temperatura (°C)", "Nível", "Estado", "Origem"];
      abaHist.getRange(1, 1, 1, cabecalho.length).setValues([cabecalho]);
      abaHist.getRange(1, 1, 1, cabecalho.length).setFontWeight("bold");
      abaHist.getRange(1, 1, 1, cabecalho.length).setBackground("#333333");
      abaHist.getRange(1, 1, 1, cabecalho.length).setFontColor("#FFFFFF");
    }

    var dadosComOrigem = gruposPorMes[nomeAba];
    var ultimaHistorico = abaHist.getLastRow() + 1;
    abaHist.getRange(ultimaHistorico, 1, dadosComOrigem.length, dadosComOrigem[0].length)
      .setValues(dadosComOrigem);

    for (var i = 0; i < dadosComOrigem.length; i++) {
      var nivel = dadosComOrigem[i][8];  // coluna 9 (indice 8) = Nivel
      var linha = ultimaHistorico + i;
      abaHist.getRange(linha, 1, 1, NUM_COLUNAS_DADOS).setBackground(obterCor(nivel));
      abaHist.getRange(linha, 1, 1, NUM_COLUNAS_DADOS).setFontColor(obterCorFonte(nivel));
    }

    Logger.log(nomeAba + ": " + dadosComOrigem.length + " leituras arquivadas.");
  });

  // Limpa aba Leituras mantendo cabeçalho
  abaLeituras.getRange(LINHA_INICIO, 1, totalLinhas, NUM_COLUNAS_DADOS).clearContent();
  abaLeituras.getRange(LINHA_INICIO, 1, totalLinhas, NUM_COLUNAS_DADOS).clearFormat();
}

// ================================================================
//   ATUALIZA DASHBOARD
// ================================================================
function atualizarDashboard(ss) {
  var abaDash = ss.getSheetByName(ABA_DASHBOARD);
  if (!abaDash) {
    abaDash = ss.insertSheet(ABA_DASHBOARD);
  }
  abaDash.clearContents();
  abaDash.clearFormats();

  var abaLeit = ss.getSheetByName(ABA_LEITURAS);

  // Coleta todos os dados (leituras atuais + TODAS as abas de historico mensal)
  var todosDados = [];

  var ultimaLeit = abaLeit.getLastRow();
  if (ultimaLeit >= LINHA_INICIO) {
    var dadosLeit = abaLeit.getRange(LINHA_INICIO, 1, ultimaLeit - LINHA_INICIO + 1, NUM_COLUNAS_DADOS).getValues();
    todosDados = todosDados.concat(dadosLeit);
  }

  // Percorre todas as abas cujo nome comeca com "Histórico "
  var todasAsAbas = ss.getSheets();
  todasAsAbas.forEach(function(aba) {
    if (aba.getName().indexOf(PREFIXO_HISTORICO) === 0) {
      var ultimaHist = aba.getLastRow();
      if (ultimaHist >= 2) {
        // historico tem NUM_COLUNAS_DADOS colunas de dados + Origem;
        // aqui só precisamos das colunas de dados (as primeiras NUM_COLUNAS_DADOS)
        var dadosHist = aba.getRange(2, 1, ultimaHist - 1, NUM_COLUNAS_DADOS).getValues();
        todosDados = todosDados.concat(dadosHist);
      }
    }
  });

  if (todosDados.length == 0) {
    abaDash.getRange(1,1).setValue("Sem dados ainda.");
    return;
  }

  // Índices (0-based) nas linhas de todosDados:
  // 0 Data | 1 Hora | 2 Corrente | 3 Tensao | 4 VibX | 5 VibY | 6 VibZ
  // 7 Temperatura | 8 Nivel | 9 Estado

  // ── Cabeçalho do dashboard ──────────────────────────────────
  abaDash.getRange("A1:H1").merge();
  abaDash.getRange("A1").setValue("Dashboard — Sistema de Monitoramento de Máquina");
  abaDash.getRange("A1").setFontSize(16).setFontWeight("bold").setHorizontalAlignment("center");
  abaDash.getRange("A1:H1").setBackground("#1a1a2e").setFontColor("#FFFFFF");

  abaDash.getRange("A2:H2").merge();
  abaDash.getRange("A2").setValue("TCC FATEC — Manufatura Avançada");
  abaDash.getRange("A2").setFontSize(11).setHorizontalAlignment("center").setFontColor("#888888");

  // ── Filtro por período (instrução) ──────────────────────────
  abaDash.getRange("A4").setValue("Filtrar por data:");
  abaDash.getRange("A4").setFontWeight("bold").setFontColor("#333333");
  abaDash.getRange("B4").setValue("Início:");
  abaDash.getRange("C4").setValue("").setBackground("#FFFDE7");
  abaDash.getRange("C4").setNote("Digite a data inicial no formato dd/MM/yyyy");
  abaDash.getRange("D4").setValue("Fim:");
  abaDash.getRange("E4").setValue("").setBackground("#FFFDE7");
  abaDash.getRange("E4").setNote("Digite a data final no formato dd/MM/yyyy");
  abaDash.getRange("B4:E4").setBorder(true, true, true, true, true, true);

  // ── Métricas gerais (corrente) ────────────────────────────────
  // Filtra apenas valores válidos (0.05A a 0.50A) — mesmo critério de antes
  var correntes = todosDados.map(function(r){ return parseFloat(r[2]); }).filter(function(v){ return !isNaN(v) && v >= 0.05 && v <= 0.50; });
  var media     = correntes.reduce(function(a,b){ return a+b; }, 0) / (correntes.length || 1);
  var variancia = correntes.reduce(function(a,b){ return a + Math.pow(b - media, 2); }, 0) / (correntes.length || 1);
  var desvio    = Math.sqrt(variancia);

  var contNiveis = {};
  todosDados.forEach(function(r) {
    var n = r[8]; // Nivel
    contNiveis[n] = (contNiveis[n] || 0) + 1;
  });

  var anomalias = (contNiveis["VERMELHO ALTO"] || 0) +
                  (contNiveis["LARANJA ALTO"]  || 0) +
                  (contNiveis["VERMELHO BAIXO"]|| 0) +
                  (contNiveis["LARANJA BAIXO"] || 0);

  var normalPct = Math.round(((contNiveis["VERDE - NORMAL"] || 0) / todosDados.length) * 100);

  // Cards de métricas
  var cards = [
    ["Média Corrente", media.toFixed(4) + " A",  "#E3F2FD", "#1565C0"],
    ["Desvio Padrão",  desvio.toFixed(4),         "#FFF8E1", "#F57F17"],
    ["Total Anomalias",anomalias,                  "#FFEBEE", "#C62828"],
    ["% Normal",       normalPct + "%",            "#E8F5E9", "#1B5E20"],
    ["Corrente Máx",   Math.max.apply(null, correntes).toFixed(4) + " A", "#FCE4EC", "#880E4F"],
    ["Corrente Mín",   Math.min.apply(null, correntes).toFixed(4) + " A", "#E8EAF6", "#283593"],
  ];

  var colCard = 1;
  cards.forEach(function(card) {
    abaDash.getRange(6, colCard, 1, 2).merge();
    abaDash.getRange(6, colCard).setValue(card[1]);
    abaDash.getRange(6, colCard).setFontSize(20).setFontWeight("bold").setFontColor(card[3]).setHorizontalAlignment("center");
    abaDash.getRange(6, colCard).setBackground(card[2]);

    abaDash.getRange(7, colCard, 1, 2).merge();
    abaDash.getRange(7, colCard).setValue(card[0]);
    abaDash.getRange(7, colCard).setFontSize(10).setFontColor(card[3]).setHorizontalAlignment("center");
    abaDash.getRange(7, colCard).setBackground(card[2]);

    abaDash.getRange(6, colCard, 2, 2).setBorder(true, true, true, true, false, false);
    colCard += 2;
  });

  // ── Distribuição por nível ───────────────────────────────────
  abaDash.getRange("A9:D9").merge();
  abaDash.getRange("A9").setValue("Qtd Leituras por Nível");
  abaDash.getRange("A9").setFontSize(13).setFontWeight("bold");
  abaDash.getRange("A9:H9").setBackground("#F5F5F5");

  var niveis = [
    ["VERDE - NORMAL",    "#27AE60", "#FFFFFF"],
    ["AMARELO BAIXO",     "#F1C40F", "#7D6608"],
    ["AMARELO ALTO",      "#F39C12", "#784212"],
    ["LARANJA BAIXO",     "#E67E22", "#FFFFFF"],
    ["LARANJA ALTO",      "#D35400", "#FFFFFF"],
    ["VERMELHO BAIXO",    "#C0392B", "#FFFFFF"],
    ["VERMELHO ALTO",     "#922B21", "#FFFFFF"],
    ["MAQUINA DESLIGADA", "#888888", "#FFFFFF"],
    ["CALIBRANDO",        "#2980B9", "#FFFFFF"],
    ["BRUTO",             "#607D8B", "#FFFFFF"],
  ];

  abaDash.getRange(10, 1).setValue("Nível").setFontWeight("bold").setBackground("#DDDDDD");
  abaDash.getRange(10, 2).setValue("Leituras").setFontWeight("bold").setBackground("#DDDDDD");
  abaDash.getRange(10, 3).setValue("%").setFontWeight("bold").setBackground("#DDDDDD");

  var linhaAtual = 11;
  niveis.forEach(function(n) {
    var count = contNiveis[n[0]] || 0;
    if (count == 0) return;
    var pct = ((count / todosDados.length) * 100).toFixed(1);
    abaDash.getRange(linhaAtual, 1).setValue(n[0]).setBackground(n[1]).setFontColor(n[2]).setFontWeight("bold");
    abaDash.getRange(linhaAtual, 2).setValue(count).setHorizontalAlignment("center");
    abaDash.getRange(linhaAtual, 3).setValue(pct + "%").setHorizontalAlignment("center");
    linhaAtual++;
  });

  // Total
  abaDash.getRange(linhaAtual, 1).setValue("Total").setFontWeight("bold").setBackground("#DDDDDD");
  abaDash.getRange(linhaAtual, 2).setValue(todosDados.length).setFontWeight("bold").setHorizontalAlignment("center");
  abaDash.getRange(linhaAtual, 3).setValue("100%").setFontWeight("bold").setHorizontalAlignment("center");
  linhaAtual++;

  // ── Tabela de anomalias ──────────────────────────────────────
  var colTabela = 5;
  abaDash.getRange(9, colTabela, 1, 4).merge();
  abaDash.getRange(9, colTabela).setValue("Últimas Anomalias");
  abaDash.getRange(9, colTabela).setFontSize(13).setFontWeight("bold");
  abaDash.getRange(9, colTabela, 1, 4).setBackground("#F5F5F5");

  abaDash.getRange(10, colTabela).setValue("Data").setFontWeight("bold").setBackground("#DDDDDD");
  abaDash.getRange(10, colTabela+1).setValue("Horário").setFontWeight("bold").setBackground("#DDDDDD");
  abaDash.getRange(10, colTabela+2).setValue("Corrente (A)").setFontWeight("bold").setBackground("#DDDDDD");
  abaDash.getRange(10, colTabela+3).setValue("Nível").setFontWeight("bold").setBackground("#DDDDDD");

  var niveisAnomalia = ["VERMELHO ALTO","LARANJA ALTO","VERMELHO BAIXO","LARANJA BAIXO"];
  var anomaliasLista = todosDados.filter(function(r) {
    return niveisAnomalia.indexOf(r[8]) >= 0;
  }).slice(-15).reverse();

  var linhaAnom = 11;
  anomaliasLista.forEach(function(r) {
    var horaRaw = r[1];
    var hora = horaRaw instanceof Date ? Utilities.formatDate(horaRaw, "America/Sao_Paulo", "HH:mm:ss") : String(horaRaw);
    abaDash.getRange(linhaAnom, colTabela).setValue(r[0]);
    abaDash.getRange(linhaAnom, colTabela+1).setValue(hora);
    abaDash.getRange(linhaAnom, colTabela+2).setValue(r[2]).setNumberFormat("0.0000");
    abaDash.getRange(linhaAnom, colTabela+3).setValue(r[8]).setBackground(obterCor(r[8])).setFontColor(obterCorFonte(r[8])).setFontWeight("bold");
    linhaAnom++;
  });

  // ── Dados para gráfico (média por hora) ─────────────────────
  var linhaDados = Math.max(linhaAtual, linhaAnom) + 2;
  abaDash.getRange(linhaDados, 1, 1, 3).setValues([["Horário", "Média (A)", "Desvio"]]);
  abaDash.getRange(linhaDados, 1, 1, 3).setFontWeight("bold").setBackground("#1565C0").setFontColor("#FFFFFF");

  var porHora = {};
  todosDados.forEach(function(r) {
    var horaRaw = r[1];
    var hora;
    if (horaRaw instanceof Date) {
      hora = Utilities.formatDate(horaRaw, "America/Sao_Paulo", "HH:mm");
    } else {
      hora = String(horaRaw).substring(0, 5);
    }
    if (!/^\d{2}:\d{2}$/.test(hora)) return;
    var v = parseFloat(r[2]);
    if (!isNaN(v) && v >= 0.05 && v <= 0.50) {
      if (!porHora[hora]) porHora[hora] = [];
      porHora[hora].push(v);
    }
  });

  var horasOrdenadas = Object.keys(porHora).sort();
  var linhaGrafico   = linhaDados + 1;
  horasOrdenadas.forEach(function(hora) {
    var vals = porHora[hora];
    var med  = vals.reduce(function(a,b){ return a+b; }, 0) / vals.length;
    var vari = vals.reduce(function(a,b){ return a + Math.pow(b-med,2); }, 0) / vals.length;
    var desv = Math.sqrt(vari);
    abaDash.getRange(linhaGrafico, 1).setValue(hora);
    abaDash.getRange(linhaGrafico, 2).setValue(parseFloat(med.toFixed(4)));
    abaDash.getRange(linhaGrafico, 3).setValue(parseFloat(desv.toFixed(4)));
    linhaGrafico++;
  });

  // Remove gráficos antigos
  var graficos = abaDash.getCharts();
  graficos.forEach(function(g) { abaDash.removeChart(g); });

  // Cria gráfico de linha
  if (linhaGrafico > linhaDados + 2) {
    var rangeDados = abaDash.getRange(linhaDados, 1, linhaGrafico - linhaDados, 2);
    var grafico = abaDash.newChart()
      .setChartType(Charts.ChartType.LINE)
      .addRange(rangeDados)
      .setPosition(linhaDados, 4, 0, 0)
      .setOption("title", "Média Corrente por Horário")
      .setOption("hAxis.title", "Horário")
      .setOption("vAxis.title", "Corrente (A)")
      .setOption("colors", ["#1565C0"])
      .setOption("backgroundColor", "#FFFFFF")
      .setOption("legend.position", "none")
      .setOption("width", 600)
      .setOption("height", 320)
      .build();
    abaDash.insertChart(grafico);
  }

  // ── Ajusta largura das colunas ───────────────────────────────
  abaDash.setColumnWidth(1, 150);
  abaDash.setColumnWidth(2, 80);
  abaDash.setColumnWidth(3, 60);
  abaDash.setColumnWidth(4, 20);
  abaDash.setColumnWidth(5, 100);
  abaDash.setColumnWidth(6, 80);
  abaDash.setColumnWidth(7, 100);
  abaDash.setColumnWidth(8, 150);

  // Congela linha do cabeçalho
  abaDash.setFrozenRows(7);

  Logger.log("Dashboard atualizado com " + todosDados.length + " leituras.");
}

// ================================================================
//   CORES POR NÍVEL
// ================================================================
function obterCor(nivel) {
  switch (nivel) {
    case "VERMELHO BAIXO":    return "#C0392B";
    case "LARANJA BAIXO":     return "#E67E22";
    case "AMARELO BAIXO":     return "#F1C40F";
    case "VERDE - NORMAL":    return "#27AE60";
    case "AMARELO ALTO":      return "#F39C12";
    case "LARANJA ALTO":      return "#D35400";
    case "VERMELHO ALTO":     return "#922B21";
    case "MAQUINA DESLIGADA": return "#888888";
    case "BRUTO":             return "#FFFFFF";
    default:                  return "#FFFFFF";
  }
}

function obterCorFonte(nivel) {
  switch (nivel) {
    case "AMARELO BAIXO": return "#7D6608";
    case "AMARELO ALTO":  return "#784212";
    case "LARANJA BAIXO": return "#784212";
    case "BRUTO":         return "#000000";
    default:              return "#FFFFFF";
  }
}

// ================================================================
//   EXECUTA MANUALMENTE (teste)
// ================================================================
function testarEnvio() {
  var dadosTeste = {
    postData: {
      contents: JSON.stringify({
        irms: 0.262,
        tensao: 220.5,
        vibX: 120,
        vibY: -80,
        vibZ: 16350,
        temperatura: 28.4,
        nivel: "BRUTO",
        estado: "Leitura bruta - fase de coleta"
      })
    }
  };
  var resultado = doPost(dadosTeste);
  Logger.log(resultado.getContent());
}

function criarDashboardManual() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  atualizarDashboard(ss);
  Logger.log("Dashboard criado!");
}

function criarHistoricoManual() {
  var ss   = SpreadsheetApp.getActiveSpreadsheet();
  var aba  = ss.getSheetByName(ABA_LEITURAS);
  arquivarHistorico(ss, aba);
  Logger.log("Histórico arquivado!");
}

function testarMudancaDeDia() {
  var ss  = SpreadsheetApp.getActiveSpreadsheet();
  var aba = ss.getSheetByName(ABA_LEITURAS);
  var hoje = Utilities.formatDate(new Date(), "America/Sao_Paulo", "dd/MM/yyyy");
  verificarMudancaDeDia(ss, aba, hoje);
  Logger.log("Verificacao de mudanca de dia executada.");
}
