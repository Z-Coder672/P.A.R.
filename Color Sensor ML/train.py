#!/usr/bin/env python3
"""Train a tiny ternary transformer to classify TCS3200 RGBC samples as blue vs black.

Architecture (~750 ternary weights, d_model=8, 4 tokens):
  - Per-feature scalar -> d_model embedding (4 tokens)
  - 1 transformer block: 1-head self-attention + FFN (8 -> 16 -> 8)
  - Mean-pool tokens -> linear classifier (8 -> 1 logit)

Weights are constrained to {-1, 0, +1} via BitNet b1.58 style quantization-aware
training (per-tensor abs-mean scale, STE). Activations entering each ternary
matmul are quantized to int8 (per-tensor absmax, STE). LayerNorm/biases/scales
stay in float32 — they're tiny and cheap on-chip.

Outputs:
  model.pt        full PyTorch checkpoint (for eval / re-export)
  model_int8.npz  quantized weights (int8 ternary), float scales, norm stats
"""

import argparse
import json
import math
import os
import sys

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader, TensorDataset

N_TOKENS = 4
N_HEADS = 1
ACT_BITS = 8


def load_data(path):
    with open(path) as f:
        d = json.load(f)
    blue = np.asarray(d["blue"], dtype=np.float32)
    black = np.asarray(d["black"], dtype=np.float32)
    X = np.concatenate([blue, black], axis=0)
    y = np.concatenate(
        [np.ones(len(blue), dtype=np.float32), np.zeros(len(black), dtype=np.float32)]
    )
    return X, y


# --------- quantization primitives (STE) ---------

class _RoundSTE(torch.autograd.Function):
    @staticmethod
    def forward(ctx, x):
        return torch.round(x)

    @staticmethod
    def backward(ctx, g):
        return g


def round_ste(x):
    return _RoundSTE.apply(x)


def quantize_ternary(w, eps=1e-5):
    """BitNet b1.58: w_q in {-1,0,1} * alpha, alpha = mean(|w|).
    Returns the dequantized fake-quant tensor (alpha * ternary_int)."""
    alpha = w.abs().mean().clamp(min=eps)
    w_scaled = w / alpha
    w_tern = round_ste(w_scaled.clamp(-1.0, 1.0))
    return w_tern * alpha


def quantize_act_int8(x, eps=1e-5):
    """Per-tensor symmetric int8 fake-quant. Used on the input to every ternary matmul."""
    s = x.detach().abs().max().clamp(min=eps) / 127.0
    q = round_ste((x / s).clamp(-127.0, 127.0))
    return q * s


# --------- ternary linear layer ---------

class TernaryLinear(nn.Module):
    """Ternary weights + int8 activations when quantize=True. Plain float linear otherwise."""

    def __init__(self, in_f, out_f, bias=True, quantize=True):
        super().__init__()
        self.weight = nn.Parameter(torch.empty(out_f, in_f))
        nn.init.kaiming_uniform_(self.weight, a=math.sqrt(5))
        self.bias = nn.Parameter(torch.zeros(out_f)) if bias else None
        self.quantize = quantize

    def forward(self, x):
        if self.quantize:
            x_q = quantize_act_int8(x)
            w_q = quantize_ternary(self.weight)
            return F.linear(x_q, w_q, self.bias)
        return F.linear(x, self.weight, self.bias)


# --------- model ---------

class TinyTernaryTransformer(nn.Module):
    def __init__(self, d_model=8, d_ff=16, quantize=True):
        super().__init__()
        self.d_model = d_model
        self.tok_proj = nn.Parameter(torch.randn(N_TOKENS, d_model) * 0.5)
        self.tok_bias = nn.Parameter(torch.zeros(N_TOKENS, d_model))

        self.ln1 = nn.LayerNorm(d_model)
        self.q = TernaryLinear(d_model, d_model, bias=False, quantize=quantize)
        self.k = TernaryLinear(d_model, d_model, bias=False, quantize=quantize)
        self.v = TernaryLinear(d_model, d_model, bias=False, quantize=quantize)
        self.o = TernaryLinear(d_model, d_model, bias=True, quantize=quantize)

        self.ln2 = nn.LayerNorm(d_model)
        self.ff1 = TernaryLinear(d_model, d_ff, bias=True, quantize=quantize)
        self.ff2 = TernaryLinear(d_ff, d_model, bias=True, quantize=quantize)

        self.ln_out = nn.LayerNorm(d_model)
        self.head = TernaryLinear(d_model, 1, bias=True, quantize=quantize)

    def forward(self, x):
        # x: (B, 4) normalized
        b = x.shape[0]
        # tokens: (B, 4, D_MODEL)
        tokens = x.unsqueeze(-1) * self.tok_proj.unsqueeze(0) + self.tok_bias.unsqueeze(0)

        # attention block
        h = self.ln1(tokens)
        q = self.q(h)
        k = self.k(h)
        v = self.v(h)
        # single head, scaled dot product
        attn = torch.matmul(q, k.transpose(-1, -2)) / math.sqrt(self.d_model)
        attn = F.softmax(attn, dim=-1)
        h = torch.matmul(attn, v)
        h = self.o(h)
        tokens = tokens + h

        # ffn block
        h = self.ln2(tokens)
        h = self.ff1(h)
        h = F.relu(h)
        h = self.ff2(h)
        tokens = tokens + h

        # pool + classify
        pooled = tokens.mean(dim=1)
        pooled = self.ln_out(pooled)
        logit = self.head(pooled).squeeze(-1)
        return logit


