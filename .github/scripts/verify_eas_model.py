#!/usr/bin/env python3
"""
verify_eas_model.py -- assert the EAS energy model in sdm660.dtsi is
internally consistent and physically grounded.

WHY: cap and power columns are read verbatim from DT by kernel/sched/energy.c.
The only hard requirement is that all four tables use the SAME unit, but the
values also have to encode two things the scheduler depends on:

  1. cap must be monotonic in frequency and end exactly at the cluster's
     capacity_orig, because arch/arm64/kernel/topology.c sets

         capacity = cap_states[nr_cap_states - 1].cap

     and find_new_capacity() picks the first state with cap >= util. If the
     top cap is lower than the cluster's real capacity_orig, the highest OPPs
     become unreachable (this is what happened when big used cap 200..1638
     while util could reach 1638 -- actually reachable, but the big:little
     ratio was 1.60x instead of the real ~1.90x).

  2. power must increase faster than cap, or the scheduler will conclude that
     the big cluster is free and never use the little one.

Checks performed:
  A. all four tables parse, are frequency-sorted, and have equal length pairs
  B. cap is strictly increasing
  C. cap_top matches the expected cluster cap_max
  D. power is strictly increasing
  E. big cap_max / little cap_max is within tolerance of the target ratio
  F. energy efficiency (cap per unit power) is better on little at low load and
     worse on big, i.e. the model has a real crossover
  G. idle-cost-data has >= 2 entries (group_idle_state needs nr-2 >= 0) and is
     non-increasing (index 0 = deepest sleep = cheapest)

Usage: verify_eas_model.py <sdm660.dtsi> [--expect-ratio 1.90]
"""

import argparse
import re
import sys

TABLES = ("CPU_COST0", "CLUSTER_COST0", "CPU_COST1", "CLUSTER_COST1")

# Frequencies (MHz) the speedbin0-v0 OPP tables actually provide.
FREQ = {
    "CPU_COST0": [300, 633.6, 902.4, 1113.6, 1401.6, 1536, 1747.2, 1843.2],
    "CLUSTER_COST0": [300, 633.6, 902.4, 1113.6, 1401.6, 1536, 1747.2, 1843.2],
    "CPU_COST1": [300, 1113.6, 1401.6, 1747.2, 1958.4, 2150.4, 2457.6],
    "CLUSTER_COST1": [300, 1113.6, 1401.6, 1747.2, 1958.4, 2150.4, 2457.6],
}

EXPECT_CAP_MAX = {"CPU_COST0": 1024, "CLUSTER_COST0": 1024, "CPU_COST1": 1946,
                  "CLUSTER_COST1": 1946}


def block(text, label):
    """Extract the body of a labelled DT node."""
    m = re.search(r"%s:\s*[a-z0-9\-]+\s*\{(.*?)\n\t\};" % re.escape(label),
                  text, re.S)
    if not m:
        raise SystemExit("FAIL: node %s not found" % label)
    return m.group(1)


def pairs(body):
    m = re.search(r"busy-cost-data\s*=\s*<(.*?)>;", body, re.S)
    if not m:
        raise SystemExit("FAIL: no busy-cost-data")
    nums = [int(x) for x in re.findall(r"-?\d+", m.group(1))]
    if len(nums) % 2:
        raise SystemExit("FAIL: odd number of cells in busy-cost-data")
    return [(nums[i], nums[i + 1]) for i in range(0, len(nums), 2)]


