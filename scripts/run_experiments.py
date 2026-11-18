import csv
import os
import statistics
import subprocess
import time
from collections import defaultdict

HOST = "127.0.0.1"
PORT = 20016
SEED = 2552
CONFIGS = [5, 20, 21]  # 5, 20 и 21 (проверка переполнения > 20)
RUNS = 3

RAW_CSV = os.path.join("data", "experiments.csv")
SUMMARY_CSV = os.path.join("data", "summary.csv")


def run_client(args):
    cmd = ["./client.exe", "--host", HOST, "--port", str(PORT), "--seed", str(SEED)] + args
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
    if res.returncode != 0:
        raise RuntimeError(f"Client failed: {' '.join(cmd)}\n{res.stdout}\n{res.stderr}")
    return res.stdout


def main():
    os.makedirs("data", exist_ok=True)
    if os.path.exists(RAW_CSV):
        os.remove(RAW_CSV)

    srv = subprocess.Popen(
        ["./server.exe", "--host", HOST, "--port", str(PORT), "--timeout", "5000"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(0.5)

    try:
        for cfg in CONFIGS:
            for run_id in range(1, RUNS + 1):
                target_id = 100 + cfg * 10 + run_id
                print(f"Running config={cfg}, run={run_id} (target_id={target_id})...")
                run_client([
                    "--id", "1",
                    "--target", str(target_id),
                    "--mode", "send",
                    "--count", str(cfg),
                    "--run", str(run_id),
                    "--csv", RAW_CSV,
                ])
                run_client([
                    "--id", str(target_id),
                    "--mode", "recv",
                    "--run", str(run_id),
                    "--csv", RAW_CSV,
                ])
    finally:
        srv.terminate()
        srv.wait(timeout=5)

    grouped = defaultdict(list)
    with open(RAW_CSV, "r", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            key = (row["phase"], int(row["config_count"]))
            grouped[key].append(row)

    with open(SUMMARY_CSV, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "phase", "config_count", "run1_ms", "run2_ms", "run3_ms", "avg_total_ms",
            "avg_connect_ms", "avg_rtt_ms", "avg_srv_ms",
            "avg_ok_ops", "avg_err_ops", "avg_bytes_total", "avg_throughput_msg_s"
        ])
        print("\n=== СВОДНАЯ ТАБЛИЦА ЭКСПЕРИМЕНТОВ ===")
        for (phase, cfg), rows in sorted(grouped.items()):
            totals = [float(r["total_ms"]) for r in rows]
            conns = [float(r["connect_ms"]) for r in rows]
            rtts = [float(r["avg_rtt_ms"]) for r in rows]
            srvs = [float(r["avg_srv_ms"]) for r in rows]
            oks = [int(r["ok_ops"]) for r in rows]
            errs = [int(r["err_ops"]) for r in rows]
            bytes_tot = [int(r["bytes_sent"]) + int(r["bytes_recv"]) for r in rows]
            tputs = [float(r["throughput_msg_s"]) for r in rows]

            summary_row = [
                phase, cfg,
                f"{totals[0]:.3f}", f"{totals[1]:.3f}", f"{totals[2]:.3f}",
                f"{statistics.mean(totals):.3f}",
                f"{statistics.mean(conns):.3f}",
                f"{statistics.mean(rtts):.3f}",
                f"{statistics.mean(srvs):.3f}",
                f"{statistics.mean(oks):.1f}",
                f"{statistics.mean(errs):.1f}",
                f"{statistics.mean(bytes_tot):.0f}",
                f"{statistics.mean(tputs):.1f}",
            ]
            writer.writerow(summary_row)
            print(
                f"phase={phase:4s} cfg={cfg:2d} | "
                f"runs(ms)=[{totals[0]:.3f}, {totals[1]:.3f}, {totals[2]:.3f}] "
                f"avg={statistics.mean(totals):.3f} ms | "
                f"ok={statistics.mean(oks):.0f} err={statistics.mean(errs):.0f} | "
                f"bytes={statistics.mean(bytes_tot):.0f}"
            )

    print(f"\nСохранено в {RAW_CSV} и {SUMMARY_CSV}")


if __name__ == "__main__":
    main()