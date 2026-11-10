// ================================================================
//   Autoencoder v3c — pesos exportados manualmente (sem TFLite Micro)
//   Treinado em dados reais de Agosto/2026 (fresadora CNC)
//   Arquitetura: 4 -> 8 (tanh) -> 3 (tanh, bottleneck) -> 8 (tanh) -> 4 (linear)
//
//   Features de entrada, NESSA ORDEM:
//     [0] Corrente (A) - bruta
//     [1] Magnitude da vibracao - sqrt(vibX^2+vibY^2+vibZ^2)
//     [2] z-score local da corrente  - (corrente - mediaJanela) / sigmaJanela
//     [3] z-score local da vibracao  - (vibMag - mediaJanelaVib) / sigmaJanelaVib
//
//   IMPORTANTE: mediaJanela, sigmaJanela, mediaJanelaVib e sigmaJanelaVib
//   ja existem no firmware (v17/v18) - reaproveitar as mesmas variaveis.
//   Se sigmaJanela ou sigmaJanelaVib forem 0 (janela ainda nao calculada),
//   usar z-score = 0 para essa amostra (mesma regra do calcZScoreGenerico).
//
//   Clipar o z-score calculado em [-8, +8] antes de normalizar, igual ao
//   pre-processamento do treino, para evitar valores extremos raros
//   (sigmaJanela momentaneamente proximo de 0) explodirem a entrada.
// ================================================================
#ifndef AUTOENCODER_V3C_H
#define AUTOENCODER_V3C_H

// ── Normalizacao (z-score por feature, calculada no treino) ──────
const float AE_MEDIA[4] = {0.58362353f, 16454.93750000f, 0.42946795f, -0.00689236f};
const float AE_DESVIO[4] = {0.13221326f, 62.93434143f, 0.74818099f, 0.66516000f};

// Limiar de erro de reconstrucao (mediana + 3*MAD do treino normal)
const float AE_LIMIAR_ANOMALIA = 0.02287133f;

// ── Pesos das camadas ─────────────────────────────────────────
const float AE_W1[32] = {0.15072362f, -0.10355920f, -0.13536283f, 0.01653656f, -0.04789126f, -0.21579112f, -0.03249638f, -0.02752723f, -0.25564381f, -0.05551661f, -0.34666136f, -0.13013095f, -0.03233760f, 0.04714103f, 0.01933264f, -0.01137634f, -0.02633134f, -0.11243937f, 0.05236680f, -0.09581128f, 0.03252655f, 0.08846697f, -0.05124225f, 0.05439461f, 0.07959603f, 0.02974979f, 0.11790997f, 0.04960068f, -0.11797218f, -0.05889541f, -0.10057291f, -0.02856647f};
const float AE_B1[8] = {0.99057025f, 0.73056024f, 0.31759363f, -0.43422195f, 0.46508124f, 0.84990305f, -0.28762159f, -0.26485789f};
const float AE_W2[24] = {0.19197577f, -0.53172302f, -0.76942348f, 0.20274366f, 0.47522321f, -0.16086707f, -0.03354148f, -0.01698211f, 0.27137473f, -0.31766737f, 0.49379903f, 0.08098375f, -0.47159079f, 0.00745182f, -0.21090052f, -0.20343739f, 0.17788407f, 0.65009457f, -0.32461119f, 0.16561025f, -0.21203746f, -0.18972889f, -0.20710973f, 0.08553481f};
const float AE_B2[3] = {0.18381868f, -0.22718970f, 0.01825938f};
const float AE_W3[24] = {-0.48807356f, -0.10531643f, 1.54343677f, 1.88690829f, 0.49992773f, -0.19395293f, -1.53172863f, -1.77012992f, -1.78425515f, -1.00417912f, -0.20171736f, 0.30787581f, 1.96432316f, -2.58286381f, 0.69288254f, 0.63488603f, -0.15378086f, 1.49040437f, 0.26911002f, -0.02752440f, 0.90489781f, -0.85581118f, 3.29447603f, -0.64409906f};
const float AE_B3[8] = {-0.36477196f, -0.04555028f, -0.52205974f, 0.33956528f, 1.10127461f, 0.84639066f, -0.97989476f, 1.25113297f};
const float AE_W4[32] = {1.35447764f, -0.17032743f, 2.60267448f, -1.73950088f, -2.18680549f, 0.60006380f, 3.85983753f, 0.46329823f, 0.52126861f, 1.75348020f, 0.90589923f, 3.59496760f, 2.08088422f, 2.33907127f, -0.85207385f, 1.80495310f, -2.96957660f, 0.10468614f, -0.88871163f, 1.04284334f, 0.08153695f, -1.54302692f, 2.55928564f, 0.10515796f, 1.20863020f, 1.91839957f, 2.43131137f, -0.88081712f, -0.19002710f, 0.72184789f, 0.30482951f, -3.01652980f};
const float AE_B4[4] = {0.31346458f, 1.10608292f, 0.28909534f, -0.87990314f};

#endif // AUTOENCODER_V3C_H