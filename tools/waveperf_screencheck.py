#!/usr/bin/env python3
"""Validity check for benchmark screenshots.

A broken shader or a mishandled GL context still produces perfect-looking
frame telemetry while drawing nothing, so every run must be checked against a
screenshot. Averaging the whole screen is far too coarse: a blank waveform
moves the global mean only a few percent and slips through.

Instead we sample a NARROW strip through the centre of each waveform. Two
decks are visible, so there are two strips. Their vertical positions were
measured from a reference screenshot by locating the rows that differ most
between a good and a blank run.

Usage: waveperf_screencheck.py <reference.png> <candidate.png> [...]
"""
import sys
from PIL import Image

# Fraction of screen height at the centre of each of the two waveforms.
STRIP_CENTRES = (0.0998, 0.1996)
STRIP_HALF_HEIGHT = 6  # pixels


def strip_stats(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    out = []
    for frac in STRIP_CENTRES:
        y = int(h * frac)
        box = (0, max(0, y - STRIP_HALF_HEIGHT), w, min(h, y + STRIP_HALF_HEIGHT))
        px = list(im.crop(box).getdata())
        mean = sum(sum(p) for p in px) / (len(px) * 3)
        # spread matters too: a flat fill is as wrong as a black one
        var = sum((sum(p) / 3 - mean) ** 2 for p in px) / len(px)
        out.append((mean, var ** 0.5))
    return out


ref = strip_stats(sys.argv[1])
print(f"reference {sys.argv[1].split('/')[-2]}: " +
      "  ".join(f"deck{i+1} mean={m:.1f} sd={s:.1f}" for i, (m, s) in enumerate(ref)))

for path in sys.argv[2:]:
    cur = strip_stats(path)
    parts, ok = [], True
    for i, ((m, s), (rm, rs)) in enumerate(zip(cur, ref)):
        ratio = m / rm if rm else 0
        sdr = s / rs if rs else 0
        if ratio < 0.6 or sdr < 0.5:
            ok = False
        parts.append(f"deck{i+1} mean={m:.1f}({ratio*100:.0f}%) sd={s:.1f}({sdr*100:.0f}%)")
    verdict = "OK" if ok else "BLANK/BROKEN - discard"
    print(f"{path.split('/')[-2]}: " + "  ".join(parts) + f"  {verdict}")
