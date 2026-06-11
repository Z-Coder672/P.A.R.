// Header-only inference for the tiny ternary transformer trained by train.py.
//
// Public API:
//   float classifier_logit(const float rgbc[4]);   // raw pre-sigmoid score
//   bool  classifier_is_blue(const float rgbc[4]); // shortcut: logit > 0
//
// All ternary matmuls run as branchless add/sub over int8 quantized
// activations — no multiplies in the inner loop, no float divides past
// the per-tensor scales. Designed for Cortex-M0+ (no FPU): every layer's
// arithmetic is dominated by integer add/sub.
#pragma once

#include <math.h>
#include <stdint.h>

#include "model_weights.h"

#ifndef CLASSIFIER_MAX_DIM
#define CLASSIFIER_MAX_DIM 64  // upper bound on any in_dim/out_dim used here
#endif

namespace classifier_detail {

static inline void layer_norm(float* __restrict out,
                              const float* __restrict in,
                              const float* __restrict gamma,
                              const float* __restrict beta,
                              int d) {
  float mean = 0.0f;
  for (int i = 0; i < d; ++i) mean += in[i];
  mean /= (float)d;

  float var = 0.0f;
  for (int i = 0; i < d; ++i) {
    float dev = in[i] - mean;
    var += dev * dev;
  }
  var /= (float)d;

  float inv = 1.0f / sqrtf(var + 1e-5f);
  for (int i = 0; i < d; ++i) {
    out[i] = (in[i] - mean) * inv * gamma[i] + beta[i];
  }
}

// Quantize `in` to int8 (per-tensor symmetric absmax) into `q`, returning the scale.
static inline float quant_int8(const float* __restrict in, int n, int8_t* __restrict q) {
  float m = 0.0f;
  for (int i = 0; i < n; ++i) {
    float a = fabsf(in[i]);
    if (a > m) m = a;
  }
  float s = (m < 1e-5f) ? (1e-5f / 127.0f) : (m / 127.0f);
  float inv_s = 1.0f / s;
  for (int i = 0; i < n; ++i) {
    float v = in[i] * inv_s;
    if (v > 127.0f) v = 127.0f;
    else if (v < -127.0f) v = -127.0f;
    q[i] = (int8_t)lrintf(v);
  }
  return s;
}

// out[j] = bias[j] + alpha * s_in * sum_i (in_q[i] * weight[j*in_dim + i])
// where weight[i] in {-1,0,+1}. Inner loop has zero multiplies.
static inline void ternary_matmul(float* __restrict out,
                                  const float* __restrict in,
                                  const int8_t* __restrict weight,
                                  float alpha,
                                  const float* bias,  // may be nullptr
                                  int out_dim, int in_dim) {
  int8_t in_q[CLASSIFIER_MAX_DIM];
  float s_in = quant_int8(in, in_dim, in_q);
  float comb = s_in * alpha;

  for (int j = 0; j < out_dim; ++j) {
    int32_t acc = 0;
    const int8_t* w = weight + j * in_dim;
    for (int i = 0; i < in_dim; ++i) {
      int8_t wi = w[i];
      if (wi > 0) acc += in_q[i];
      else if (wi < 0) acc -= in_q[i];
    }
    float r = (float)acc * comb;
    if (bias) r += bias[j];
    out[j] = r;
  }
}

static inline void softmax_inplace(float* x, int n) {
  float m = x[0];
  for (int i = 1; i < n; ++i) if (x[i] > m) m = x[i];
  float sum = 0.0f;
  for (int i = 0; i < n; ++i) {
    x[i] = expf(x[i] - m);
    sum += x[i];
  }
  float inv = 1.0f / sum;
  for (int i = 0; i < n; ++i) x[i] *= inv;
}

}  // namespace classifier_detail


