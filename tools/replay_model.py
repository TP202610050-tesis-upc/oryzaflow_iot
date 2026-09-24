#!/usr/bin/env python3
"""
replay_model.py — recalcula fuera de línea la telemetría del nodo OryzaFlow (F6, docs/fallos_firmware.md).

Reproduce EXACTAMENTE las ecuaciones de firmware/oryzaflow_edge/drying_model.cpp (v1.1.0):
  * Chung-Pfost modificada en base seca -> base húmeda (F1/F2)
  * Page / Midilli con integración por pasos y tiempo equivalente (F10)
  * estado por segmento diario (F3): M_prev se reinicia al cambiar de segmento; el primer dr se omite
Toda la aritmética se redondea a float32 tras cada operación (struct.pack) para imitar al ESP32-S3.
Solo usa la biblioteca estándar (sin numpy/pandas).

Uso típico
----------
  # auto-test (mismos casos que selfTestModel() del firmware):
  python tools/replay_model.py --self-check

  # serie exportada (CSV ancho: ts,t,rh[,me,m,mr,dr,te,seg,om]) con un solo segmento:
  python tools/replay_model.py serie.csv --m0 22 --m-target 12 --k 0.12 --n 1.05 --start-ts 2026-10-03T07:00:00-05:00

  # varios segmentos (JSON tal como lo devuelve GET /api/v1/batches/{id}/segments):
  python tools/replay_model.py serie.csv --segments segmentos.json --m-target 12 --k 0.12 --n 1.05 --out recalculado.csv

  # export "largo" de ThingsBoard (ts,key,value):
  python tools/replay_model.py export_tb.csv --long --segments segmentos.json --m-target 12 --k 0.12 --n 1.05

Columnas reconocidas (insensible a mayúsculas; también las de la tabla sensor_readings del backend):
  ts|timestamp, t|temperatura, rh|humedad, me|humedad_equilibrio, m|humedad_grano, mr|razon_humedad,
  dr|tasa_secado, te|tiempo_restante_horas, seg|segmento, om|offset_m
`ts` acepta epoch en ms, epoch en s o ISO-8601 (sin zona => UTC).

Salida: diferencia máxima y media |publicado - recalculado| para me, m, te (criterio: <= 0.01 pp en m y me,
<= 0.05 h en te) y, con --out, el CSV recalculado.
"""
import argparse
import csv
import json
import math
import struct
import sys
from datetime import datetime, timezone

# ----------------------------------------------------------------------------- float32 helpers
def f32(x):
    """Redondea a IEEE-754 binary32 (lo que hace el ESP32 con cada operación float)."""
    return struct.unpack('<f', struct.pack('<f', float(x)))[0]

def logf(x):  return f32(math.log(x))
def expf(x):  return f32(math.exp(x))
def powf(x, y): return f32(math.pow(x, y))

NAN = float('nan')

# ----------------------------------------------------------------------------- constantes (config.h)
CP_E = f32(29.394)
CP_F = f32(4.6015)
CP_C = f32(35.703)
MODEL_EPS = f32(1e-4)
MODEL_MR_MIN = f32(1e-6)
MODEL_T_MAX_H = f32(500.0)
MODEL_TIME_SCALE = f32(1.0)
F100 = f32(100.0)
F1 = f32(1.0)

# ----------------------------------------------------------------------------- drying_model.cpp
def moisture_bs_to_bh(bs):
    return f32(f32(F100 * bs) / f32(F100 + bs))

def moisture_bh_to_bs(bh):
    return f32(f32(F100 * bh) / f32(F100 - bh))

def equilibrium_moisture_bs(tC, rhPct):
    hr = f32(rhPct / F100)
    if hr < f32(0.01): hr = f32(0.01)
    if hr > f32(0.99): hr = f32(0.99)
    arg = f32(-(f32(tC + CP_C)) * logf(hr))
    if arg <= 0.0:
        return NAN
    return f32(CP_E - f32(CP_F * logf(arg)))

def equilibrium_moisture_bh(tC, rhPct):
    bs = equilibrium_moisture_bs(tC, rhPct)
    if math.isnan(bs):
        return NAN
    return moisture_bs_to_bh(bs)


class LotParams:
    def __init__(self, m0, mTarget, k, n, model='PAGE', a=1.0, b=0.0, offsetM=0.0, segmento=0, startMs=0.0, active=True):
        self.active = active
        self.startMs = float(startMs)      # epoch ms (double, como startMs en escala millis)
        self.m0 = f32(m0)
        self.mTarget = f32(mTarget)
        self.k = f32(k)
        self.n = f32(n)
        self.model = model.upper()
        self.a = f32(a)
        self.b = f32(b)
        self.offsetM = f32(offsetM)
        self.segmento = int(segmento)


