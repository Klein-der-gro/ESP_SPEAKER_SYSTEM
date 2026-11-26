#!/usr/bin/env python3
"""Register this PC as a test box and monitor hub audio packets."""

import argparse
import select
import socket
import struct
import time

MAGIC = 0x314B5053
ROLE = {"left": 0, "right": 1}
HELLO = struct.Struct("<IBBHI")
ACK = struct.Struct("<IB3x")
SYNC_REQUEST = struct.Struct("<IB3xQ")
AUDIO_HEADER = struct.Struct("<IBBHIIQQ")
AUDIO_PACKET_BYTES = 1056
AUDIO_TYPE = 1
HELLO_TYPE = 2
HELLO_ACK_TYPE = 3
SYNC_REQUEST_TYPE = 4
SYNC_RESPONSE_TYPE = 5


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hub", default="192.168.4.1",
                        help="ESP B access-point address")
    parser.add_argument("--role", choices=ROLE, default="left",
                        help="stereo channel to register for")
    args = parser.parse_args()

    control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    audio = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    control.bind(("", 5005))
    audio.bind(("", 5004))
    control.setblocking(False)
    audio.setblocking(False)
    hub_control = (args.hub, 5005)
    hub_audio = (args.hub, 5004)

    print("Listening for {} audio from {}. Connect this PC to SPEAKER-NET."
          .format(args.role.upper(), args.hub), flush=True)

    next_hello = 0.0
    next_sync = 0.0
    last_seq = None
    interval_packets = 0
    total_packets = 0
    gaps = 0
    invalid = 0
    last_rate = None
    window_start = time.monotonic()

    try:
        while True:
            now = time.monotonic()
            if now >= next_hello:
                control.sendto(HELLO.pack(MAGIC, HELLO_TYPE, ROLE[args.role],
                                          0, 2), hub_control)
                next_hello = now + 2.0
            if now >= next_sync:
                t1 = time.monotonic_ns() // 1000
                control.sendto(SYNC_REQUEST.pack(MAGIC, SYNC_REQUEST_TYPE, t1),
                               hub_control)
                next_sync = now + 1.0

            readable, _, _ = select.select([control, audio], [], [], 0.1)
            for sock in readable:
                data, _peer = sock.recvfrom(2048)
                if sock is control:
                    if len(data) >= ACK.size:
                        magic, packet_type = ACK.unpack_from(data)
                        if magic == MAGIC and packet_type == HELLO_ACK_TYPE:
                            print("Hub registration acknowledged", flush=True)
                        elif magic == MAGIC and packet_type == SYNC_RESPONSE_TYPE:
                            print("Clock sync response received", flush=True)
                elif len(data) == AUDIO_PACKET_BYTES:
                    magic, packet_type, _flags, payload_len, seq, rate, _frame, _time = (
                        AUDIO_HEADER.unpack_from(data)
                    )
                    if (magic != MAGIC or packet_type != AUDIO_TYPE
                            or payload_len != AUDIO_PACKET_BYTES - AUDIO_HEADER.size):
                        invalid += 1
                        continue
                    if last_seq is not None:
                        delta = (seq - last_seq) & 0xFFFFFFFF
                        if 1 < delta < 0x80000000:
                            gaps += delta - 1
                    last_seq = seq
                    last_rate = rate
                    interval_packets += 1
                    total_packets += 1
                else:
                    invalid += 1

            now = time.monotonic()
            if now - window_start >= 1.0:
                print("packets/s={} total={} missing={} invalid={} last_seq={} rate={}"
                      .format(interval_packets, total_packets, gaps, invalid,
                              last_seq if last_seq is not None else "-",
                              last_rate if last_rate is not None else "-"),
                      flush=True)
                interval_packets = 0
                window_start = now
    except KeyboardInterrupt:
        print("\nStopped.", flush=True)
    finally:
        control.close()
        audio.close()


if __name__ == "__main__":
    main()
