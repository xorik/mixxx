#!/usr/bin/env python3
"""Turn one benchmark run directory into metrics plus a validity verdict.

A run is only worth reading if we can show that
  * nothing the user could notice happened (focus never moved, power source
    steady, no sound device was ever opened),
  * frames were really produced for the whole measured window,
  * and the code path under test was really executed.

Anything that cannot be shown becomes a FAIL/WARN line here instead of silently
polluting the numbers.
"""

import json
import os
import re
import subprocess
import sys
from datetime import datetime

KV = re.compile(r'(\w+)=("?)([-\d.]+)\2')
TS = re.compile(r'^(\d{2}):(\d{2}):(\d{2})\.(\d{3})')


def read(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def meta_of(outdir):
    meta = {}
    for line in read(os.path.join(outdir, 'meta.txt')).splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            meta.setdefault(k, []).append(v)
    return meta


def phases_of(outdir):
    ph = {}
    for line in read(os.path.join(outdir, 'phases.txt')).splitlines():
        parts = line.split()
        if len(parts) >= 2:
            ph[parts[1]] = float(parts[0])
    return ph


def seconds_of_day(epoch):
    d = datetime.fromtimestamp(epoch)
    return d.hour * 3600 + d.minute * 60 + d.second + d.microsecond / 1e6


def parse_waveperf(outdir, t0, t1):
    rows = []
    for line in read(os.path.join(outdir, 'waveperf.txt')).splitlines():
        m = TS.match(line)
        if not m:
            continue
        sod = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1e3
        if t0 is not None and not (t0 <= sod <= t1):
            continue
        row = {k: float(v) for k, _, v in KV.findall(line[line.index('WAVEPERF'):])}
        row['t'] = sod
        rows.append(row)
    return rows


def cfg_of(outdir):
    """The config the run was actually started with, section-aware."""
    cfg = {}
    section = ''
    for line in read(os.path.join(outdir, 'mixxx.cfg.used')).splitlines():
        line = line.rstrip()
        if line.startswith('['):
            section = line
        elif ' ' in line:
            k, v = line.split(' ', 1)
            cfg['%s%s' % (section, k)] = v
    return cfg


def pct(values, q):
    if not values:
        return None
    s = sorted(values)
    return s[min(len(s) - 1, int(q * (len(s) - 1)))]


def mean(values):
    return sum(values) / len(values) if values else None


def main(outdir):
    meta = meta_of(outdir)
    ph = phases_of(outdir)
    stdout = read(os.path.join(outdir, 'stdout.txt'))
    # Early hooks log before the log file exists, so both streams count.
    log = read(os.path.join(outdir, 'mixxx.log')) + stdout

    fails, warns = [], []
    cfg = cfg_of(outdir)

    t0 = t1 = None
    if 'measure_start' in ph and 'measure_end' in ph:
        t0, t1 = seconds_of_day(ph['measure_start']), seconds_of_day(ph['measure_end'])
    rows = parse_waveperf(outdir, t0, t1)

    # ---------------------------------------------------------------- focus
    front = []
    for line in read(os.path.join(outdir, 'frontapp.tsv')).splitlines():
        parts = line.split('\t')
        if len(parts) >= 2:
            front.append((float(parts[0]), parts[1], parts[2] if len(parts) > 2 else ''))
    front_names = sorted({n for _, n, _ in front if n and n != '?'})
    front_before = meta.get('front_before', ['?'])[0]
    if not front:
        fails.append('no frontmost-application samples were recorded')
    elif len(front_names) > 1:
        fails.append('the user switched applications during the run (%s) — the run is '
                     'contaminated, not necessarily by us' % ', '.join(front_names))
    elif front_names and front_names[0] != front_before:
        fails.append('frontmost application is %s, was %s before the run'
                     % (front_names[0], front_before))
    if any('ixxx' in n for n in front_names):
        fails.append('MIXXX CAME TO THE FRONT — the run disturbed the user')

    powers = sorted({p for _, _, p in front if p})
    if len(powers) > 1:
        fails.append('power source changed during the run: %s' % ', '.join(powers))
    elif powers and powers[0] != 'AC Power':
        warns.append('ran on %s' % powers[0])

    # --------------------------------------------------------- background mode
    # Judge the STATE the process ended up in, not the return value of the call
    # that set it: a bundle marked LSUIElement is accessory before we ask, and
    # asking again returns NO while the mode is perfectly active. Log format
    # shared with MIX-11 so both projects parse with the same code.
    m = re.search(r'BENCHHIT MIXXX_BENCH_BACKGROUND=1 policyWas=(\w+) policyNow=(\w+)', log)
    if m:
        if m.group(2) != 'accessory':
            fails.append('the process is in the "%s" activation policy, not accessory — '
                         'it can come to the front' % m.group(2))
    else:
        m_old = re.search(r'accessoryPolicyApplied=(\w+)', log)
        if not m_old:
            fails.append('the accessory-activation hook did not run — this build cannot '
                         'be trusted to stay out of the foreground')
        elif m_old.group(1) not in ('true', '1'):
            fails.append('the old-format hook reported no accessory policy')

    # ------------------------------------------------------------ which screen
    screen = {}
    # Names are logged quoted by qDebug, which is convenient: they contain spaces.
    m = re.search(r'BENCHSCREEN reason="(\w+)" window=(\d+)x(\d+)\+(-?\d+)\+(-?\d+) '
                  r'screen="([^"]*)" refreshHz=([\d.]+) dpr=([\d.]+)', log)
    if m:
        screen = {'reason': m.group(1),
                  'window': '%sx%s+%s+%s' % m.group(2, 3, 4, 5),
                  'name': m.group(6),
                  'refresh_hz': float(m.group(7)),
                  'dpr': float(m.group(8))}
    else:
        warns.append('the screen the window landed on was not reported')
    layout = sorted(set(re.findall(
        r'BENCHSCREENS name="([^"]*)" geometry=(\S+) refreshHz=([\d.]+) dpr=([\d.]+)', log)))
    # With one screen the benchmark window cannot be parked away from the user:
    # it runs on the screen they are working on. That does not invalidate the
    # run, but it has to travel WITH the numbers, not in someone's memory.
    m_count = re.search(r'screenCount=(\d+)', log)
    screen_count = int(m_count.group(1)) if m_count else (len(layout) or None)
    single_screen = screen_count == 1 or meta.get('screens_system', ['0'])[0] == '1'
    if single_screen:
        warns.append('measured on the user\'s only screen: the window could not be parked '
                     'anywhere else, so window dragging, scrolling or another GL '
                     'application may have contaminated this run')

    # ------------------------------------------------------------- PLL lock
    # A run whose PLL never finished initialising still produces frames, but at
    # the startup default period rather than the display's: the numbers look
    # normal and describe the wrong synchronisation. Observed on a ProMotion
    # panel, whose reported rate does not match its real cadence while the
    # window is inactive - which is how every benchmark window runs.
    pll = {}
    m = re.search(r'PLL locked to ([\d.]+) us \(display reports ([\d.]+) us\)', log)
    gave_up = re.search(r'PLL display cross-check gave up after (\d+)', log)
    uses_pll = 'ST_PLL' in log or m or 'phase-locked-loop' in log
    if m:
        pll = {'period_us': float(m.group(1)), 'display_us': float(m.group(2)),
               'cross_check_gave_up': bool(gave_up)}
        refresh = screen.get('refresh_hz')
        if refresh and refresh > 1:
            expected_us = 1e6 / refresh
            off = abs(pll['period_us'] - expected_us) / expected_us
            if off > 0.05:
                fails.append('the PLL locked to %.0f us but the screen it is on runs at '
                             '%.3g Hz (%.0f us): the run is synchronised to something else'
                             % (pll['period_us'], refresh, expected_us))
        if gave_up:
            warns.append('the PLL gave up cross-checking against the display after %s '
                         'implausible deltas and locked on the median alone'
                         % gave_up.group(1))
    elif uses_pll and rows:
        fails.append('no "PLL locked to" line: the loop never left initialisation, so the '
                     'period is still the startup default and the synchronisation of this '
                     'run is not the display\'s')

    # ---------------------------------------------------------------- audio
    if re.search(r'SoundDevicePortAudio.*\bopen\b', log):
        fails.append('a real audio device was opened — the run could make sound')
    if 'MIXXX_BENCH_NO_AUDIO' not in read(os.path.join(outdir, 'meta.txt')) and \
            'SoundManager::setupDevices' not in log:
        warns.append('could not confirm the audio configuration from the log')
    n_outputs = len(re.findall(r'SoundDevice.*Adding output', log))

    # --------------------------------------------------------------- frames
    if not rows:
        fails.append('no WAVEPERF telemetry in the measured window — nothing was rendered')
    else:
        expected = float(meta.get('measure_s', ['0'])[0] or 0)
        if expected and len(rows) < expected * 0.8:
            fails.append('only %d telemetry seconds for a %gs window — rendering stalled'
                         % (len(rows), expected))
        if all(r.get('fps', 0) < 1 for r in rows):
            fails.append('fps is zero for the whole window — the window is probably occluded')

    fps = [r.get('fps', 0) for r in rows]
    frame_ms = [r.get('meanMs', 0) for r in rows]
    metrics = {
        'seconds': len(rows),
        'fps_mean': mean(fps),
        'fps_min': min(fps) if fps else None,
        'frame_ms_mean': mean(frame_ms),
        'frame_ms_p50': mean([r.get('p50Ms', 0) for r in rows]),
        'frame_ms_p95': mean([r.get('p95Ms', 0) for r in rows]),
        'frame_ms_p99': mean([r.get('p99Ms', 0) for r in rows]),
        'frame_ms_max': max([r.get('maxMs', 0) for r in rows]) if rows else None,
        'drops_total': sum(r.get('dropsDelta', 0) for r in rows),
        'drops_worst_second': max([r.get('dropsDelta', 0) for r in rows]) if rows else None,
        'device_px': rows[-1].get('totalDevicePx') if rows else None,
        'widgets': rows[-1].get('widgets') if rows else None,
        'waveform_type': rows[-1].get('type') if rows else None,
        'target_fps': rows[-1].get('targetFps') if rows else None,
        'audio_outputs': n_outputs,
        'renderer_options': rows[-1].get('options') if rows else None,
        'pll_locked_us': pll.get('period_us'),
        'pll_display_us': pll.get('display_us'),
        'pll_cross_check_gave_up': pll.get('cross_check_gave_up'),
        'profile_version': cfg.get('[Config]Version'),
        'screen': screen.get('name'),
        'screen_refresh_hz': screen.get('refresh_hz'),
        'window': screen.get('window'),
        'dpr': screen.get('dpr'),
    }
    # A frame rate above the refresh rate of the screen the window is on means
    # the frames were never presented, so the numbers describe nothing.
    if screen.get('refresh_hz') and metrics['fps_mean']:
        if metrics['fps_mean'] > screen['refresh_hz'] * 1.05:
            warns.append('fps %.1f exceeds the %.1f Hz refresh rate of %s — frames are '
                         'not being presented' % (metrics['fps_mean'],
                                                  screen['refresh_hz'], screen['name']))

    # ------------------------------- was the requested configuration measured?
    # Same class of error as a broken shader with perfect telemetry: Mixxx can
    # silently substitute a waveform type (its pre-2.6 upgrade path replaces an
    # unknown WaveformType with plain RGB) or be built with different renderer
    # options, and nothing in the numbers would look wrong.
    if rows:
        for key, field, what in (('[Waveform]WaveformType', 'type', 'waveform type'),
                                 ('[Waveform]waveform_options', 'options', 'renderer options')):
            requested = cfg.get(key)
            if requested is None:
                continue
            effective = rows[-1].get(field)
            if effective is None or effective < 0:
                warns.append('this build does not report the effective %s' % what)
            elif int(effective) != int(requested):
                fails.append('%s requested %s but %s was rendered — Mixxx substituted it, '
                             'the run measures something else' % (what, requested, int(effective)))
    version = cfg.get('[Config]Version', '')
    if version:
        parts = version.split('-')[0].split('.')
        if len(parts) >= 2 and (int(parts[0]), int(parts[1])) < (2, 6):
            fails.append('profile version %s predates 2.6.0: the upgrade path rewrites '
                         'WaveformType on every start' % version)

    # ------------------------------------------------ was the patch executed?
    binary = meta.get('bin', [''])[0]
    strings_blob = ''
    if binary and os.path.exists(binary):
        try:
            strings_blob = subprocess.run(['strings', binary], capture_output=True,
                                          text=True, timeout=120).stdout
        except Exception:
            strings_blob = ''
    flags = {}
    for kv in meta.get('env', []):
        name, _, value = kv.partition('=')
        if not name.startswith('MIXXX_BENCH_'):
            continue
        flags[name] = value
        if not value:
            continue                      # empty value = flag deliberately off
        known = (not strings_blob) or (name in strings_blob)
        hit = re.search(r'BENCHHIT "?%s"?' % re.escape(name), log) is not None
        if not known:
            fails.append('%s is not read by this binary — the change under test '
                         'is NOT in the build' % name)
        elif not hit:
            warns.append('%s: the binary reads this variable, but no BENCHHIT line proves '
                         'the patched code path actually ran' % name)

    verdict = 'INVALID' if fails else ('SUSPECT' if warns else 'VALID')
    summary = {
        'outdir': outdir,
        'label': meta.get('label', ['?'])[0],
        'verdict': verdict,
        'fails': fails,
        'warns': warns,
        'metrics': metrics,
        'flags': flags,
        'screen_layout': [{'name': n, 'geometry': g, 'refresh_hz': float(r), 'dpr': float(d)}
                          for n, g, r, d in layout],
        'single_screen': single_screen,
        'screen_count': screen_count,
        'screens_system': meta.get('screens_system', [None])[0],
        'gui_apps': [meta.get('gui_apps_before', [None])[0], meta.get('gui_apps_after', [None])[0]],
        'front_app': front_names,
        'front_samples': len(front),
    }
    with open(os.path.join(outdir, 'summary.json'), 'w') as f:
        json.dump(summary, f, indent=2)

    print('--- %s [%s]' % (summary['label'], verdict))
    if rows:
        print('    fps %.1f (min %.1f)  frame %.2f ms  p95 %.2f  p99 %.2f  max %.2f  '
              'drops %d (worst second %d)  px %.0f'
              % (metrics['fps_mean'], metrics['fps_min'], metrics['frame_ms_mean'],
                 metrics['frame_ms_p95'], metrics['frame_ms_p99'], metrics['frame_ms_max'],
                 metrics['drops_total'], metrics['drops_worst_second'],
                 metrics['device_px'] or 0))
        print('    front app: %s (%d samples, %s)' % (
            ', '.join(front_names) or '?', len(front),
            'unchanged' if len(front_names) < 2 else 'CHANGED'))
        if screen:
            print('    window %s on screen %s @ %.3g Hz, dpr %.3g' % (
                screen['window'], screen['name'], screen['refresh_hz'], screen['dpr']))
        if pll:
            print('    PLL locked to %.0f us (display reports %.0f us)%s' % (
                pll['period_us'], pll['display_us'],
                ', cross-check gave up' if pll['cross_check_gave_up'] else ''))
    for f_ in fails:
        print('    FAIL: %s' % f_)
    for w in warns:
        print('    WARN: %s' % w)
    print('    %s/summary.json' % outdir)
    return 0 if verdict != 'INVALID' else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else '.'))
