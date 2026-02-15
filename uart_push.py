import sys
import time
import serial

def read_lines_nonblock(ser, seconds=0.2):
    """Read any available lines for up to `seconds`."""
    end = time.time() + seconds
    out = []
    while time.time() < end:
        line = ser.readline()
        if line:
            out.append(line.decode("utf-8", "replace").rstrip())
        else:
            time.sleep(0.01)
    return out

def parse_config(path):
    items = []
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            k = k.strip()
            v = v.strip()
            if not k:
                continue
            items.append((k, v))
    return items

def main():
    if len(sys.argv) < 3:
        print("Kullanim: python uart_push.py COM3 config.txt")
        sys.exit(1)

    port = sys.argv[1]
    cfg_path = sys.argv[2]

    items = parse_config(cfg_path)
    if not items:
        print("HATA: config bos veya okunamadi.")
        sys.exit(2)

    print(f"[INFO] Port: {port} | Dosya: {cfg_path} | Satir: {len(items)}")
    print("[INFO] Port aciliyor (115200)...")

    ser = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(0.3)
    ser.reset_input_buffer()

    # Boot log varsa yakala
    boot = read_lines_nonblock(ser, seconds=0.8)
    if boot:
        print("[ESP] (boot/log)")
        for ln in boot[-12:]:
            print("  " + ln)

    def send(cmd):
        ser.write((cmd + "\n").encode("utf-8"))
        ser.flush()
        # Kısa süre cevap oku
        resp = read_lines_nonblock(ser, seconds=0.5)
        return resp

    print("[INFO] BEGIN")
    for ln in send("BEGIN"):
        print("[ESP] " + ln)

    ok = 0
    err = 0

    for k, v in items:
        cmd = f"SET {k} {v}"
        print(f"[SEND] {cmd}")
        resp = send(cmd)
        if not resp:
            print("[WARN] cevap yok")
            continue
        for ln in resp:
            print("[ESP] " + ln)
            if ln.startswith("OK "):
                ok += 1
            if ln.startswith("ERR "):
                err += 1

    print("[INFO] COMMIT")
    resp = send("COMMIT")
    for ln in resp:
        print("[ESP] " + ln)

    print(f"[DONE] OK={ok} ERR={err}")
    ser.close()

if __name__ == "__main__":
    main()