class ModelState:
    def __init__(self):
        self.reset()

    def reset(self):
        self.valid = False
        self.mPrev = NAN
        self.tEff = f32(0.0)
        self.prevMs = 0.0


def model_mr(lp, tH):
    if tH < 0.0: tH = f32(0.0)
    page = expf(f32(-lp.k * powf(tH, lp.n)))
    if lp.model == 'MIDILLI':
        return f32(f32(lp.a * page) + f32(lp.b * tH))
    return page


def _is_page_equivalent(lp):
    return lp.model == 'PAGE' or (lp.a == F1 and lp.b == f32(0.0))


def model_time_for_mr(lp, mr):
    if _is_page_equivalent(lp):
        if mr >= F1: return f32(0.0)
        if mr <= 0.0: return NAN
        return powf(f32(-logf(mr) / lp.k), f32(F1 / lp.n))
    if mr >= lp.a: return f32(0.0)
    lo, hi = f32(0.0), MODEL_T_MAX_H
    if model_mr(lp, hi) > mr:
        return hi
    for _ in range(40):
        mid = f32(f32(0.5) * f32(lo + hi))
        if model_mr(lp, mid) > mr:
            lo = mid
        else:
            hi = mid
    return f32(f32(0.5) * f32(lo + hi))


def compute_drying_state(lp, tC, rhPct, nowMs, ms):
    """Devuelve dict con me, hasB, m, mr, te, hasDr, dr, seg, om y actualiza ms. Espejo de computeDryingState()."""
    tC, rhPct = f32(tC), f32(rhPct)
    s = {'me': equilibrium_moisture_bh(tC, rhPct), 'hasB': False, 'm': NAN, 'mr': NAN, 'te': NAN,
         'hasDr': False, 'dr': NAN, 'seg': 0, 'om': f32(0.0)}
    if not lp.active:
        return s
    if not (lp.k > 0.0) or not (lp.n > 0.0) or math.isnan(s['me']):
        return s

    me = s['me']
    denom = f32(lp.m0 - me)
    drying = False

    if not ms.valid:
        tH = f32(f32((nowMs - lp.startMs) / 3600000.0) * MODEL_TIME_SCALE)
        if tH < 0.0: tH = f32(0.0)
        if denom <= MODEL_EPS:
            mModel = lp.m0
            tEff = f32(0.0)
        else:
            mModel = f32(me + f32(denom * model_mr(lp, tH)))
            tEff = tH
            drying = True
    else:
        dt = f32(f32((nowMs - ms.prevMs) / 3600000.0) * MODEL_TIME_SCALE)
        if dt < 0.0: dt = f32(0.0)
        mrPrev = f32(f32(ms.mPrev - me) / denom) if denom > MODEL_EPS else f32(0.0)
        if mrPrev <= 0.0:
            mModel = ms.mPrev
            tEff = ms.tEff
        else:
            if mrPrev > F1: mrPrev = F1
            if mrPrev < MODEL_MR_MIN: mrPrev = MODEL_MR_MIN
            tEq = model_time_for_mr(lp, mrPrev)
            tEff = f32(tEq + dt)
            mModel = f32(me + f32(denom * model_mr(lp, tEff)))
            drying = True
        if dt > 0.0:
            s['dr'] = f32(f32(ms.mPrev - mModel) / dt)
            s['hasDr'] = True

    mr = f32(f32(mModel - me) / denom) if denom > MODEL_EPS else F1
    if mr < 0.0: mr = f32(0.0)
    if mr > F1: mr = F1
    s['mr'] = mr
    s['m'] = f32(mModel + lp.offsetM)
    s['hasB'] = True

    mrObj = f32(f32(lp.mTarget - me) / denom) if denom > MODEL_EPS else f32(-1.0)
    if drying and mrObj > 0.0 and mrObj < F1 and mModel > lp.mTarget:
        te = f32(model_time_for_mr(lp, mrObj) - tEff)
        s['te'] = te if te > 0.0 else f32(0.0)
    else:
        s['te'] = f32(0.0)

    s['seg'] = lp.segmento
    s['om'] = lp.offsetM

    ms.mPrev = mModel
    ms.tEff = tEff
    ms.prevMs = nowMs
    ms.valid = True
    return s