inline float classifier_logit(const float rgbc[4]) {
  using namespace classifier_detail;

  constexpr int N = MODEL_N_TOKENS;  // 4
  constexpr int D = MODEL_D_MODEL;   // 4
  constexpr int F = MODEL_D_FF;      // 8

  // 1. Z-score normalize input.
  float x[N];
  for (int i = 0; i < N; ++i) x[i] = (rgbc[i] - INPUT_MEAN[i]) / INPUT_STD[i];

  // 2. Tokenize: tokens[t][d] = x[t] * tok_proj[t,d] + tok_bias[t,d].
  float tokens[N][D];
  for (int t = 0; t < N; ++t) {
    float xt = x[t];
    const float* p = &TOK_PROJ[t * D];
    const float* b = &TOK_BIAS[t * D];
    for (int d = 0; d < D; ++d) tokens[t][d] = xt * p[d] + b[d];
  }

  // 3. Attention block.
  float h[N][D];
  for (int t = 0; t < N; ++t) layer_norm(h[t], tokens[t], LN1_W, LN1_B, D);

  float q[N][D], k[N][D], v[N][D];
  for (int t = 0; t < N; ++t) {
    ternary_matmul(q[t], h[t], Q_W, Q_ALPHA, nullptr, D, D);
    ternary_matmul(k[t], h[t], K_W, K_ALPHA, nullptr, D, D);
    ternary_matmul(v[t], h[t], V_W, V_ALPHA, nullptr, D, D);
  }

  const float scale = 1.0f / sqrtf((float)D);
  float scores[N][N];
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      float s = 0.0f;
      for (int d = 0; d < D; ++d) s += q[i][d] * k[j][d];
      scores[i][j] = s * scale;
    }
    softmax_inplace(scores[i], N);
  }

  float attn_out[N][D];
  for (int i = 0; i < N; ++i) {
    for (int d = 0; d < D; ++d) {
      float s = 0.0f;
      for (int j = 0; j < N; ++j) s += scores[i][j] * v[j][d];
      attn_out[i][d] = s;
    }
  }

  float o_out[N][D];
  for (int t = 0; t < N; ++t) ternary_matmul(o_out[t], attn_out[t], O_W, O_ALPHA, O_B, D, D);
  for (int t = 0; t < N; ++t)
    for (int d = 0; d < D; ++d) tokens[t][d] += o_out[t][d];

  // 4. FFN block.
  for (int t = 0; t < N; ++t) layer_norm(h[t], tokens[t], LN2_W, LN2_B, D);

  float ff1_out[N][F];
  for (int t = 0; t < N; ++t) {
    ternary_matmul(ff1_out[t], h[t], FF1_W, FF1_ALPHA, FF1_B, F, D);
    for (int i = 0; i < F; ++i) if (ff1_out[t][i] < 0.0f) ff1_out[t][i] = 0.0f;
  }
  float ff2_out[N][D];
  for (int t = 0; t < N; ++t) {
    ternary_matmul(ff2_out[t], ff1_out[t], FF2_W, FF2_ALPHA, FF2_B, D, F);
  }
  for (int t = 0; t < N; ++t)
    for (int d = 0; d < D; ++d) tokens[t][d] += ff2_out[t][d];

  // 5. Mean pool.
  float pooled[D] = {0};
  for (int t = 0; t < N; ++t)
    for (int d = 0; d < D; ++d) pooled[d] += tokens[t][d];
  float inv_n = 1.0f / (float)N;
  for (int d = 0; d < D; ++d) pooled[d] *= inv_n;

  // 6. Final layer norm + head.
  float pooled_n[D];
  layer_norm(pooled_n, pooled, LN_OUT_W, LN_OUT_B, D);
  float logit_buf[1];
  ternary_matmul(logit_buf, pooled_n, HEAD_W, HEAD_ALPHA, HEAD_B, 1, D);
  return logit_buf[0];
}

inline bool classifier_is_blue(const float rgbc[4]) {
  return classifier_logit(rgbc) > 0.0f;
}
