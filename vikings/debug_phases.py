#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
debug_phases.py — Verifica o ciclo correto dos Vikings:
  FASE 1: Todos os vikings NORMAIS comem
  FASE 2: Esperam o banquete terminar (barreira chieftain_wait_banquet_over)
  FASE 3: Todos rezam (normais + atrasados) — SO apos a barreira
  FASE 4: Todos terminam

Evento real de sincronizacao:
  A barreira e liberada DENTRO de chieftain_release_seat_plates, que e chamada
  ANTES do plog("has finished eating"). Por isso usamos o log de DEBUG
  "Vikings Finished: N" como marcador do fim real do banquete, e nao o
  "has finished eating" (que e so um plog tardio, apos o retorno da funcao).

Uso:
  python3 debug_phases.py [-v N] [-c N] [-e MS] [-p MS] [--no-compile]
"""

import subprocess, sys, re, os, argparse, io

# Forca UTF-8 na saida (necessario no Windows/cp1252)
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

# ANSI cores
R="\033[0m"; B="\033[1m"; GR="\033[32m"; YL="\033[33m"
CY="\033[36m"; RD="\033[31m"; BL="\033[34m"; MG="\033[35m"; DM="\033[2m"

OK_S  = "[OK]"; FAIL_S = "[!!]"; ARR = "-->"; PRAY = "[P]"
WAIT  = "[~]";  WARN   = "[!]";  DONE = "[V]"

def hdr(txt, c=CY):
    bar = "=" * 65
    print(f"\n{c}{B}+{bar}+\n|  {txt:<63}|\n+{bar}+{R}")

def phase_open(n, txt, c):
    pad = "-" * max(0, 50 - len(txt))
    print(f"\n{c}{B}+-- FASE {n}: {txt} {pad}+{R}")

def phase_close(c):
    print(f"{c}{B}+{'-'*60}+{R}")

# Compilacao
def compile_debug(d):
    print(f"{CY}[*] Compilando em modo DEBUG (RELEASE=false)...{R}")
    r = subprocess.run(
        ["make", "-C", d, "RELEASE=false", "-B"],
        capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    if r.returncode:
        print(f"{RD}[ERRO] Falha na compilacao:{R}\n{r.stderr}"); sys.exit(1)
    print(f"{GR}{OK_S} Compilacao OK{R}")

# Execucao
def run_prog(d, extra_args):
    exe = f"{d}/program"
    for cand in (f"{d}/program", f"{d}/program.exe",
                 f"{d}/src/program", f"{d}/src/program.exe"):
        if os.path.isfile(cand):
            exe = cand; break

    cmd = [exe] + extra_args
    print(f"{CY}[*] Executando: {' '.join(cmd)}{R}\n")
    r = subprocess.run(cmd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=120)
    return r.stdout + r.stderr

# Regexes 
RE_EAT_S   = re.compile(r"\[viking\] Viking=(\d+) is now eating")
# "has finished eating" e so um plog tardio — nao e usado como evento de sincronizacao
RE_EAT_E   = re.compile(r"\[viking\] Viking=(\d+) has finished eating")
RE_PRY_S   = re.compile(r"\[viking\] Viking=(\d+) is now praying")
RE_PRY_E   = re.compile(r"\[viking\] Viking=(\d+) has finished praying")
# Este e o evento REAL de sincronizacao: Vikings Finished chega a N dentro
# de chieftain_release_seat_plates, ANTES do plog("has finished eating").
RE_FINISHED= re.compile(r"Vikings Finished:\s*(\d+)")
RE_NVIKS   = re.compile(r"Number of vikings\s*:\s*(\d+)")

# Analise 
def analyse(raw, num_normal):
    lines = raw.splitlines()
    hdr("ANALISE DAS FASES -- VIKINGS")

    # Coleta timeline (line_index, tag, valor) 
    timeline = []
    for i, ln in enumerate(lines):
        for pat, tag in [
            (RE_EAT_S,    "eat_start"),
            (RE_EAT_E,    "eat_end_log"),   # log tardio, so para exibicao
            (RE_PRY_S,    "pray_start"),
            (RE_PRY_E,    "pray_end"),
            (RE_FINISHED, "finished_n"),     # evento real de sincronizacao
        ]:
            m = pat.search(ln)
            if m:
                timeline.append((i, tag, int(m.group(1))))

    # IDs por tipo
    normal_ids = {vid for _, tag, vid in timeline if tag == "eat_start"}
    late_ids   = {vid for _, tag, vid in timeline
                  if tag == "pray_start" and vid not in normal_ids}

    pray_starts = [(i, vid) for i, tag, vid in timeline if tag == "pray_start"]

    # Linha em que "Vikings Finished" atingiu num_normal pela primeira vez
    # (esse e o momento exato em que a barreira e liberada)
    banquet_end_idx = next(
        (i for i, tag, val in timeline
         if tag == "finished_n" and val == num_normal),
        -1
    )

    # Fase 1 
    phase_open(1, "VIKINGS COMENDO (eat)", GR)
    eat_order = []
    for ln in lines:
        m = RE_EAT_S.search(ln)
        if m:
            print(f"  {GR}{ARR} Viking {int(m.group(1)):>3} comecou a comer{R}")
        m = RE_EAT_E.search(ln)
        if m:
            v = int(m.group(1)); eat_order.append(v)
            print(f"  {GR}{DONE} Viking {v:>3} log 'terminou de comer' [{len(eat_order)}/{num_normal}]"
                  f"{DM}  (plog tardio){R}")
    phase_close(GR)

    # Fase 2 
    phase_open(2, "BARREIRA -- evento real de sincronizacao", YL)
    ok_banquet = banquet_end_idx != -1
    if ok_banquet:
        print(f"  {YL}{WAIT} 'Vikings Finished: {num_normal}' na linha {banquet_end_idx + 1}")
        print(f"  {YL}{WAIT} Esse e o momento REAL em que chieftain_release_seat_plates")
        print(f"  {YL}      setou banquet_over=1 e liberou os waiters.")
        print(f"  {GR}{B}  {DONE} Barreira liberada — banquete encerrado!{R}")
    else:
        print(f"  {RD}{FAIL_S} 'Vikings Finished: {num_normal}' nunca encontrado!{R}")
    phase_close(YL)

    # Fase 3 
    phase_open(3, "VIKINGS REZANDO (pray)", BL)
    pray_started = []; pray_ended = []
    for ln in lines:
        m = RE_PRY_S.search(ln)
        if m:
            v = int(m.group(1)); pray_started.append(v)
            kind = "normal  " if v in normal_ids else "atrasado"
            print(f"  {BL}{PRAY} Viking {v:>3} ({kind}) comecou a rezar  [{len(pray_started)}]{R}")
        m = RE_PRY_E.search(ln)
        if m:
            v = int(m.group(1)); pray_ended.append(v)
            print(f"  {BL}{DONE} Viking {v:>3} terminou de rezar{R}")
    phase_close(BL)

    # Fase 4: verificacao 
    phase_open(4, "RESULTADO FINAL", MG)

    # Violadores: rezaram em linha anterior ao evento real de banquete
    bad_normal = [(i, v) for i, v in pray_starts
                  if v in normal_ids and i < banquet_end_idx]
    bad_late   = [(i, v) for i, v in pray_starts
                  if v in late_ids   and i < banquet_end_idx]

    ok_eating = (len(eat_order) == num_normal)
    ok_order  = (len(bad_normal) == 0 and len(bad_late) == 0)
    ok_prayed = (len(pray_ended) > 0)

    print(f"\n  {'VERIFICACAO':<40} {'RESULTADO'}")
    print(f"  {'-'*40} {'-'*20}")

    def chk(label, ok, detail=""):
        icon  = f"{GR}{OK_S}{R}"   if ok else f"{RD}{FAIL_S}{R}"
        state = f"{GR}OK{R}"       if ok else f"{RD}FALHA{R}"
        extra = f"  {DM}{detail}{R}" if detail else ""
        print(f"  {icon} {label:<38} {state}{extra}")

    chk(f"Todos os {num_normal} vikings normais comeram", ok_eating)
    chk("Nenhuma reza antes do evento real de banquete", ok_order,
        "" if ok_order else
        f"({len(bad_normal)} normal(is), {len(bad_late)} atrasado(s) violaram)")
    chk("Todos terminaram de rezar", ok_prayed)

    if bad_normal:
        print(f"\n  {RD}{WARN} Normais que rezaram antes de 'Finished:{num_normal}': "
              f"{[v for _,v in bad_normal]}{R}")
    if bad_late:
        print(f"\n  {YL}{WARN} Atrasados que rezaram antes de 'Finished:{num_normal}': "
              f"{[v for _,v in bad_late]}{R}")

    print()
    if ok_eating and ok_order and ok_prayed:
        print(f"  {GR}{B}{OK_S} ORDEM CORRETA: comer --> esperar --> rezar --> terminar{R}")
    else:
        print(f"  {RD}{B}{FAIL_S} ORDEM INCORRETA -- veja os {FAIL_S} acima{R}")
    phase_close(MG)

    # Output bruto filtrado 
    print(f"\n{DM}{'-'*65}")
    print("OUTPUT BRUTO (linhas relevantes):")
    print(f"  Legenda: {GR}verde=comer{R}{DM}, {BL}azul=rezar{R}{DM}, "
          f"{YL}amarelo=Finished/barreira{R}{DM}")
    print(f"{'-'*65}{R}")
    keys = ["eating", "praying", "Finished", "banquet"]
    for i, ln in enumerate(lines):
        if any(k.lower() in ln.lower() for k in keys):
            marker = f"{YL}>>> BARREIRA{R} " if i == banquet_end_idx else "    "
            c = GR if "eating" in ln else BL if "praying" in ln \
                else YL if "Finished" in ln else DM
            print(f"  {marker}{c}{ln}{R}")
    print(f"{DM}{'-'*65}{R}\n")


# Main
if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))

    ap = argparse.ArgumentParser(
        description="Debug de fases: comer -> esperar -> rezar -> terminar"
    )
    ap.add_argument("-v", type=int, default=6,   help="Vikings normais (default 6)")
    ap.add_argument("-c", type=int, default=4,   help="Cadeiras (default 4)")
    ap.add_argument("-e", type=int, default=300, help="Max comer ms (default 300)")
    ap.add_argument("-p", type=int, default=200, help="Max rezar ms (default 200)")
    ap.add_argument("--no-compile", action="store_true", help="Pula compilacao")
    args = ap.parse_args()

    if not args.no_compile:
        compile_debug(script_dir)

    raw = run_prog(script_dir, ["-v", str(args.v), "-c", str(args.c),
                                "-e", str(args.e), "-p", str(args.p)])

    m = RE_NVIKS.search(raw)
    num_normal = int(m.group(1)) if m else args.v

    analyse(raw, num_normal)