# ----------------------------------------------------------------------------- auto-test (= selfTestModel())
def self_check():
    fails = 0
    def near(a, b, tol): return abs(a - b) <= tol

    me1 = equilibrium_moisture_bh(24.98, 66.20)
    me2 = equilibrium_moisture_bh(28.0, 60.0)
    bs2 = equilibrium_moisture_bs(28.0, 60.0)
    okA = near(me1, 12.72, 0.01) and near(me2, 11.79, 0.01) and near(bs2, 13.37, 0.01)
    print(f"  [{'PASS' if okA else 'FAIL'}] (a) CP: me(24.98,66.2)={me1:.4f} (esp 12.72)  me(28,60)={me2:.4f} b.h. / {bs2:.4f} b.s. (esp 11.79 / 13.37)")
    fails += not okA

    lp = LotParams(m0=22.0, mTarget=13.0, k=0.12, n=1.05, segmento=1, startMs=0.0)
    H = 3600000.0

    st0 = ModelState()
    z = compute_drying_state(lp, 42.0, 45.0, 0.0, st0)
    okB = near(z['m'], 22.0, 0.005)
    print(f"  [{'PASS' if okB else 'FAIL'}] (b) M(t=0)={z['m']:.4f} (esp 22.00)")
    fails += not okB

    st1 = ModelState()
    a = compute_drying_state(lp, 42.0, 45.0, 4.0 * H, st1)
    b = compute_drying_state(lp, 42.0, 45.0, 5.0 * H, st1)
    st2 = ModelState()
    c = compute_drying_state(lp, 42.0, 45.0, 4.0 * H, st2)
    for i in range(1, 13):
        c = compute_drying_state(lp, 42.0, 45.0, (4.0 + i / 12.0) * H, st2)
    okC = (near(b['me'], 9.42, 0.01) and near(b['mr'], 0.522, 0.001) and near(b['m'], 15.99, 0.01)
           and near(b['te'], 4.36, 0.01) and b['hasDr'] and near(b['dr'], 0.96, 0.01) and not a['hasDr']
           and near(c['m'], b['m'], 0.01))
    print(f"      t=4h: me={a['me']:.4f} m={a['m']:.4f} mr={a['mr']:.4f} te={a['te']:.4f} dr={'-' if not a['hasDr'] else a['dr']}")
    print(f"      t=5h: me={b['me']:.4f} m={b['m']:.4f} mr={b['mr']:.4f} te={b['te']:.4f} dr={b['dr']:.4f}")
    print(f"  [{'PASS' if okC else 'FAIL'}] (c) @5h esperado 9.42 / 15.99 / 0.522 / 4.36 / 0.96; 12x5min m={c['m']:.6f} (|d|={abs(c['m'] - b['m']):.2e})")
    fails += not okC

    lm = LotParams(m0=22.0, mTarget=13.0, k=0.12, n=1.05, model='MIDILLI', a=1.0, b=0.0, segmento=1)
    st3 = ModelState()
    d = compute_drying_state(lm, 42.0, 45.0, 4.0 * H, st3)
    d = compute_drying_state(lm, 42.0, 45.0, 5.0 * H, st3)
    okD = d['m'] == b['m'] and d['te'] == b['te'] and d['mr'] == b['mr']
    print(f"  [{'PASS' if okD else 'FAIL'}] (d) Midilli(a=1,b=0) m={d['m']:.6f} te={d['te']:.6f} == Page m={b['m']:.6f} te={b['te']:.6f}")
    fails += not okD

    # (e) Me variable no produce saltos: serie sintética (mañana húmeda -> mediodía seco -> tarde húmeda);
    #     m debe ser monótona no creciente dentro del segmento (criterio F10).
    lp2 = LotParams(m0=22.0, mTarget=12.0, k=0.12, n=1.05, segmento=1, startMs=0.0)
    st4 = ModelState()
    prev, mono, maxjump = None, True, 0.0
    for i in range(0, 12 * 10 + 1):           # 10 h a pasos de 5 min
        h = i / 12.0
        t = 22.0 + 10.0 * math.sin(math.pi * h / 10.0)          # 22 -> 32 -> 22 °C
        rh = 80.0 - 35.0 * math.sin(math.pi * h / 10.0)         # 80 -> 45 -> 80 %
        r = compute_drying_state(lp2, t, rh, h * H, st4)
        if prev is not None:
            if r['m'] > prev + 1e-6: mono = False
            maxjump = max(maxjump, abs(r['m'] - prev))
        prev = r['m']
    okE = mono and maxjump < 0.5
    print(f"  [{'PASS' if okE else 'FAIL'}] (e) Me variable 10 h: m monótona={mono}, salto máximo entre pasos={maxjump:.4f} pp, m final={prev:.4f}")
    fails += not okE

    print(f"--- self-check: {'PASS' if fails == 0 else 'FAIL'} ({fails} fallos) ---")
    return fails == 0