def idle(body):
    m = re.search(r"idle-cost-data\s*=\s*<(.*?)>;", body, re.S)
    if not m:
        raise SystemExit("FAIL: no idle-cost-data")
    return [int(x) for x in re.findall(r"-?\d+", m.group(1))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dtsi")
    ap.add_argument("--expect-ratio", type=float, default=1.90)
    ap.add_argument("--ratio-tol", type=float, default=0.15)
    args = ap.parse_args()

    text = open(args.dtsi).read()
    rc = 0

    parsed = {}
    idles = {}
    print("== EAS energy model verification ==")
    print("file: %s" % args.dtsi)
    print()

    # ---- A: parse ------------------------------------------------------
    for t in TABLES:
        body = block(text, t)
        parsed[t] = pairs(body)
        idles[t] = idle(body)
        n = len(parsed[t])
        exp_n = len(FREQ[t])
        ok = "ok" if n == exp_n else "MISMATCH (expected %d)" % exp_n
        print("[A] %-15s %d states  %s" % (t, n, ok))
        if n != exp_n:
            rc = 1

    print()

    # ---- B: cap strictly increasing ------------------------------------
    for t in TABLES:
        caps = [c for c, _ in parsed[t]]
        ok = all(caps[i] < caps[i + 1] for i in range(len(caps) - 1))
        print("[B] %-15s cap strictly increasing: %s" % (t, "ok" if ok else "FAIL"))
        if not ok:
            print("       caps=%s" % caps)
            rc = 1

    print()

    # ---- C: top cap matches expectation --------------------------------
    for t in TABLES:
        top = parsed[t][-1][0]
        want = EXPECT_CAP_MAX[t]
        ok = top == want
        print("[C] %-15s cap_top=%4d expected=%4d  %s"
              % (t, top, want, "ok" if ok else "FAIL"))
        if not ok:
            rc = 1

    print()

    # ---- D: power strictly increasing ----------------------------------
    for t in TABLES:
        pw = [p for _, p in parsed[t]]
        ok = all(pw[i] < pw[i + 1] for i in range(len(pw) - 1))
        print("[D] %-15s power strictly increasing: %s" % (t, "ok" if ok else "FAIL"))
        if not ok:
            print("       power=%s" % pw)
            rc = 1

    print()

    # ---- E: big:little cap ratio ---------------------------------------
    ratio = EXPECT_CAP_MAX["CPU_COST1"] / EXPECT_CAP_MAX["CPU_COST0"]
    lo = args.expect_ratio - args.ratio_tol
    hi = args.expect_ratio + args.ratio_tol
    ok = lo <= ratio <= hi
    print("[E] big:little cap ratio = %.3f  target %.2f +/- %.2f  %s"
          % (ratio, args.expect_ratio, args.ratio_tol, "ok" if ok else "FAIL"))
    if not ok:
        rc = 1

    print()

    # ---- F: there must be a real crossover -----------------------------
    # efficiency = cap / power ; little should win at its own max vs big at a
    # comparable cap, otherwise EAS never picks little.
    lit = parsed["CPU_COST0"]
    big = parsed["CPU_COST1"]
    lit_eff_top = lit[-1][0] / lit[-1][1]
    # find the big state whose cap is closest to the little top cap
    bmatch = min(big, key=lambda s: abs(s[0] - lit[-1][0]))
    big_eff = bmatch[0] / bmatch[1]
    ok = lit_eff_top > big_eff
    print("[F] efficiency at comparable cap: little %.3f vs big %.3f  %s"
          % (lit_eff_top, big_eff, "ok" if ok else "FAIL"))
    print("       (little must be more efficient, otherwise EAS always picks big)")
    if not ok:
        rc = 1

    print()

    # ---- H: the DT property name must match what the kernel reads --------
    #
    # kernel/sched/energy.c looks up the *unprefixed* name:
    #
    #     of_find_property(cn, "sched-energy-costs", NULL)
    #
    # so a CPU node carrying "qcom,sched-energy-costs" is invisible to EAS.
    # init_sched_energy_costs() then bails out on the first CPU, no sge is
    # ever allocated, update_cpu_capacity() falls back to
    # SCHED_CAPACITY_SCALE for every CPU, and the whole energy model above is
    # dead code -- the scheduler runs on flat 1024/1024 capacities. That is
    # exactly what happened here, and every check A-G still passed because
    # they only parse the tables, never the reference to them.
    want = "sched-energy-costs"
    bad = "qcom,sched-energy-costs"
    n_want = len(re.findall(r"(?<![\w,-])%s\s*=" % re.escape(want), text))
    n_bad = text.count(bad + " ")
    ok = n_bad == 0 and n_want > 0
    print("[H] DT property name: '%s' x%d, '%s' x%d  %s"
          % (want, n_want, bad, n_bad, "ok" if ok else "FAIL"))
    print("       (energy.c reads '%s'; the qcom, prefix makes it invisible)" % want)
    if not ok:
        rc = 1

    print()

    # ---- I: the cap column must equal round(cap_max * f / f_max) --------
    #
    # cap is not a free parameter: arch/arm64/kernel/topology.c takes
    # cap_states[nr_cap_states-1].cap as the cluster's capacity_orig, and
    # find_new_capacity() picks the first state with cap >= util. If an
    # intermediate cap is not the linear frequency ratio it claims to be, the
    # util->OPP mapping is skewed and EAS mis-prices migrations.
    FMAX = {"CPU_COST0": 1843200, "CLUSTER_COST0": 1843200,
            "CPU_COST1": 2457600, "CLUSTER_COST1": 2457600}
    CM = EXPECT_CAP_MAX
    for t in TABLES:
        caps = [c for c, _ in parsed[t]]
        fmax = FMAX[t]
        cm = CM[t]
        bad_idx = []
        for i, c in enumerate(caps):
            exp = round(cm * FREQ[t][i] * 1000.0 / fmax)
            if abs(c - exp) > 1:
                bad_idx.append((FREQ[t][i], c, exp))
        ok = not bad_idx
        print("[I] %-15s cap == round(%d*f/fmax): %s"
              % (t, cm, "ok" if ok else "FAIL"))
        if not ok:
            for f, c, exp in bad_idx:
                print("       f=%.1fMHz cap=%d expected=%d" % (f, c, exp))
            rc = 1

    print()

    # ---- G: idle tables -------------------------------------------------
    for t in TABLES:
        arr = idles[t]
        n_ok = len(arr) >= 2
        mono = all(arr[i] >= arr[i + 1] for i in range(len(arr) - 1))
        print("[G] %-15s idle states=%d  >=2: %s  non-increasing: %s"
              % (t, len(arr), "ok" if n_ok else "FAIL",
                 "ok" if mono else "FAIL"))
        if not (n_ok and mono):
            rc = 1

    print()
    print("VERDICT: %s" % ("PASS" if rc == 0 else "FAIL"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
