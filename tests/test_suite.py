import socket
import struct
import subprocess
import time
import sys

HOST = "127.0.0.1"
PORT = 20016
SEED = 2552


def run_client(args):
    cmd = ["./client.exe", "--host", HOST, "--port", str(PORT)] + args
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
    return res.returncode, res.stdout, res.stderr


def send_raw_frame(sock, payload: str):
    data = payload.encode("utf-8")
    sock.sendall(struct.pack("!I", len(data)) + data)


def recv_raw_frame(sock) -> str:
    hdr = b""
    while len(hdr) < 4:
        chunk = sock.recv(4 - len(hdr))
        if not chunk:
            raise RuntimeError("Connection closed")
        hdr += chunk
    (length,) = struct.unpack("!I", hdr)
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            raise RuntimeError("Connection closed")
        body += chunk
    return body.decode("utf-8")


def main():
    srv = subprocess.Popen(
        ["./server.exe", "--host", HOST, "--port", str(PORT), "--timeout", "5000"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.5)

    try:
        # Тест 1: Минимум 3 одновременно активных клиента + онлайн-доставка
        print("[TEST 1] 3 concurrent clients + online delivery...")
        c2 = subprocess.Popen(
            ["./client.exe", "--host", HOST, "--port", str(PORT), "--id", "2", "--mode", "hold", "--duration", "2000"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        c3 = subprocess.Popen(
            ["./client.exe", "--host", HOST, "--port", str(PORT), "--id", "3", "--mode", "hold", "--duration", "2000"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        time.sleep(0.3)
        rc1, out1, _ = run_client(["--id", "1", "--target", "2", "--mode", "send", "--count", "3", "--seed", str(SEED)])
        out2, _ = c2.communicate(timeout=10)
        c3.communicate(timeout=10)
        assert rc1 == 0 and "ok=3 err=0" in out1, f"Test 1 failed: {out1}"
        assert out2.count("[MSG] from=1") == 3, f"Client 2 missed online msgs: {out2}"
        print("  PASS")

        # Тест 2: 5 сообщений отключенному пользователю, порядок FIFO, отсутствие повторной выдачи
        print("[TEST 2] 5 offline messages, FIFO order, no re-delivery...")
        rc, out, _ = run_client(["--id", "1", "--target", "4", "--mode", "send", "--count", "5", "--seed", str(SEED)])
        assert rc == 0 and "ok=5 err=0" in out, f"Send 5 failed: {out}"
        rc, out, _ = run_client(["--id", "4", "--mode", "recv"])
        assert rc == 0 and "delivered=5 order_ok=1 no_redelivery=1" in out, f"Recv 5 failed: {out}"
        print("  PASS")

        # Тест 3: 20 сообщений + проверка переполнения (21-е сообщение отклоняется)
        print("[TEST 3] 20 offline messages + overflow rejection on 21st...")
        rc, out, _ = run_client(["--id", "1", "--target", "5", "--mode", "send", "--count", "21", "--seed", str(SEED)])
        assert rc == 0 and "ok=20 err=1" in out and "QUEUE_FULL" in out, f"Overflow failed: {out}"
        rc, out, _ = run_client(["--id", "5", "--mode", "recv"])
        assert rc == 0 and "delivered=20 order_ok=1 no_redelivery=1" in out, f"Recv 20 failed: {out}"
        print("  PASS")

        # Тест 4: Проверка некорректных сообщений и неизвестных команд
        print("[TEST 4] Protocol validation (unknown command, bad format)...")
        with socket.create_connection((HOST, PORT), timeout=3) as s:
            send_raw_frame(s, "1|LOGIN|1|10|")
            resp = recv_raw_frame(s)
            assert "|OK|" in resp, f"Login failed: {resp}"

            send_raw_frame(s, "1|UNKNOWN_CMD|2|10|")
            resp = recv_raw_frame(s)
            assert "|ERROR|" in resp and "UNKNOWN_COMMAND" in resp, f"Expected UNKNOWN_COMMAND, got: {resp}"

            send_raw_frame(s, "bad_payload_without_delimiters")
            resp = recv_raw_frame(s)
            assert "|ERROR|" in resp and "BAD_FORMAT" in resp, f"Expected BAD_FORMAT, got: {resp}"

            send_raw_frame(s, "1|LOGOUT|3|10|")
            resp = recv_raw_frame(s)
            assert "|OK|" in resp
        print("  PASS")

        print("\nALL TESTS PASSED!")
    finally:
        srv.terminate()
        srv.wait(timeout=5)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as e:
        print(f"TEST FAILED: {e}", file=sys.stderr)
        sys.exit(1)