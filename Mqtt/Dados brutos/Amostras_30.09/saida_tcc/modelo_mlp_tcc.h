// MLP 6-8-4-3 (ReLU, ReLU, softmax) - 107 parametros - gerado por treinar_modelo_tcc_v1.py
// entradas ja em z-score (parametros_zscore.h), nesta ordem: corrente, potencia, pf, delta_corrente, media_corrente, desvio_corrente
// saidas: 0 = PARADA, 1 = VAZIO, 2 = USINANDO
// SOBRECARGA (> 5,3 A) e CRITICO (>= 7,2 A) ficam por limiar fixo no firmware.
#pragma once
#include <math.h>

const float W1[6][8] = {
    {0.49453166f, -0.29858759f, -0.48066297f, -0.20201376f, -0.58252811f, 0.32433575f, 1.25273287f, -0.00513209f},
    {-0.09481888f, -0.75297779f, -0.45994130f, -0.66255558f, -0.39908642f, -0.68211514f, 0.10315425f, 0.28550965f},
    {0.20607626f, -0.43895325f, 0.55986470f, -0.43643942f, 0.76361221f, -0.32280892f, -1.01714349f, 0.86849862f},
    {0.03374393f, 0.07849685f, 0.39908740f, -0.89401984f, 0.03078011f, -0.63034999f, 0.12344004f, -0.42008874f},
    {0.02709504f, -0.30177313f, -0.09411032f, -0.27326015f, 0.00410024f, -0.70555240f, 0.04880604f, -0.00378839f},
    {0.70832103f, 0.19294190f, 0.51550931f, 0.02224308f, -0.30848017f, 0.11999963f, -0.06733652f, -0.05769572f}
};
const float B1[8] = {-0.54308838f, -0.47381860f, 0.99874610f, 0.38841248f, 0.98740250f, -0.12968786f, 0.13929255f, 0.17396796f};
const float W2[8][4] = {
    {0.57954788f, -0.54628509f, -0.50292522f, -0.50913173f},
    {-0.06973706f, -1.04784381f, -0.29311040f, -0.46278423f},
    {-0.70639586f, 0.66839451f, 0.69767946f, 0.82106233f},
    {-0.27840954f, 0.79058534f, 0.84595007f, 0.04832405f},
    {-1.27334666f, 1.79807854f, -0.95065433f, 1.14313197f},
    {-0.49102026f, -0.41239464f, 0.46444538f, 0.01107148f},
    {1.30852401f, -0.34043971f, -0.13343501f, -0.20185150f},
    {-0.40914547f, 0.46741509f, -0.25932389f, 1.11346757f}
};
const float B2[4] = {0.12313040f, 0.76461411f, -0.04496030f, 0.80040663f};
const float W3[4][3] = {
    {-1.76406372f, -1.11582637f, 1.63043571f},
    {-0.31563151f, 2.08517003f, -1.75352979f},
    {-0.21017708f, -0.64045727f, -1.07058430f},
    {-0.90176427f, 1.52535450f, -1.28249598f}
};
const float B3[3] = {-0.42510563f, 0.62186825f, -0.09821882f};

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
