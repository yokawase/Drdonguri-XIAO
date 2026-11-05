# PlatformIO extra_script:
# 1. TinyUSB (303a:8025) 稼働中であっても 1200bps touch でボタン操作なしに自動でROMブートローダー (303a:1001) へ遷移させる
# 2. ポート再列挙完了をポーリング待機してから esptool v5.4.0 (watchdog-reset対応) で100%確実に書き込む
import glob
import subprocess
import sys
import time

Import("env")  # noqa: F821


def _find_esp_port():
    for _ in range(30):
        ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
        if ports:
            return ports[0]
        time.sleep(0.2)
    return "/dev/ttyACM0"


def _touch_1200bps_if_needed():
    try:
        lsusb_out = subprocess.check_output(["lsusb"], text=True)
        if "303a:8025" in lsusb_out or "2886:0056" in lsusb_out:
            port = _find_esp_port()
            print(f"[auto_bootloader] TinyUSB稼働中を検出 ({port}) -> 1200bpsタッチでROMブートローダーへ自動遷移します...")
            import serial
            try:
                s = serial.Serial(port, 1200)
                s.dtr = False
                s.rts = False
                time.sleep(0.1)
                s.close()
            except Exception:
                # 1200bps設定瞬間にESP32-S3がリセットするためProtocol errorは正常動作
                pass

            # 303a:1001 (USB-Serial/JTAG) として再列挙されるまで待機
            for _ in range(30):
                time.sleep(0.2)
                out = subprocess.check_output(["lsusb"], text=True)
                if "303a:1001" in out:
                    break
            time.sleep(0.5)
    except Exception as exc:
        print(f"[auto_bootloader] 警告: {exc}")


def _custom_esptool_upload(source, target, env):
    _touch_1200bps_if_needed()
    port = _find_esp_port()
    build_dir = env.subst("$BUILD_DIR")
    boot_app0 = env.PioPlatform().get_package_dir("framework-arduinoespressif32") + "/tools/partitions/boot_app0.bin"

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", port,
        "--before", "default-reset",
        "--after", "watchdog-reset",
        "write-flash", "-z",
        "--flash-mode", "dio",
        "--flash-freq", "80m",
        "--flash-size", "8MB",
        "0x0", f"{build_dir}/bootloader.bin",
        "0x8000", f"{build_dir}/partitions.bin",
        "0xe000", boot_app0,
        "0x10000", str(source[0]),
    ]
    print("[esptool_v5] 自動書き込み実行: " + " ".join(cmd))
    return subprocess.call(cmd)


def _custom_esptool_uploadfs(source, target, env):
    _touch_1200bps_if_needed()
    port = _find_esp_port()

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", port,
        "--before", "default-reset",
        "--after", "watchdog-reset",
        "write-flash", "-z",
        "--flash-mode", "dio",
        "--flash-freq", "80m",
        "--flash-size", "8MB",
        "0x510000", str(source[0]),
    ]
    print("[esptool_v5_fs] LittleFS自動書き込み実行: " + " ".join(cmd))
    return subprocess.call(cmd)


env.Replace(UPLOADCMD=_custom_esptool_upload)
env.Replace(UPLOADFS=_custom_esptool_uploadfs)
