#!/bin/bash
# run_experiments.sh - Reproduces Task 5 scheduler experiments
# Run from boilerplate/ directory after running: make ci
#
# Usage: bash run_experiments.sh

set -e

BOILERPLATE_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$BOILERPLATE_DIR"

if [ ! -x "./cpu_hog" ] || [ ! -x "./io_pulse" ]; then
    echo "[error] Binaries not found. Run 'make ci' first."
    exit 1
fi

echo "============================================"
echo " Task 5: Scheduler Experiments"
echo "============================================"
echo ""

# ── Experiment 1: Two CPU-bound workloads at different nice values ──────────
echo "[exp1] Two cpu_hog processes: nice=0 vs nice=19 (running concurrently)"
echo "       Each runs for 10 seconds."
echo ""

nice -n 0  /usr/bin/time -v ./cpu_hog 10 > exp1_nice0.txt  2>&1 &
PID_HIGH=$!
nice -n 19 /usr/bin/time -v ./cpu_hog 10 > exp1_nice19.txt 2>&1 &
PID_LOW=$!

wait $PID_HIGH
wait $PID_LOW

echo "[exp1] Results:"
echo ""
echo "  --- nice=0 (high priority) ---"
grep -E "elapsed|Maximum resident|Percent" exp1_nice0.txt  || tail -5 exp1_nice0.txt
echo ""
echo "  --- nice=19 (low priority) ---"
grep -E "elapsed|Maximum resident|Percent" exp1_nice19.txt || tail -5 exp1_nice19.txt
echo ""

# ── Experiment 2: CPU-bound vs I/O-bound concurrently ───────────────────────
echo "[exp2] cpu_hog (CPU-bound) vs io_pulse (I/O-bound) at same priority"
echo "       cpu_hog runs 15s, io_pulse runs 30 iterations with 100ms sleep."
echo ""

nice -n 0 /usr/bin/time -v ./cpu_hog  15       > exp2_cpu.txt 2>&1 &
PID_CPU=$!
nice -n 0 /usr/bin/time -v ./io_pulse 30 100   > exp2_io.txt  2>&1 &
PID_IO=$!

wait $PID_CPU
wait $PID_IO

echo "[exp2] Results:"
echo ""
echo "  --- cpu_hog (CPU-bound) ---"
grep -E "elapsed|Maximum resident|Percent" exp2_cpu.txt  || tail -5 exp2_cpu.txt
echo ""
echo "  --- io_pulse (I/O-bound) ---"
grep -E "elapsed|Maximum resident|Percent" exp2_io.txt   || tail -5 exp2_io.txt
echo ""

echo "============================================"
echo " Done. Raw output files:"
echo "   exp1_nice0.txt   exp1_nice19.txt"
echo "   exp2_cpu.txt     exp2_io.txt"
echo "============================================"