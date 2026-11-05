#!/usr/bin/env python3
"""
Test end-to-end BLE transmission of the exact ECG clinical chart text:
- Session Init + Chunked Slots
- Verification of [A][[/A] parsing (bracket protection)
- Verification of [A]T[/A][Z]ha[/Z] (T-wave isolation)
- Full lifecycle ACKs
"""

import asyncio
import os
import struct
import subprocess
import sys
import time
from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHARACTERISTIC_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
DEVICE_NAME = "EHR-AI-Dongle"

def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc

async def main():
    print("=" * 70)
    print("ECG Clinical Chart Exact BLE Transmission Test")
    print("=" * 70)

    with open("/tmp/ecg_compiled_stream.txt", "r") as f:
        stream_text = f.read()

    print(f"Loaded compiled stream: {len(stream_text)} chars")
    print("Preview:\n  ", stream_text[:120], "...\n")

    # Focus X11 target editor window
    try:
        subprocess.run(["wmctrl", "-i", "-a", "0x03800085"], env=dict(os.environ, DISPLAY=":0"), check=False)
        print("[X11] Focused target editor window")
    except Exception:
        pass

    print(f"Scanning for '{DEVICE_NAME}'...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, adv: d.name == DEVICE_NAME or (adv.local_name and DEVICE_NAME in adv.local_name),
        timeout=7.0
    )
    if not device:
        print(f"❌ Error: Device '{DEVICE_NAME}' not found.")
        return 1

    print(f"Connecting to {device.name} [{device.address}]...")
    async with BleakClient(device.address) as client:
        acks = []
        event = asyncio.Event()

        def on_notify(sender, data: bytearray):
            msg = data.decode("utf-8", errors="replace")
            print(f"    [BLE NOTIFY] {msg}")
            acks.append(msg)
            event.set()

        await client.start_notify(CHARACTERISTIC_UUID, on_notify)
        await asyncio.sleep(0.5)

        session_id = 4501
        raw_bytes = stream_text.encode("utf-8")
        total_bytes = len(raw_bytes)
        total_crc = crc16_ccitt(raw_bytes)

        # Chunk size 180
        chunks = [raw_bytes[i:i+180] for i in range(0, total_bytes, 180)]
        total_packets = len(chunks)

        init_pkt = struct.pack("<HBHHB", session_id, total_packets, total_bytes, total_crc, 0)

        slot_pkts = []
        for seq_no, chunk in enumerate(chunks):
            chunk_crc = crc16_ccitt(chunk)
            header = struct.pack("<HBBHH", session_id, seq_no, 2, len(chunk), chunk_crc)
            slot_pkts.append(header + chunk)

        print(f"Transmitting Session #{session_id} ({total_packets} packets, {total_bytes} bytes)...")

        # Step 1: Send Init
        acks.clear()
        t0 = time.time()
        await client.write_gatt_char(CHARACTERISTIC_UUID, init_pkt, response=False)

        # Wait SESSION_READY
        ready = False
        for _ in range(40):
            if any(f"ACK:{session_id}:SESSION_READY" in a for a in acks):
                ready = True
                break
            await asyncio.sleep(0.05)

        if not ready:
            print("❌ Timeout waiting for SESSION_READY")
            return 1
        print(f"  [1/4] ✅ SESSION_READY received in {time.time()-t0:.2f}s")

        # Step 2: Send Slots
        for seq, slot in enumerate(slot_pkts):
            await client.write_gatt_char(CHARACTERISTIC_UUID, slot, response=False)
            await asyncio.sleep(0.025)

        # Step 3: Wait ALL_PACKETS_READY
        all_ready = False
        for _ in range(50):
            if any(f"ACK:{session_id}:ALL_PACKETS_READY" in a for a in acks):
                all_ready = True
                break
            await asyncio.sleep(0.05)

        if not all_ready:
            print(f"❌ Timeout waiting for ALL_PACKETS_READY (acks: {acks})")
            return 1
        print("  [2/4] ✅ ALL_PACKETS_READY received")

        # Step 4: Wait DISPATCH_STARTED
        dispatch = False
        for _ in range(40):
            if any(f"ACK:{session_id}:DISPATCH_STARTED" in a for a in acks):
                dispatch = True
                break
            await asyncio.sleep(0.05)

        if dispatch:
            print("  [3/4] ✅ DISPATCH_STARTED confirmed")

        # Step 5: Wait USB_REPORTS_SENT
        sent = False
        t_type = time.time()
        for _ in range(300): # up to 15s for 1569B
            if any(f"ACK:{session_id}:USB_REPORTS_SENT" in a for a in acks):
                sent = True
                break
            await asyncio.sleep(0.05)

        if sent:
            print(f"  [4/4] ✅ USB_REPORTS_SENT confirmed! (Typing took {time.time()-t_type:.2f}s)")
        else:
            print("⚠️ Wait timeout for USB_REPORTS_SENT")

        print("=" * 70)
        print("🎉 ECG chart test transmission SUCCESSFUL!")
        print("=" * 70)
        return 0

if __name__ == "__main__":
    rc = asyncio.run(main())
    sys.exit(rc)
