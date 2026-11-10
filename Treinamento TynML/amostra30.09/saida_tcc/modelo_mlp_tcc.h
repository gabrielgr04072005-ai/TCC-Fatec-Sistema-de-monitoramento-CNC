// MLP 6-8-4-3 (ReLU, ReLU, softmax) - 107 parametros - gerado por treinar_modelo_tcc_v1.py
// entradas ja em z-score (parametros_zscore.h), nesta ordem: corrente, potencia, pf, delta_corrente, media_corrente, desvio_corrente
// saidas: 0 = PARADA, 1 = VAZIO, 2 = USINANDO
// SOBRECARGA (> 5,3 A) e CRITICO (>= 7,2 A) ficam por limiar fixo no firmware.
#pragma once
#include <math.h>

const float W1[6][8] = {
    {0.53366691f, 0.86728519f, 0.05055980f, -0.21947576f, 0.49311036f, -0.52638477f, -0.89198667f, 1.06695902f},
    {-0.12817758f, 1.05467081f, -0.57102484f, -0.24658260f, -0.17604826f, 0.22938201f, -0.17626999f, 0.13362536f},
    {-0.77097213f, -0.77423823f, -0.58578163f, 0.33206001f, -0.07427716f, -0.29533717f, 0.93058705f, 0.18116464f},
    {0.30084422f, 0.74182349f, -0.26420289f, -0.60645622f, 0.00215346f, -0.06877885f, 0.34400028f, 0.39290547f},
    {0.56805283f, 0.66552639f, -0.13013326f, -0.33021915f, 0.02610510f, 0.07229219f, 0.30048993f, 0.37545782f},
    {-0.19646057f, 0.00560259f, -0.36005369f, -0.23091491f, 0.14407465f, -0.71083021f, 0.12378360f, 0.04842582f}
};
const float B1[8] = {1.34880733f, -0.23096332f, -0.13420622f, 0.82753754f, -0.26834536f, -0.08540392f, 1.36926937f, -0.42356533f};
const float W2[8][4] = {
    {0.81678295f, 0.43111029f, -0.22139154f, -0.47654995f},
    {1.04982042f, -0.10428388f, 1.16954494f, -0.61657482f},
    {-0.22191943f, -0.10391195f, 0.96450806f, 0.05871785f},
    {-0.10665455f, 1.35134768f, 0.47669429f, -0.23150802f},
    {0.60976481f, -0.55219924f, 0.11006476f, 0.23411533f},
    {-0.75496262f, 0.41429007f, 0.28044772f, 0.59943312f},
    {-1.19474173f, 2.31290627f, -2.04640102f, -0.54464060f},
    {1.21569204f, -1.20686829f, 0.68778878f, -0.20442793f}
};
const float B2[4] = {-0.25019005f, 0.86857253f, -0.24555218f, -0.29609117f};
const float W3[4][3] = {
    {-1.60407400f, -0.52331007f, 1.08364987f},
    {-0.34979734f, 1.78279197f, -1.82742107f},
    {0.88188517f, -1.49318433f, 0.77346683f},
    {-0.79334193f, -0.79521716f, 0.76702255f}
};
const float B3[3] = {-0.15061314f, 0.60020673f, -0.75057423f};

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
