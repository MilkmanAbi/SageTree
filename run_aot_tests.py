#!/usr/bin/env python3
"""SageTree AOT test harness.

Compiles each EXPECT-bearing .sage test through `sage --aot` -> gcc -> native
binary, then compares the binary's stdout against the test's `# EXPECT:` lines
(the same ground truth run_tests.py uses for the interpreter).

Usage:
    python3 run_aot_tests.py [dir ...] [-v]
Default dirs: lang stdlib safety perf
"""
import subprocess, glob, sys, os

SAGE = './sage'
RT   = 'obj/rt/libsage_runtime.a'
INC  = 'runtime'
DIRS = [a for a in sys.argv[1:] if not a.startswith('-')] or ['lang', 'stdlib', 'safety', 'perf']
VERBOSE = '-v' in sys.argv

def expected(fp):
    lines = open(fp, errors='replace').readlines()
    return '\n'.join(l[10:].rstrip() for l in lines if l.startswith('# EXPECT: '))

def run_one(fp):
    r = subprocess.run([SAGE, '--aot', fp], capture_output=True, timeout=30)
    if r.returncode != 0 or not r.stdout:
        return 'CRASH', (r.stderr[-400:].decode('utf-8', 'replace'))
    open('/tmp/_aot.c', 'wb').write(r.stdout)
    r2 = subprocess.run(['gcc', '-std=c11', '-O2', f'-I{INC}', '/tmp/_aot.c', RT,
                         '-o', '/tmp/_aot_bin', '-lm', '-lpthread', '-ldl', '-lffi', '-L/usr/lib/python3.12/config-3.12-x86_64-linux-gnu', '-L/usr/lib/x86_64-linux-gnu', '-lpython3.12', '-ldl', '-lm'],
                        capture_output=True)
    if r2.returncode != 0:
        return 'LINK', (r2.stderr[-400:].decode('utf-8', 'replace'))
    try:
        got = subprocess.run(['/tmp/_aot_bin'], capture_output=True, timeout=15).stdout
    except subprocess.TimeoutExpired:
        return 'TIMEOUT', ''
    exp = expected(fp)
    if got.decode('utf-8', 'replace').rstrip() == exp.rstrip():
        return 'PASS', ''
    return 'WRONG', f"exp={exp[:200]!r}\n got={got[:200]!r}"

def main():
    grand = {'PASS': 0, 'WRONG': 0, 'LINK': 0, 'CRASH': 0, 'TIMEOUT': 0}
    detail = {k: [] for k in grand}
    for d in DIRS:
        files = sorted(glob.glob(f'tests/{d}/*.sage'))
        files = [f for f in files if expected(f)]   # only EXPECT-bearing
        cat = {'PASS': 0, 'WRONG': 0, 'LINK': 0, 'CRASH': 0, 'TIMEOUT': 0}
        for f in files:
            try:
                status, info = run_one(f)
            except subprocess.TimeoutExpired:
                status, info = 'CRASH', '(sage --aot timeout)'
            cat[status] += 1
            grand[status] += 1
            if status != 'PASS':
                detail[status].append((os.path.relpath(f), info))
        print(f"{d:8} {cat['PASS']:3}/{len(files):3} PASS | "
              f"LINK:{cat['LINK']} WRONG:{cat['WRONG']} CRASH:{cat['CRASH']} TO:{cat['TIMEOUT']}")
    total = sum(grand.values())
    print(f"{'TOTAL':8} {grand['PASS']:3}/{total:3} PASS | "
          f"LINK:{grand['LINK']} WRONG:{grand['WRONG']} CRASH:{grand['CRASH']} TO:{grand['TIMEOUT']}")
    if VERBOSE:
        for k in ['CRASH', 'LINK', 'WRONG', 'TIMEOUT']:
            for name, info in detail[k]:
                print(f"\n[{k}] {name}\n{info}")
    else:
        for k in ['CRASH', 'LINK', 'WRONG', 'TIMEOUT']:
            if detail[k]:
                print(f"  {k}: {', '.join(n for n, _ in detail[k])}")

if __name__ == '__main__':
    main()