# --------- training ---------

def train(args):
    torch.manual_seed(0)
    np.random.seed(0)

    X, y = load_data(args.data)
    print(f"loaded: {len(X)} samples ({int(y.sum())} blue, {int((1 - y).sum())} black)")

    # 80/20 split
    idx = np.random.permutation(len(X))
    n_tr = int(0.8 * len(X))
    tr, te = idx[:n_tr], idx[n_tr:]

    # normalize: z-score using training stats
    mean = X[tr].mean(axis=0)
    std = X[tr].std(axis=0) + 1e-6
    Xn = (X - mean) / std

    Xtr = torch.from_numpy(Xn[tr]).float()
    ytr = torch.from_numpy(y[tr]).float()
    Xte = torch.from_numpy(Xn[te]).float()
    yte = torch.from_numpy(y[te]).float()

    train_loader = DataLoader(TensorDataset(Xtr, ytr), batch_size=256, shuffle=True)

    model = TinyTernaryTransformer(d_model=args.d_model, d_ff=args.d_ff, quantize=not args.float)
    n_params = sum(p.numel() for p in model.parameters())
    n_tern = sum(m.weight.numel() for m in model.modules() if isinstance(m, TernaryLinear))
    print(f"params: {n_params} total, {n_tern} ternary")

    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.epochs)
    bce = nn.BCEWithLogitsLoss()

    best_acc = 0.0
    for epoch in range(args.epochs):
        model.train()
        ep_loss = 0.0
        n_seen = 0
        for xb, yb in train_loader:
            opt.zero_grad()
            logit = model(xb)
            loss = bce(logit, yb)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            ep_loss += loss.item() * xb.shape[0]
            n_seen += xb.shape[0]
        sched.step()

        model.eval()
        with torch.no_grad():
            pred = (torch.sigmoid(model(Xte)) > 0.5).float()
            acc = (pred == yte).float().mean().item()
            tr_pred = (torch.sigmoid(model(Xtr)) > 0.5).float()
            tr_acc = (tr_pred == ytr).float().mean().item()
        print(
            f"epoch {epoch + 1:3d}/{args.epochs}  loss={ep_loss / n_seen:.4f}  "
            f"train_acc={tr_acc:.4f}  test_acc={acc:.4f}"
        )
        if acc > best_acc:
            best_acc = acc
            torch.save(
                {"model": model.state_dict(), "mean": mean, "std": std, "test_acc": acc},
                args.out_pt,
            )

    print(f"best test_acc={best_acc:.4f}  -> {args.out_pt}")
    if not args.float:
        export_int8(args.out_pt, args.out_npz)


# --------- export quantized weights ---------

def export_int8(ckpt_path, npz_path):
    """Save ternary weights as int8 in {-1,0,1} plus their alpha scales,
    layernorm stats, embeddings, biases, and input normalization."""
    ckpt = torch.load(ckpt_path, map_location="cpu", weights_only=False)
    sd = ckpt["model"]

    out = {
        "input_mean": ckpt["mean"].astype(np.float32),
        "input_std": ckpt["std"].astype(np.float32),
        "tok_proj": sd["tok_proj"].numpy().astype(np.float32),
        "tok_bias": sd["tok_bias"].numpy().astype(np.float32),
    }

    for prefix in ("ln1", "ln2", "ln_out"):
        out[f"{prefix}_w"] = sd[f"{prefix}.weight"].numpy().astype(np.float32)
        out[f"{prefix}_b"] = sd[f"{prefix}.bias"].numpy().astype(np.float32)

    for name in ("q", "k", "v", "o", "ff1", "ff2", "head"):
        w = sd[f"{name}.weight"]
        alpha = w.abs().mean().clamp(min=1e-5)
        w_int = torch.round((w / alpha).clamp(-1.0, 1.0)).to(torch.int8)
        out[f"{name}_w_int"] = w_int.numpy()
        out[f"{name}_alpha"] = np.float32(alpha.item())
        bkey = f"{name}.bias"
        if bkey in sd:
            out[f"{name}_b"] = sd[bkey].numpy().astype(np.float32)

    np.savez(npz_path, **out)
    bytes_total = sum(v.nbytes for v in out.values())
    print(f"exported {npz_path} ({bytes_total} bytes of weights+stats)")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(here, "color_data.json"))
    ap.add_argument("--out-pt", default=os.path.join(here, "model.pt"))
    ap.add_argument("--out-npz", default=os.path.join(here, "model_int8.npz"))
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--d-model", type=int, default=8)
    ap.add_argument("--d-ff", type=int, default=16)
    ap.add_argument("--float", action="store_true", help="disable ternary/int8 quantization")
    args = ap.parse_args()

    if not os.path.exists(args.data):
        print(f"error: data file not found: {args.data}", file=sys.stderr)
        sys.exit(1)

    train(args)


if __name__ == "__main__":
    main()
