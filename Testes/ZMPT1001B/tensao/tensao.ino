// =====================================================
// ZMPT101B - MEDIÇÃO DE TENSÃO AC
// Arduino UNO
//
// Saídas:
// Vmax  = pico positivo
// Vmin  = pico negativo
// Vrms  = tensão eficaz
//
// Rede utilizada: 127 V RMS
// =====================================================

const int PINO_ZMPT = A0;

// =====================================================
// CONFIGURAÇÃO
// =====================================================

const int AMOSTRAS = 1000;

// Fator obtido inicialmente:
// 127 V / aproximadamente 34,5 ADC RMS
const float FATOR_CALIBRACAO = 3.681;

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);

  delay(1000);

  Serial.println("ZMPT101B iniciado");
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
  float soma = 0;

  // ===================================================
  // 1 - CALCULA O OFFSET
  // ===================================================

  for (int i = 0; i < AMOSTRAS; i++)
  {
    soma += analogRead(PINO_ZMPT);
  }

  float offset = soma / AMOSTRAS;

  // ===================================================
  // 2 - COLETA A ONDA E CALCULA RMS
  // ===================================================

  float somaQuadrados = 0;

  float valorMax = -10000;
  float valorMin = 10000;

  for (int i = 0; i < AMOSTRAS; i++)
  {
    int leitura = analogRead(PINO_ZMPT);

    // Remove o offset DC
    float sinalAC = leitura - offset;

    // Guarda pico positivo
    if (sinalAC > valorMax)
    {
      valorMax = sinalAC;
    }

    // Guarda pico negativo
    if (sinalAC < valorMin)
    {
      valorMin = sinalAC;
    }

    // RMS
    somaQuadrados += sinalAC * sinalAC;
  }

  // ===================================================
  // 3 - RMS DO ADC
  // ===================================================

  float rmsADC = sqrt(somaQuadrados / AMOSTRAS);

  // ===================================================
  // 4 - CONVERSÃO PARA VOLTS
  // ===================================================

  float tensaoRMS = rmsADC * FATOR_CALIBRACAO;

  float tensaoPico = valorMax * FATOR_CALIBRACAO;

  float tensaoMin = valorMin * FATOR_CALIBRACAO;

  // ===================================================
  // 5 - MONITOR SERIAL
  // ===================================================

  Serial.print("Offset: ");
  Serial.print(offset, 2);

  Serial.print(" | RMS ADC: ");
  Serial.print(rmsADC, 2);

  Serial.print(" | Vmax: ");
  Serial.print(tensaoPico, 2);

  Serial.print(" V | Vmin: ");
  Serial.print(tensaoMin, 2);

  Serial.print(" V | Vrms: ");
  Serial.print(tensaoRMS, 2);

  Serial.println(" V");

  // ===================================================
  // 6 - SERIAL PLOTTER
  // ===================================================
  //
  // Cada variável aparece como uma linha diferente
  // no Serial Plotter.
  //
  // ===================================================

  Serial.print("Vmax:");
  Serial.print(tensaoPico, 2);

  Serial.print("\tVmin:");
  Serial.print(tensaoMin, 2);

  Serial.print("\tVrms:");
  Serial.println(tensaoRMS, 2);

  delay(100);
}