# ----------------------------------------------------------------------------- E/S
ALIASES = {
    'ts': 'ts', 'timestamp': 'ts', 'time': 'ts',
    't': 't', 'temperatura': 't', 'temp': 't',
    'rh': 'rh', 'humedad': 'rh',
    'me': 'me', 'humedad_equilibrio': 'me', 'humedadequilibrio': 'me',
    'm': 'm', 'humedad_grano': 'm', 'humedadgrano': 'm',
    'mr': 'mr', 'razon_humedad': 'mr', 'razonhumedad': 'mr',
    'dr': 'dr', 'tasa_secado': 'dr', 'tasasecado': 'dr',
    'te': 'te', 'tiempo_restante_horas': 'te', 'tiemporestantehoras': 'te',
    'seg': 'seg', 'segmento': 'seg',
    'om': 'om', 'offset_m': 'om', 'offsetm': 'om',
    'key': 'key', 'value': 'value',
}


def parse_ts(v):
    """epoch ms | epoch s | ISO-8601 -> epoch ms (float)."""
    if v is None:
        raise ValueError("ts vacío")
    s = str(v).strip()
    if s == '':
        raise ValueError("ts vacío")
    try:
        x = float(s)
        if x > 1e11:      # ms
            return x
        return x * 1000.0  # s
    except ValueError:
        pass
    s2 = s.replace('Z', '+00:00')
    if ' ' in s2 and 'T' not in s2:
        s2 = s2.replace(' ', 'T', 1)
    dt = datetime.fromisoformat(s2)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.timestamp() * 1000.0


def fnum(v):
    if v is None: return None
    s = str(v).strip()
    if s == '' or s.lower() in ('nan', 'null', 'none'): return None
    return float(s)


def read_rows(path, long_format):
    with open(path, newline='', encoding='utf-8-sig') as fh:
        reader = csv.DictReader(fh)
        raw = list(reader)
    if not raw:
        return []
    def norm(k): return ALIASES.get(k.strip().lower(), k.strip().lower())
    rows = []
    if long_format:
        by_ts = {}
        for r in raw:
            rr = {norm(k): v for k, v in r.items()}
            ts = parse_ts(rr['ts'])
            by_ts.setdefault(ts, {'ts': ts})[norm(rr['key'])] = rr['value']
        for ts in sorted(by_ts):
            rows.append(by_ts[ts])
    else:
        for r in raw:
            rr = {norm(k): v for k, v in r.items()}
            rr['ts'] = parse_ts(rr['ts'])
            rows.append(rr)
        rows.sort(key=lambda r: r['ts'])
    return rows


def load_segments(path):
    with open(path, encoding='utf-8') as fh:
        data = json.load(fh)
    segs = []
    for s in data:
        segs.append({
            'numero': int(s.get('numero', len(segs) + 1)),
            'inicioTs': parse_ts(s['inicioTs']),
            'finTs': parse_ts(s['finTs']) if s.get('finTs') not in (None, '') else None,
            'm0': float(s.get('m0', s.get('m0Segmento'))),
        })
    segs.sort(key=lambda s: s['inicioTs'])
    return segs


def segment_for(segs, ts):
    for s in segs:
        if ts >= s['inicioTs'] and (s['finTs'] is None or ts <= s['finTs']):
            return s
    return None


def replay(rows, segs, args):
    out = []
    state = ModelState()
    current = None
    for r in rows:
        ts = r['ts']
        t = fnum(r.get('t')); rh = fnum(r.get('rh'))
        if t is None or rh is None:
            continue
        seg = segment_for(segs, ts)
        if seg is None:
            if current is not None:
                state.reset(); current = None
            lp = LotParams(0, 0, 0, 0, active=False)
        else:
            if current is None or seg['numero'] != current['numero']:
                state.reset(); current = seg
            lp = LotParams(m0=seg['m0'], mTarget=args.m_target, k=args.k, n=args.n, model=args.model,
                           a=args.a, b=args.b, offsetM=args.offset_m, segmento=seg['numero'],
                           startMs=seg['inicioTs'], active=True)
        s = compute_drying_state(lp, t, rh, ts, state)
        out.append({'ts': ts, 't': f32(t), 'rh': f32(rh), 'calc': s,
                    'pub': {k: fnum(r.get(k)) for k in ('me', 'm', 'mr', 'dr', 'te')}})
    return out


