#!/usr/bin/env python3
"""Aggregate an A/B series and say whether it is comparable at all.

The series is only readable if every run measured the same thing on the same
machine state: one binary, one screen, one waveform type, mains power. Anything
else is reported as a broken run and excluded loudly - a series with a silently
replaced run is worse than no series.

Usage: tools/bench/series_report.py <series_dir>
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from summarize import main as summarize_main  # noqa: E402
from summarize import meta_of, phases_of, read, seconds_of_day, parse_waveperf  # noqa: E402

PLL_LINE = re.compile(r'^(\d{2}):(\d{2}):(\d{2})\.(\d{3}).*phase-locked-loop: '
                      r'(-?\d+) ([-\d.]+) ([-\d.]+)')


def mean_of(values):
    return sum(values) / len(values) if values else None


def arm_of(name):
    """Which experimental arm a run directory belongs to."""
    for token, arm in (('asym', 'wrap=asym'), ('sym', 'wrap=sym'),
                       ('clamp1', 'clamp=1'), ('clamp0', 'clamp=0')):
        if name.startswith(token):
            return arm
    return '?'


def pll_lines(outdir, t0, t1):
    """(period_us, phase_error_us) from the 10-second PLL log lines."""
    out = []
    for line in read(os.path.join(outdir, 'mixxx.log')).splitlines():
        m = PLL_LINE.match(line)
        if not m:
            continue
        sod = (int(m.group(1)) * 3600 + int(m.group(2)) * 60 +
               int(m.group(3)) + int(m.group(4)) / 1e3)
        if t0 is not None and not (t0 <= sod <= t1):
            continue
        out.append((float(m.group(6)), float(m.group(7))))
    return out


def main(series):
    runs = sorted(d for d in os.listdir(series)
                  if os.path.isdir(os.path.join(series, d)))
    if not runs:
        print('no runs in %s' % series)
        return 1

    rows = []
    for name in runs:
        outdir = os.path.join(series, name)
        summary_path = os.path.join(outdir, 'summary.json')
        if not os.path.exists(summary_path):
            summarize_main(outdir)
        try:
            with open(summary_path) as f:
                summary = json.load(f)
        except OSError:
            print('%-14s NO SUMMARY - run did not complete' % name)
            continue
        meta = meta_of(outdir)
        ph = phases_of(outdir)
        t0 = t1 = None
        if 'measure_start' in ph and 'measure_end' in ph:
            t0, t1 = seconds_of_day(ph['measure_start']), seconds_of_day(ph['measure_end'])
        wp = parse_waveperf(outdir, t0, t1)
        periods = [r['pllPeriodUs'] for r in wp if r.get('pllPeriodUs')]
        pll = pll_lines(outdir, t0, t1)
        rows.append({
            'name': name,
            'arm': arm_of(name),
            'verdict': summary['verdict'],
            'fails': summary['fails'],
            'sha': (meta.get('bin_sha256', ['?'])[0])[:16],
            'screen': summary['metrics'].get('screen'),
            'refresh': summary['metrics'].get('screen_refresh_hz'),
            'fps': summary['metrics'].get('fps_mean'),
            'drops': summary['metrics'].get('drops_total'),
            'type': summary['metrics'].get('waveform_type'),
            'options': summary['metrics'].get('renderer_options'),
            'benchhit': bool(re.search(r'BENCHHIT MIXXX_BENCH_PLL_(CLAMP|WRAP)',
                                       read(os.path.join(outdir, 'mixxx.log')))),
            # Phase error: the metric that tells "the cause is gone" from "the
            # symptom is held down". Logged per second by the telemetry.
            'phase_sd': mean_of([r['phaseErrSdUs'] for r in wp if 'phaseErrSdUs' in r]),
            'phase_mean': mean_of([r['phaseErrMeanUs'] for r in wp if 'phaseErrMeanUs' in r]),
            'phase_worst_tel': max((abs(r['phaseErrWorstUs']) for r in wp
                                    if 'phaseErrWorstUs' in r), default=None),
            'p_first': periods[0] if periods else None,
            'p_last': periods[-1] if periods else None,
            'p_min': min(periods) if periods else None,
            'p_max': max(periods) if periods else None,
            'phase_worst': min((p for _, p in pll), default=None),
        })

    # ---------------------------------------------------- is this comparable?
    problems = []
    shas = {r['sha'] for r in rows}
    if len(shas) > 1:
        problems.append('MORE THAN ONE BINARY in the series (%s): the series is void as a '
                        'whole, not partially' % ', '.join(sorted(shas)))
    screens = {(r['screen'], r['refresh']) for r in rows if r['screen']}
    if len(screens) > 1:
        problems.append('runs were measured on different screens: %s' % screens)
    for s, hz in screens:
        if hz and abs(hz - 120) > 1:
            problems.append('screen %s runs at %.3g Hz, not the intended 120 Hz' % (s, hz))
    if len({(r['type'], r['options']) for r in rows}) > 1:
        problems.append('waveform type/options differ between runs')
    for r in rows:
        if not r['benchhit']:
            problems.append('%s: no BENCHHIT line - the arm it measured is unproven' % r['name'])

    print('=== %s' % series)
    print('%-14s %-9s %-8s %9s %9s %9s %9s %8s %8s %7s %6s' % (
        'run', 'arm', 'verdict',
        'pll_first', 'pll_last', 'pll_drift', 'phase_sd', 'phase_wr', 'fps', 'drops', 'scr'))
    for r in rows:
        def f(x, w=9, d=1):
            return ('%*.*f' % (w, d, x)) if isinstance(x, (int, float)) else '%*s' % (w, '-')
        drift = (r['p_last'] - r['p_first']) if (
                r['p_first'] is not None and r['p_last'] is not None) else None
        worst = r['phase_worst_tel']
        if worst is None:
            worst = r['phase_worst']
        print('%-14s %-9s %-8s %s %s %s %s %s %s %s %6s' % (
            r['name'], r['arm'], r['verdict'],
            f(r['p_first']), f(r['p_last']), f(drift), f(r['phase_sd']), f(worst, 8),
            f(r['fps'], 7), f(r['drops'], 6, 0),
            r['refresh'] and ('%dHz' % r['refresh']) or '-'))
        for fail in r['fails']:
            print('%-14s   FAIL: %s' % ('', fail))

    valid = [r for r in rows if r['verdict'] != 'INVALID']
    excluded = [r for r in rows if r['verdict'] == 'INVALID']
    print()
    for p in problems:
        print('SERIES PROBLEM: %s' % p)
    if excluded:
        print('EXCLUDED (shown above, not replaced by repeats): %s'
              % ', '.join(r['name'] for r in excluded))

    for arm in sorted({r['arm'] for r in valid}):
        sel = [r for r in valid if r['arm'] == arm]
        if not sel:
            print('%s: no valid run' % arm)
            continue
        fps = [r['fps'] for r in sel if r['fps'] is not None]
        drift = [r['p_last'] - r['p_first'] for r in sel
                 if r['p_first'] is not None and r['p_last'] is not None]
        sd = [r['phase_sd'] for r in sel if r['phase_sd'] is not None]
        last = [r['p_last'] for r in sel if r['p_last'] is not None]
        print('%s over %d valid run(s): period at end %s us | drift over the window %s us '
              '| phase error sd %s us | fps %s'
              % (arm, len(sel),
                 ('%.1f' % (sum(last) / len(last))) if last else '-',
                 (', '.join('%+.1f' % d for d in drift)) if drift else '-',
                 ('%.0f' % (sum(sd) / len(sd))) if sd else '-',
                 ('%.1f' % (sum(fps) / len(fps))) if fps else '-'))
    print()
    print('Order of importance: the period and where it drifts, then the phase error,')
    print('then fps. The drop counter is reference only.')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else '.'))
