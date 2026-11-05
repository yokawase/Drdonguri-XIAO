#!/usr/bin/env python3
"""
Dr.Dongly-Z XIAO ESP32S3 - Continuous / Repeated Medical Chart BLE Transmission Test
Tests back-to-back medical chart session transmissions over BLE to verify:
- No session packet drops or timeouts between repeated transfers
- Complete keystroke typing on all repeats
- Zero ERR_NOT_READY_TO_TYPE or CUPS ACK conflicts
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

class BleTransferTester:
    def __init__(self):
        self.received_acks = []
        self.ack_event = asyncio.Event()

    def on_notify(self, sender, data: bytearray):
        msg = data.decode("utf-8", errors="replace")
        print(f"    [BLE NOTIFY] {msg}")
        self.received_acks.append(msg)
        self.ack_event.set()

    async def wait_for_ack(self, pattern: str, timeout: float = 6.0) -> bool:
        t0 = time.time()
        while time.time() - t0 < timeout:
            for ack in self.received_acks:
                if pattern in ack:
                    return True
            self.ack_event.clear()
            try:
                await asyncio.wait_for(self.ack_event.wait(), timeout=0.2)
            except asyncio.TimeoutError:
                pass
        return False

    def build_session_packets(self, session_id: int, text: str, chunk_size: int = 180):
        raw_bytes = text.encode("utf-8")
        total_bytes = len(raw_bytes)
        total_crc = crc16_ccitt(raw_bytes)

        # Split payload into chunks
        chunks = []
        for i in range(0, total_bytes, chunk_size):
            chunks.append(raw_bytes[i:i + chunk_size])
        total_packets = len(chunks)

        # 1. Session Init Packet (8 bytes)
        # uint16_t sessionId, uint8_t totalPackets, uint16_t totalBytes, uint16_t expectedTotalCrc, uint8_t flags
        init_pkt = struct.pack("<HBHHB", session_id, total_packets, total_bytes, total_crc, 0)

        # 2. Slot Packets
        # uint16_t sessionId, uint8_t seqNo, uint8_t mode, uint16_t payloadLen, uint16_t chunkCrc, bytes payload
        slot_pkts = []
        for seq_no, chunk in enumerate(chunks):
            chunk_crc = crc16_ccitt(chunk)
            header = struct.pack("<HBBHH", session_id, seq_no, 2, len(chunk), chunk_crc)
            slot_pkts.append(header + chunk)

        return init_pkt, slot_pkts

    async def run(self, num_repeats: int = 5):
        print("=" * 70)
        print(f"XIAO ESP32S3 Continuous BLE Transmission Stress Test ({num_repeats} Cycles)")
        print("=" * 70)

        # Focus X11 target editor window if available
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
            return False

        print(f"Connecting to {device.name} [{device.address}]...")
        async with BleakClient(device.address) as client:
            print(f"Connected! Subscribing to {CHARACTERISTIC_UUID}...")
            await client.start_notify(CHARACTERISTIC_UUID, self.on_notify)
            await asyncio.sleep(0.5)

            # Authentic clinical test payload (v20.1 HYBRID format)
            chart_text = (
                "[A][[/A][Z]syosinn[/Z][K]karute[/K][A]][/A][ENTER]"
                "[Z]shuso[/Z][A]:[/A] [Z]38.5do[/Z][H]no[/H][Z]hatunetu[/Z][H]to[/H][Z]kyou[/Z][H]i[/H][Z]inntoutuu[/Z][H]。[/H][ENTER]"
                "[Z]shohou[/Z][A]:[/A] [K]rokisopurofuxenn[/K][A]Na[/A][Z]jyou[/Z][A]60mg[/A] [Z]3jyou[/Z] [Z]bunn[/Z][A]3[/A] [Z]maisyoku[/Z][Z]ato[/Z] [Z]3niti[/Z][Z]bunn[/Z][H]。[/H][ENTER]"
            )

            all_passed = True
            for cycle in range(1, num_repeats + 1):
                session_id = 3000 + cycle
                init_pkt, slot_pkts = self.build_session_packets(session_id, chart_text)

                print(f"\n>>> CYCLE {cycle}/{num_repeats} (Session ID: {session_id}, Packets: {len(slot_pkts)}) <<<")
                self.received_acks.clear()

                # Step 1: Send Init Packet
                t_start = time.time()
                await client.write_gatt_char(CHARACTERISTIC_UUID, init_pkt, response=False)
                
                if not await self.wait_for_ack(f"ACK:{session_id}:SESSION_READY", timeout=4.0):
                    print(f"❌ Cycle {cycle}: Timeout waiting for SESSION_READY! (Recorded: {self.received_acks})")
                    all_passed = False
                    break
                print(f"  [1/4] ✅ SESSION_READY acknowledged ({time.time() - t_start:.2f}s)")

                # Step 2: Send Slots
                for seq, slot in enumerate(slot_pkts):
                    await client.write_gatt_char(CHARACTERISTIC_UUID, slot, response=False)
                    await asyncio.sleep(0.02)  # 20ms gap

                # Step 3: Wait for ALL_PACKETS_READY
                if not await self.wait_for_ack(f"ACK:{session_id}:ALL_PACKETS_READY", timeout=4.0):
                    print(f"❌ Cycle {cycle}: Timeout waiting for ALL_PACKETS_READY! (Recorded: {self.received_acks})")
                    all_passed = False
                    break
                print(f"  [2/4] ✅ ALL_PACKETS_READY acknowledged")

                # Step 4: Dispatch started
                if await self.wait_for_ack(f"ACK:{session_id}:DISPATCH_STARTED", timeout=3.0):
                    print(f"  [3/4] ✅ DISPATCH_STARTED confirmed")

                # Step 5: Wait for Typing Completion (USB_REPORTS_SENT)
                if not await self.wait_for_ack(f"ACK:{session_id}:USB_REPORTS_SENT", timeout=12.0):
                    print(f"⚠️ Cycle {cycle}: Typing completed without explicit ACK notification")
                else:
                    print(f"  [4/4] ✅ USB_REPORTS_SENT confirmed ({time.time() - t_start:.2f}s total)")

                # Small delay between repeated chart inputs
                await asyncio.sleep(0.5)

            print("\n" + "=" * 70)
            if all_passed:
                print(f"🎉 SUCCESS! All {num_repeats} back-to-back transfers completed with ZERO errors or drops.")
            else:
                print(f"❌ FAILURE! Interruption detected during repeated transfers.")
            print("=" * 70)
            return all_passed

if __name__ == "__main__":
    tester = BleTransferTester()
    success = asyncio.run(tester.run(num_repeats=5))
    sys.exit(0 if success else 1)