def report(out):
    stats = {}
    for key, tol in (('me', 0.01), ('m', 0.01), ('te', 0.05), ('mr', None), ('dr', None)):
        diffs = []
        for o in out:
            p = o['pub'].get(key)
            c = o['calc'].get(key)
            if p is None or c is None or math.isnan(c):
                continue
            if key in ('m', 'mr', 'dr', 'te') and not o['calc']['hasB']:
                continue
            if key == 'dr' and not o['calc']['hasDr']:
                continue
            diffs.append(abs(p - c))
        if diffs:
            mx, mean = max(diffs), sum(diffs) / len(diffs)
            verdict = '' if tol is None else ('  OK' if mx <= tol else f'  EXCEDE {tol}')
            print(f"  {key:>3}: n={len(diffs):5d}  max|d|={mx:.4f}  mean|d|={mean:.4f}{verdict}")
            stats[key] = (len(diffs), mx, mean)
        else:
            print(f"  {key:>3}: sin columna publicada para comparar")
    return stats


def write_out(path, out):
    cols = ['ts', 'ts_iso', 't', 'rh', 'me', 'lot', 'm', 'mr', 'dr', 'te', 'seg', 'om',
            'pub_me', 'pub_m', 'pub_mr', 'pub_dr', 'pub_te']
    with open(path, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for o in out:
            c = o['calc']; p = o['pub']
            iso = datetime.fromtimestamp(o['ts'] / 1000.0, tz=timezone.utc).isoformat()
            def fmt(x, nd=4):
                return '' if x is None or (isinstance(x, float) and math.isnan(x)) else f"{x:.{nd}f}"
            w.writerow([int(o['ts']), iso, fmt(o['t'], 2), fmt(o['rh'], 2), fmt(c['me']), 'true' if c['hasB'] else 'false',
                        fmt(c['m']) if c['hasB'] else '', fmt(c['mr']) if c['hasB'] else '',
                        fmt(c['dr']) if c['hasDr'] else '', fmt(c['te']) if c['hasB'] else '',
                        c['seg'] if c['hasB'] and c['seg'] > 0 else '', fmt(c['om'], 2) if c['hasB'] else '',
                        fmt(p['me']), fmt(p['m']), fmt(p['mr']), fmt(p['dr']), fmt(p['te'])])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv', nargs='?', help='serie exportada (ts,t,rh[,me,m,mr,dr,te,seg,om])')
    ap.add_argument('--long', action='store_true', help='entrada en formato largo (ts,key,value)')
    ap.add_argument('--segments', help='JSON de segmentos [{numero, inicioTs, finTs, m0}]')
    ap.add_argument('--start-ts', help='inicio del único segmento (ISO o epoch) si no se usa --segments')
    ap.add_argument('--m0', type=float, help='M0 del único segmento [%% b.h.]')
    ap.add_argument('--m-target', type=float, default=12.0, help='M_target [%% b.h.] (def. 12.0)')
    ap.add_argument('--k', type=float, default=0.12)
    ap.add_argument('--n', type=float, default=1.05)
    ap.add_argument('--model', choices=['PAGE', 'MIDILLI'], default='PAGE')
    ap.add_argument('--a', type=float, default=1.0)
    ap.add_argument('--b', type=float, default=0.0)
    ap.add_argument('--offset-m', type=float, default=0.0)
    ap.add_argument('--out', help='CSV recalculado')
    ap.add_argument('--self-check', action='store_true', help='ejecuta el auto-test del modelo y sale')
    args = ap.parse_args(argv)

    if args.self_check:
        print("--- self-check replay_model.py (espejo de selfTestModel() del firmware v1.1.0) ---")
        return 0 if self_check() else 1

    if not args.csv:
        ap.error('falta el CSV de entrada (o use --self-check)')
    if args.segments:
        segs = load_segments(args.segments)
    elif args.start_ts is not None and args.m0 is not None:
        segs = [{'numero': 1, 'inicioTs': parse_ts(args.start_ts), 'finTs': None, 'm0': args.m0}]
    else:
        ap.error('indique --segments o bien --start-ts y --m0')

    rows = read_rows(args.csv, args.long)
    if not rows:
        print('CSV sin filas'); return 1
    out = replay(rows, segs, args)
    nB = sum(1 for o in out if o['calc']['hasB'])
    print(f"filas={len(out)}  con lote activo={nB}  segmentos={len(segs)}  modelo={args.model} k={args.k} n={args.n} mTarget={args.m_target}")
    print("diferencias |publicado - recalculado|:")
    report(out)
    if args.out:
        write_out(args.out, out)
        print(f"recalculado -> {args.out}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
