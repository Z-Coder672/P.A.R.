#!/usr/bin/env python3
"""Evolve simple arithmetic decision rules over RGBC for blue-vs-black classification.

Tree-based genetic programming. Individuals are expression trees over features
{r,g,b,c}, constants, and operators {+,-,*,/}. Fitness is classification
accuracy on the train split (predict blue if expr > 0, else black; the sign
that scores best is taken so polarity is free), minus a small per-node
parsimony penalty to favor short, human-readable rules.

Goal is to surface expressions like "b/c > 10" — the kind of rule the
existing classifier in verifyAndFix() uses, but discovered automatically.

Usage:
  ./venv/bin/python evolve.py [--gens 80] [--pop 500] [--seed 0]
"""

import argparse
import json
import os
import random
from dataclasses import dataclass
from typing import Optional

import numpy as np

OPS = ["+", "-", "*", "/"]
VARS = ["r", "g", "b", "c"]
MAX_INIT_DEPTH = 3
MAX_NODES = 25
PARSIMONY = 0.003  # accuracy points subtracted per node — favors short rules
TOURN_K = 5
CX_RATE = 0.7
MUT_RATE = 0.4


@dataclass
class Node:
    kind: str  # 'const' | 'var' | 'op'
    value: object = None  # float, str, or operator char
    left: Optional["Node"] = None
    right: Optional["Node"] = None


# --------- tree ops ---------

def random_tree(depth, rng):
    if depth <= 0 or (depth < MAX_INIT_DEPTH and rng.random() < 0.35):
        if rng.random() < 0.6:
            return Node("var", rng.choice(VARS))
        return Node("const", round(rng.uniform(-30.0, 30.0), 2))
    op = rng.choice(OPS)
    return Node("op", op, random_tree(depth - 1, rng), random_tree(depth - 1, rng))


def clone(n):
    if n.kind == "op":
        return Node("op", n.value, clone(n.left), clone(n.right))
    return Node(n.kind, n.value)


def size(n):
    if n.kind == "op":
        return 1 + size(n.left) + size(n.right)
    return 1


def all_refs(n, parent=None, side=None):
    yield (n, parent, side)
    if n.kind == "op":
        yield from all_refs(n.left, n, "left")
        yield from all_refs(n.right, n, "right")


def to_str(n):
    if n.kind == "const":
        return f"{n.value:g}"
    if n.kind == "var":
        return n.value
    return f"({to_str(n.left)} {n.value} {to_str(n.right)})"


def mutate(root, rng):
    n = clone(root)
    refs = list(all_refs(n))
    target, parent, side = rng.choice(refs)
    new_sub = random_tree(rng.randint(1, 3), rng)
    if parent is None:
        return new_sub
    setattr(parent, side, new_sub)
    return n


def crossover(a, b, rng):
    a = clone(a)
    a_refs = list(all_refs(a))
    b_refs = list(all_refs(b))
    _, pa, sa = rng.choice(a_refs)
    tb, _, _ = rng.choice(b_refs)
    sub = clone(tb)
    if pa is None:
        return sub
    setattr(pa, sa, sub)
    return a


# --------- evaluation ---------

def evaluate(n, data):
    if n.kind == "const":
        return np.full(data["r"].shape, n.value, dtype=np.float64)
    if n.kind == "var":
        return data[n.value]
    l = evaluate(n.left, data)
    r = evaluate(n.right, data)
    op = n.value
    if op == "+":
        return l + r
    if op == "-":
        return l - r
    if op == "*":
        return l * r
    # safe division
    return np.where(np.abs(r) < 1e-9, 0.0, l / r)


def acc_of(n, data, y):
    with np.errstate(all="ignore"):
        v = evaluate(n, data)
    v = np.nan_to_num(v, nan=0.0, posinf=1e12, neginf=-1e12)
    pred = (v > 0).astype(np.float32)
    a = (pred == y).mean()
    return max(a, 1.0 - a)  # polarity is free


def fitness(n, data, y):
    return acc_of(n, data, y) - PARSIMONY * size(n)


