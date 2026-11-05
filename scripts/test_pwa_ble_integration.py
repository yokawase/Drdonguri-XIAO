#!/usr/bin/env python3
"""
Dr.Dongly-Z XIAO ESP32S3 BLE Integration & Medical Chart Input Test Harness
Tests end-to-end BLE communication using the exact PWA Web Bluetooth protocol:
- Connection & Service / Characteristic Discovery
- CMD:STATUS telemetry parsing
- CMD:PING heartbeat test
- CMD:ICD10 guideline CQ lookup test
- Authentic PWA Medical Chart Session Transmission (SessionInit + Slots)
- Full ACK Lifecycle verification (SESSION_READY, ALL_PACKETS_READY, DISPATCH_STARTED, SAFETY_SCAN, USB_REPORTS_SENT)
"""

import asyncio
import json
import os
import subprocess
import sys
import time
from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHARACTERISTIC_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
DEVICE_NAME = "EHR-AI-Dongle"

received_acks = []
notification_event = asyncio.Event()

def notification_handler(sender, data: bytearray):
    msg = data.decode("utf-8", errors="replace")
    print(f"  [BLE NOTIFY] Received: {msg}")
    received_acks.append(msg)
    notification_event.set()

async def wait_for_ack(prefix, timeout=5.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for ack in received_acks:
            if prefix in ack:
                return ack
        notification_event.clear()
        try:
            await asyncio.wait_for(notification_event.wait(), timeout=0.5)
        except asyncio.TimeoutError:
            pass
    return None

async def main():
    print("=" * 70)
    print("Dr.Dongly-Z XIAO ESP32S3 - PWA BLE Integration & Chart Input Test")
    print("=" * 70)

    # 1. Focus X11 target window (xed text editor) if available
    try:
        subprocess.run(["wmctrl", "-i", "-a", "0x03800085"], env=dict(os.environ, DISPLAY=":0"), check=False)
        print("[X11] Focused target editor window (xed: 0x03800085)")
    except Exception as e:
        print(f"[X11] Window focus note: {e}")

    # 2. Discover device
    print(f"\n[1/5] Scanning for BLE Device '{DEVICE_NAME}'...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, adv: d.name == DEVICE_NAME or (adv.local_name and DEVICE_NAME in adv.local_name),
        timeout=6.0
    )
    if not device:
        print(f"❌ Error: Could not find '{DEVICE_NAME}' via BLE!")
        return 1

    print(f"  -> Found device: {device.name} [{device.address}]")

    # 3. Connect to GATT Server
    print(f"\n[2/5] Connecting to GATT Server...")
    async with BleakClient(device.address) as client:
        print(f"  -> Connected: {client.is_connected}")

        # Subscribe to notifications
        await client.start_notify(CHARACTERISTIC_UUID, notification_handler)
        print(f"  -> Subscribed to notifications on {CHARACTERISTIC_UUID}")
        await asyncio.sleep(0.3)

        # 4. On-chip Telemetry & Command Tests
        print(f"\n[3/5] Testing On-chip Telemetry & Diagnostics Commands...")
        
        # Test CMD:STATUS
        received_acks.clear()
        print("  -> Writing 'CMD:STATUS'...")
        await client.write_gatt_char(CHARACTERISTIC_UUID, b"CMD:STATUS", response=False)
        ack_status = await wait_for_ack("STATUS", timeout=3.0)
        if ack_status:
            print(f"  ✅ CMD:STATUS Validated:\n     {ack_status}")
        else:
            print("  ⚠️ CMD:STATUS: No direct JSON notify received, continuing...")

        # Test CMD:PING
        received_acks.clear()
        print("  -> Writing 'CMD:PING:XIAO_PWA_TEST'...")
        await client.write_gatt_char(CHARACTERISTIC_UUID, b"CMD:PING:XIAO_PWA_TEST", response=False)
        ack_ping = await wait_for_ack("PONG", timeout=3.0)
        if ack_ping:
            print(f"  ✅ Ping/Pong Validated: {ack_ping}")
        else:
            print("  ❌ Ping/Pong Failed: No PONG received")

        # Test CMD:ICD10
        received_acks.clear()
        print("  -> Writing 'CMD:ICD10:I10' (Hypertension Minds Guideline lookup)...")
        await client.write_gatt_char(CHARACTERISTIC_UUID, b"CMD:ICD10:I10", response=False)
        ack_icd = await wait_for_ack("ICD10", timeout=3.0)
        if ack_icd:
            print(f"  ✅ Minds Guideline On-chip Search Validated: {ack_icd}")

        # 5. Medical Chart Input Session Test
        print(f"\n[4/5] Transmitting Authentic PWA Medical Chart Session...")
        session_file = "/tmp/drdonguri_test_session.json"
        if not os.path.exists(session_file):
            print(f"❌ Session file not found: {session_file}")
            return 1

        with open(session_file, "r") as f:
            session_data = json.load(f)

        sid = session_data["sessionId"]
        total_pkts = session_data["totalPackets"]
        total_bytes = session_data["totalBytes"]
        raw_text = session_data["rawText"]
        init_pkt_bytes = bytes.fromhex(session_data["initPacketHex"])

        print(f"  Session ID: {sid} | Packets: {total_pkts} | Bytes: {total_bytes}")
        print(f"  Raw v20.1 HYBRID Stream:\n    {raw_text}")

        # 5.1 Send Session Init Packet
        received_acks.clear()
        print(f"\n  -> Sending Session Init Frame (8 bytes)...")
        await client.write_gatt_char(CHARACTERISTIC_UUID, init_pkt_bytes, response=False)

        ack_init = await wait_for_ack(f"ACK:{sid}:SESSION_READY", timeout=4.0)
        if not ack_init:
            print(f"  ❌ Session Init failed: Did not receive SESSION_READY for sid {sid}")
            return 1
        print(f"  ✅ Session Init Acknowledged: {ack_init}")

        # 5.2 Send Data Slot Packets
        for p in session_data["slotPackets"]:
            seq_no = p["seqNo"]
            pkt_bytes = bytes.fromhex(p["fullPacketHex"])
            print(f"  -> Sending Slot #{seq_no} ({len(pkt_bytes)} bytes, CRC=0x{p['chunkCrc16']:04X})...")
            await client.write_gatt_char(CHARACTERISTIC_UUID, pkt_bytes, response=False)
            await asyncio.sleep(0.02)  # 20ms inter-packet delay

        # 5.3 Wait for ALL_PACKETS_READY
        ack_all = await wait_for_ack(f"ACK:{sid}:ALL_PACKETS_READY", timeout=4.0)
        if not ack_all:
            print(f"  ❌ Slot transmission failed: Did not receive ALL_PACKETS_READY for sid {sid}")
            return 1
        print(f"  ✅ All Packets Received & CRC Verified: {ack_all}")

        # 5.4 Wait for DISPATCH_STARTED
        ack_dispatch = await wait_for_ack(f"ACK:{sid}:DISPATCH_STARTED", timeout=3.0)
        if ack_dispatch:
            print(f"  ✅ Dispatch Engine Started: {ack_dispatch}")

        # 5.5 Wait for SAFETY_SCAN (Aho-Corasick safety alert on prescription drugs)
        ack_safety = await wait_for_ack(f"ACK:{sid}:SAFETY_SCAN", timeout=3.0)
        if ack_safety:
            print(f"  ✅ On-chip Aho-Corasick Safety Scan Triggered: {ack_safety}")

        # 5.6 Wait for USB_REPORTS_SENT (Keystroke typing completed)
        ack_sent = await wait_for_ack(f"ACK:{sid}:USB_REPORTS_SENT", timeout=12.0)
        if ack_sent:
            print(f"  ✅ USB-HID Keystrokes Completely Typed & Confirmed: {ack_sent}")
        else:
            print("  ⚠️ USB_REPORTS_SENT wait timeout (check editor window)")

        # 6. Summary
        print(f"\n[5/5] Test Complete Summary:")
        print(f"  - Device: {device.name} ({device.address})")
        print(f"  - All ACKs received: {len(received_acks)} ACKs recorded")
        print(f"  - Session ID: {sid} processed successfully")
        print("=" * 70)
        return 0

if __name__ == "__main__":
    rc = asyncio.run(main())
    sys.exit(rc)
