import csv
import os
import statistics
from collections import defaultdict
import matplotlib.pyplot as plt

RAW_CSV = os.path.join("data", "experiments.csv")


def load_data():
    grouped = defaultdict(list)
    with open(RAW_CSV, "r", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            grouped[(row["phase"], int(row["config_count"]))].append(row)
    return grouped


def main():
    os.makedirs("report", exist_ok=True)
    grouped = load_data()

    send_cfgs = [5, 20, 21]
    labels = ["5 сообщ.", "20 сообщ.", "21 сообщ.\n(переполн.)"]

    send_times = [statistics.mean(float(r["total_ms"]) for r in grouped[("send", c)]) for c in send_cfgs]
    recv_cfgs_map = {5: 5, 20: 20, 21: 20}  # при 21 попытке доставки в очереди сохраняется 20
    recv_times = [statistics.mean(float(r["total_ms"]) for r in grouped[("recv", recv_cfgs_map[c])]) for c in send_cfgs]

    # График 1: Время выполнения
    x = list(range(len(send_cfgs)))
    width = 0.35

    fig, ax = plt.subplots(figsize=(8, 5), dpi=150)
    b1 = ax.bar([i - width / 2 for i in x], send_times, width, label="Отправка в очередь (SEND)")
    b2 = ax.bar([i + width / 2 for i in x], recv_times, width, label="Получение и подтверждение (GET_PENDING + ACK)")

    ax.set_title("Зависимость среднего времени операций от числа сообщений (TCP)")
    ax.set_xlabel("Конфигурация эксперимента")
    ax.set_ylabel("Среднее время выполнения, мс")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.grid(axis="y", linestyle="--", alpha=0.5)
    ax.legend()

    for bar in list(b1) + list(b2):
        h = bar.get_height()
        ax.annotate(f"{h:.2f}", xy=(bar.get_x() + bar.get_width() / 2, h),
                    xytext=(0, 3), textcoords="offset points", ha="center", va="bottom", fontsize=8)

    plt.tight_layout()
    plt.savefig(os.path.join("report", "chart_time.png"))
    plt.close()

    # График 2: Объем данных и ошибки переполнения
    send_bytes = [
        statistics.mean(int(r["bytes_sent"]) + int(r["bytes_recv"]) for r in grouped[("send", c)])
        for c in send_cfgs
    ]
    recv_bytes = [
        statistics.mean(int(r["bytes_sent"]) + int(r["bytes_recv"]) for r in grouped[("recv", recv_cfgs_map[c])])
        for c in send_cfgs
    ]
    ok_ops = [statistics.mean(int(r["ok_ops"]) for r in grouped[("send", c)]) for c in send_cfgs]
    err_ops = [statistics.mean(int(r["err_ops"]) for r in grouped[("send", c)]) for c in send_cfgs]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.5), dpi=150)

    b_s = ax1.bar([i - width / 2 for i in x], send_bytes, width, label="Фаза SEND (байт)")
    b_r = ax1.bar([i + width / 2 for i in x], recv_bytes, width, label="Фаза GET_PENDING+ACK (байт)")
    ax1.set_title("Объём переданных и полученных данных")
    ax1.set_xlabel("Конфигурация эксперимента")
    ax1.set_ylabel("Объём данных, байт")
    ax1.set_xticks(x)
    ax1.set_xticklabels(labels)
    ax1.grid(axis="y", linestyle="--", alpha=0.5)
    ax1.legend(fontsize=8)
    for bar in list(b_s) + list(b_r):
        h = bar.get_height()
        ax1.annotate(f"{int(h)}", xy=(bar.get_x() + bar.get_width() / 2, h),
                     xytext=(0, 3), textcoords="offset points", ha="center", va="bottom", fontsize=7)

    b_ok = ax2.bar([i - width / 2 for i in x], ok_ops, width, label="Принято сервером (OK)", color="#2ca02c")
    b_err = ax2.bar([i + width / 2 for i in x], err_ops, width, label="Отклонено (QUEUE_FULL)", color="#d62728")
    ax2.set_title("Результат обработки и переполнение очереди")
    ax2.set_xlabel("Конфигурация эксперимента")
    ax2.set_ylabel("Количество сообщений, шт.")
    ax2.set_xticks(x)
    ax2.set_xticklabels(labels)
    ax2.grid(axis="y", linestyle="--", alpha=0.5)
    ax2.legend(fontsize=8)
    for bar in list(b_ok) + list(b_err):
        h = bar.get_height()
        ax2.annotate(f"{int(h)}", xy=(bar.get_x() + bar.get_width() / 2, h),
                     xytext=(0, 3), textcoords="offset points", ha="center", va="bottom", fontsize=8)

    plt.tight_layout()
    plt.savefig(os.path.join("report", "chart_metrics.png"))
    plt.close()

    print("Графики сохранены: report/chart_time.png и report/chart_metrics.png")


if __name__ == "__main__":
    main()