# --------- main loop ---------

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(here, "color_data.json"))
    ap.add_argument("--gens", type=int, default=80)
    ap.add_argument("--pop", type=int, default=500)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    with open(args.data) as f:
        d = json.load(f)
    blue = np.asarray(d["blue"], dtype=np.float64)
    black = np.asarray(d["black"], dtype=np.float64)
    X = np.concatenate([blue, black], axis=0)
    y = np.concatenate([np.ones(len(blue)), np.zeros(len(black))]).astype(np.float32)

    np.random.seed(args.seed)
    rng = random.Random(args.seed)
    perm = np.random.permutation(len(X))
    n_tr = int(0.8 * len(X))
    tr, te = perm[:n_tr], perm[n_tr:]
    data_tr = {v: X[tr, i] for i, v in enumerate(VARS)}
    data_te = {v: X[te, i] for i, v in enumerate(VARS)}
    y_tr, y_te = y[tr], y[te]

    # Seed: hand-crafted simple ratio trees + ramped half-and-half random init.
    seeds = []
    consts = [1, 2, 5, 10, 20, 28, 50]
    for a in VARS:
        for b_ in VARS:
            if a == b_:
                continue
            for k in consts:
                # (a / b) - k  →  rule "a/b > k"
                seeds.append(
                    Node("op", "-", Node("op", "/", Node("var", a), Node("var", b_)), Node("const", float(k)))
                )
                # a - b*k  →  rule "a > b*k"
                seeds.append(
                    Node("op", "-", Node("var", a), Node("op", "*", Node("var", b_), Node("const", float(k))))
                )
    rng.shuffle(seeds)
    seeds = seeds[: args.pop // 4]
    rest = []
    while len(seeds) + len(rest) < args.pop:
        d = rng.randint(1, MAX_INIT_DEPTH)
        rest.append(random_tree(d, rng))
    pop = seeds + rest

    best, best_fit = None, -1.0
    # Pareto front: per-size best (size -> (raw_acc, tree)).
    front: dict[int, tuple[float, Node]] = {}
    for gen in range(args.gens):
        scored = [(fitness(t, data_tr, y_tr), t) for t in pop]
        scored.sort(key=lambda x: -x[0])
        if scored[0][0] > best_fit:
            best_fit, best = scored[0][0], scored[0][1]
        for _, t in scored[: args.pop // 5]:
            sz = size(t)
            a = acc_of(t, data_tr, y_tr)
            cur = front.get(sz)
            if cur is None or a > cur[0]:
                front[sz] = (a, t)
        if (gen + 1) % 5 == 0 or gen == 0:
            te_acc = acc_of(best, data_te, y_te)
            print(
                f"gen {gen + 1:3d}  best_fit={best_fit:.4f}  "
                f"train_acc={acc_of(best, data_tr, y_tr):.4f}  "
                f"test_acc={te_acc:.4f}  size={size(best)}  expr: {to_str(best)}"
            )

        # next generation: elite + tournament-selected children
        n_elite = max(1, args.pop // 50)
        new_pop = [t for _, t in scored[:n_elite]]

        def tourn():
            cands = rng.sample(scored, TOURN_K)
            return max(cands, key=lambda x: x[0])[1]

        while len(new_pop) < args.pop:
            child = crossover(tourn(), tourn(), rng) if rng.random() < CX_RATE else clone(tourn())
            if rng.random() < MUT_RATE:
                child = mutate(child, rng)
            if size(child) > MAX_NODES:
                # too bloated: reroll a small fresh tree
                child = random_tree(rng.randint(2, 3), rng)
            new_pop.append(child)
        pop = new_pop

    # Final report — figure out actual sign so we can write the rule cleanly.
    v_te = np.nan_to_num(evaluate(best, data_te), nan=0.0, posinf=1e12, neginf=-1e12)
    pred_pos = (v_te > 0).astype(np.float32)
    acc_pos = (pred_pos == y_te).mean()
    direction = ">" if acc_pos >= 0.5 else "<"
    final_acc = max(acc_pos, 1 - acc_pos)
    expr = to_str(best)

    print()
    print(f"Best expression  : {expr}")
    print(f"Decision rule    : predict BLUE iff  {expr} {direction} 0   (else BLACK)")
    print(f"Test accuracy    : {final_acc:.4f}  ({size(best)} nodes)")

    print()
    print("Pareto front (best rule at each size, by test acc):")
    print(f"  {'size':>4}  {'test_acc':>8}  rule")
    best_so_far = 0.0
    for sz in sorted(front.keys()):
        _, t = front[sz]
        v = np.nan_to_num(evaluate(t, data_te), nan=0.0, posinf=1e12, neginf=-1e12)
        ap = (v > 0).astype(np.float32)
        a_te = (ap == y_te).mean()
        d = ">" if a_te >= 0.5 else "<"
        a_te = max(a_te, 1 - a_te)
        if a_te > best_so_far:  # only show entries that strictly improve as size grows
            best_so_far = a_te
            print(f"  {sz:>4}  {a_te:>8.4f}  {to_str(t)} {d} 0")


if __name__ == "__main__":
    main()
