#!/usr/bin/env python3
"""Reference inference that mirrors ColorClassifier/classifier.h byte-for-byte
in pure NumPy. Run on the held-out split to confirm the C-side math reproduces
the PyTorch model's accuracy. If this script disagrees with train.py, the bug
is in the export pipeline — not in the Arduino code."""

import json
import os

import numpy as np
import torch

HERE = os.path.dirname(os.path.abspath(__file__))


def quant_int8(x):
    m = float(np.max(np.abs(x)))
    s = max(m, 1e-5) / 127.0
    q = np.clip(np.round(x / s), -127, 127).astype(np.int8)
    return q, s


def ternary_matmul(x, w_int, alpha, bias=None):
    # w_int: (out, in), values in {-1,0,1}; alpha float; bias (out,) or None.
    q, s = quant_int8(x)
    acc = (w_int.astype(np.int32) @ q.astype(np.int32))
    out = acc.astype(np.float32) * (s * alpha)
    if bias is not None:
        out = out + bias
    return out


def layer_norm(x, w, b, eps=1e-5):
    mean = x.mean()
    var = ((x - mean) ** 2).mean()
    return (x - mean) / np.sqrt(var + eps) * w + b


def predict_logit(rgbc, z):
    N = z["tok_proj"].shape[0]
    D = z["tok_proj"].shape[1]
    F_ = z["ff1_w_int"].shape[0]

    x = (rgbc - z["input_mean"]) / z["input_std"]
    tokens = x[:, None] * z["tok_proj"] + z["tok_bias"]  # (N, D)

    # Attention
    h = np.stack([layer_norm(tokens[t], z["ln1_w"], z["ln1_b"]) for t in range(N)])
    q = np.stack([ternary_matmul(h[t], z["q_w_int"], float(z["q_alpha"])) for t in range(N)])
    k = np.stack([ternary_matmul(h[t], z["k_w_int"], float(z["k_alpha"])) for t in range(N)])
    v = np.stack([ternary_matmul(h[t], z["v_w_int"], float(z["v_alpha"])) for t in range(N)])

    scale = 1.0 / np.sqrt(D)
    scores = (q @ k.T) * scale  # (N, N)
    scores -= scores.max(axis=1, keepdims=True)
    e = np.exp(scores)
    scores = e / e.sum(axis=1, keepdims=True)
    attn_out = scores @ v  # (N, D)

    o = np.stack([
        ternary_matmul(attn_out[t], z["o_w_int"], float(z["o_alpha"]), z["o_b"])
        for t in range(N)
    ])
    tokens = tokens + o

    # FFN
    h2 = np.stack([layer_norm(tokens[t], z["ln2_w"], z["ln2_b"]) for t in range(N)])
    ff1 = np.stack([
        ternary_matmul(h2[t], z["ff1_w_int"], float(z["ff1_alpha"]), z["ff1_b"])
        for t in range(N)
    ])
    ff1 = np.maximum(ff1, 0)
    ff2 = np.stack([
        ternary_matmul(ff1[t], z["ff2_w_int"], float(z["ff2_alpha"]), z["ff2_b"])
        for t in range(N)
    ])
    tokens = tokens + ff2

    pooled = tokens.mean(axis=0)
    pooled_n = layer_norm(pooled, z["ln_out_w"], z["ln_out_b"])
    logit = ternary_matmul(pooled_n, z["head_w_int"], float(z["head_alpha"]), z["head_b"])
    return float(logit[0])


def main():
    z = np.load(os.path.join(HERE, "model_int8.npz"))
    with open(os.path.join(HERE, "color_data.json")) as f:
        d = json.load(f)
    blue = np.asarray(d["blue"], dtype=np.float32)
    black = np.asarray(d["black"], dtype=np.float32)
    X = np.concatenate([blue, black], axis=0)
    y = np.concatenate([np.ones(len(blue)), np.zeros(len(black))]).astype(np.float32)

    np.random.seed(0)
    perm = np.random.permutation(len(X))
    te = perm[int(0.8 * len(X)):]

    preds = np.array([predict_logit(X[i], z) > 0 for i in te])
    acc = (preds.astype(np.float32) == y[te]).mean()
    print(f"NumPy reference test_acc = {acc:.4f}  ({len(te)} samples)")

    # Compare to PyTorch model
    import sys
    sys.path.insert(0, HERE)
    from train import TinyTernaryTransformer  # noqa
    ck = torch.load(os.path.join(HERE, "model.pt"), map_location="cpu", weights_only=False) \
        if os.path.exists(os.path.join(HERE, "model.pt")) else None
    if ck is None:
        ck = torch.load("/tmp/m_small.pt", map_location="cpu", weights_only=False)
    sd = ck["model"]
    d_model = sd["tok_proj"].shape[1]
    d_ff = sd["ff1.weight"].shape[0]
    model = TinyTernaryTransformer(d_model=d_model, d_ff=d_ff, quantize=True)
    model.load_state_dict(sd)
    model.eval()
    Xn = (X - ck["mean"]) / ck["std"]
    with torch.no_grad():
        pt_logit = model(torch.from_numpy(Xn[te]).float()).numpy()
    pt_pred = (pt_logit > 0).astype(np.float32)
    pt_acc = (pt_pred == y[te]).mean()
    agree = (pt_pred == preds.astype(np.float32)).mean()
    print(f"PyTorch model test_acc    = {pt_acc:.4f}")
    print(f"Per-sample agreement      = {agree:.4f}")


if __name__ == "__main__":
    main()
