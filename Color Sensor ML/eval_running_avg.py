#!/usr/bin/env python3
"""Test whether averaging consecutive sensor reads beats single-frame classification.

Two experiments:
  A) Take the already-trained small ternary model (d_model=4) and feed it
     5-step running averages of the raw RGBC stream. No retraining — just
     measure how much the smoother input alone helps.
  B) Retrain a fresh small ternary model from scratch on the averaged data.

Same train/test split seed as train.py (seed=0, 80/20) so numbers are comparable.
"""

import argparse
import json
import os
import sys

import numpy as np
import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from train import TinyTernaryTransformer, train as run_train  # noqa: E402

WINDOW = 5


def running_avg(arr, w):
    """Sliding window mean along axis 0. arr: (N,F) -> (N-w+1, F)."""
    n = arr.shape[0]
    if n < w:
        return arr[:0]
    pad = np.zeros((1, arr.shape[1]), dtype=np.float64)
    cs = np.cumsum(np.concatenate([pad, arr.astype(np.float64)], axis=0), axis=0)
    return ((cs[w:] - cs[:-w]) / w).astype(np.float32)


def load_avg(path, w):
    with open(path) as f:
        d = json.load(f)
    blue = running_avg(np.asarray(d["blue"], dtype=np.float32), w)
    black = running_avg(np.asarray(d["black"], dtype=np.float32), w)
    X = np.concatenate([blue, black], axis=0)
    y = np.concatenate([np.ones(len(blue)), np.zeros(len(black))]).astype(np.float32)
    return X, y, len(blue), len(black)


def split_idx(n, seed=0, frac=0.8):
    np.random.seed(seed)
    perm = np.random.permutation(n)
    n_tr = int(frac * n)
    return perm[:n_tr], perm[n_tr:]


def eval_existing(ckpt_path, X, y):
    ck = torch.load(ckpt_path, map_location="cpu", weights_only=False)
    sd = ck["model"]
    d_model = sd["tok_proj"].shape[1]
    d_ff = sd["ff1.weight"].shape[0]
    model = TinyTernaryTransformer(d_model=d_model, d_ff=d_ff, quantize=True)
    model.load_state_dict(sd)
    model.eval()
    Xn = (X - ck["mean"]) / ck["std"]
    _, te = split_idx(len(X))
    with torch.no_grad():
        logit = model(torch.from_numpy(Xn[te]).float())
        pred = (torch.sigmoid(logit) > 0.5).float().numpy()
    return (pred == y[te]).mean(), d_model, d_ff


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(HERE, "color_data.json"))
    ap.add_argument("--ckpt", default="/tmp/m_small.pt")
    ap.add_argument("--epochs", type=int, default=100)
    ap.add_argument("--window", type=int, default=WINDOW)
    args = ap.parse_args()

    X, y, n_blue, n_black = load_avg(args.data, args.window)
    print(f"{args.window}-step running avg: {len(X)} samples ({n_blue} blue, {n_black} black)")

    # ---- Mode A: existing small ternary model on averaged inputs ----
    acc_a, dm, df = eval_existing(args.ckpt, X, y)
    print()
    print(f"Mode A — pretrained ternary d_model={dm} d_ff={df}, no retrain:")
    print(f"  test_acc on {args.window}-step avg = {acc_a:.4f}")

    # ---- Mode B: retrain on averaged data ----
    avg_path = "/tmp/color_data_avg5.json"
    with open(avg_path, "w") as f:
        json.dump(
            {
                "blue": X[y == 1].tolist(),
                "black": X[y == 0].tolist(),
            },
            f,
        )

    print()
    print(f"Mode B — retrain ternary d_model={dm} d_ff={df} on averaged data:")
    train_args = argparse.Namespace(
        data=avg_path,
        out_pt="/tmp/m_small_avg.pt",
        out_npz="/tmp/m_small_avg.npz",
        epochs=args.epochs,
        lr=2e-3,
        d_model=dm,
        d_ff=df,
        float=False,
    )
    run_train(train_args)


if __name__ == "__main__":
    main()
