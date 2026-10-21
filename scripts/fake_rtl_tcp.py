#!/usr/bin/env python3
"""A fake rtl_tcp server that plays a synthetic FM band.

It speaks the rtl_tcp protocol exactly as the reference server does
(tools/rtl_tcp.c of osmocom/rtl-sdr):

  * the server sends the 12-byte "RTL0" header and NOTHING else until the
    client starts the stream -- there are no per-command acknowledgements, so
    a client that expects them injects them into the IQ stream as garbage;
  * a dedicated thread reads the 5-byte binary commands while a second thread
    streams IQ, so a retune (0x01) that arrives mid-stream takes effect
    immediately.

The synthetic band holds one carrier on each of a set of ABSOLUTE
frequencies, plus noise. A carrier is only present in a read when it falls
inside the captured band, exactly like a real front end, so the tuner
discovers the stations by sweeping rather than being handed them.

It exists so the whole path -- the XDR handshake, the scan engine, the wire
format -- can be exercised without an SDR dongle.

Usage:
    scripts/fake_rtl_tcp.py --port 1234 --stations 96200000,101200000
"""

import argparse
import math
import random
import socket
import struct
import threading
import time

FS = 256000
SAMPLES_PER_READ = 16384

# rtl_tcp command codes (struct command { unsigned char cmd; unsigned int param; })
CMD_SET_FREQ = 0x01
CMD_SET_SAMPLE_RATE = 0x02
CMD_SET_GAIN_MODE = 0x03
CMD_SET_GAIN = 0x04
CMD_SET_FREQ_CORRECTION = 0x05
CMD_SET_AGC_MODE = 0x08


def build_iq(tune_hz, stations, samples, noise, rng, first_index, phase_epoch):
    """Build one block of IQ for a tuner parked at `tune_hz`.

    `first_index` is the absolute sample index of the first sample and
    `phase_epoch` the index at which the NCO was last restarted, so the
    carrier phase is continuous across reads and restarts on a retune.
    """
    out = bytearray()
    carriers = []
    for st in stations:
        offset = st - tune_hz
        # The front end only sees what is inside the captured band.
        if -FS / 2 < offset < FS / 2:
            carriers.append(offset)
    for i in range(samples):
        iv = rng.uniform(-noise, noise)
        qv = rng.uniform(-noise, noise)
        n = first_index + i - phase_epoch
        for offset in carriers:
            phase = 2.0 * math.pi * offset * (n / FS)
            iv += math.cos(phase) * 0.5
            qv += math.sin(phase) * 0.5
        out.append(max(0, min(255, round(iv * 127.5 + 127.5))))
        out.append(max(0, min(255, round(qv * 127.5 + 127.5))))
    return bytes(out)


class RtlTcpSession:
    """One connected client: a command reader thread and an IQ writer thread."""

    def __init__(self, conn, stations, noise, seed):
        self.conn = conn
        self.stations = stations
        self.noise = noise
        self.rng = random.Random(seed)
        self.sample_index = 0
        self.tune_hz = 100000000
        self.phase_epoch = 0
        self.lock = threading.Lock()
        self.running = True

    def current_tune(self):
        with self.lock:
            return self.tune_hz, self.sample_index, self.phase_epoch

    def set_tune(self, hz):
        with self.lock:
            self.tune_hz = hz
            # A real front end restarts its NCO when the centre frequency
            # changes, so the carrier phase starts again from zero. Keeping
            # the phase running across a retune would make the samples on
            # either side of the change discontinuous, which is not what
            # the spectrum of a real tuner looks like.
            self.phase_epoch = self.sample_index

    def read_commands(self):
        """Read the 5-byte commands. Never answers them: the reference
        server does not acknowledge commands either."""
        while self.running:
            header = self._recv_exactly(5)
            if header is None:
                break
            cmd = header[0]
            param = struct.unpack(">I", header[1:5])[0]
            if cmd == CMD_SET_FREQ:
                self.set_tune(param)
                print("set freq %d" % param, flush=True)
            elif cmd == CMD_SET_SAMPLE_RATE:
                print("set sample rate %d" % param, flush=True)
            elif cmd == CMD_SET_GAIN_MODE:
                print("set gain mode %d" % param, flush=True)
            elif cmd == CMD_SET_GAIN:
                print("set gain %d" % param, flush=True)
            elif cmd == CMD_SET_FREQ_CORRECTION:
                print("set freq correction %d" % param, flush=True)
            elif cmd == CMD_SET_AGC_MODE:
                print("set agc mode %d" % param, flush=True)

    def _recv_exactly(self, count):
        buf = b""
        while len(buf) < count:
            if not self.running:
                return None
            try:
                chunk = self.conn.recv(count - len(buf))
            except OSError:
                self.running = False
                return None
            if not chunk:
                self.running = False
                return None
            buf += chunk
        return buf

    def stream_iq(self):
        while self.running:
            tune, index, epoch = self.current_tune()
            payload = build_iq(
                tune, self.stations, SAMPLES_PER_READ, self.noise, self.rng,
                index, epoch,
            )
            with self.lock:
                self.sample_index += SAMPLES_PER_READ
            try:
                self.conn.sendall(payload)
            except OSError:
                self.running = False
                break
            # Pace the stream like the real server: the dongle produces the
            # samples at the sample rate, so a client that does not read
            # accumulates a bounded backlog rather than seconds of it. Without
            # the pacing this server runs several times faster than real
            # time and every read returns stale samples.
            time.sleep(SAMPLES_PER_READ / FS)

    def serve(self):
        # The greeting: "RTL0", tuner type, gain count. Nothing else is sent
        # before the IQ stream starts.
        self.conn.sendall(struct.pack(">4sII", b"RTL0", 5, 29))
        reader = threading.Thread(target=self.read_commands, daemon=True)
        reader.start()
        self.stream_iq()


def handle(conn, stations, noise, seed):
    session = RtlTcpSession(conn, stations, noise, seed)
    try:
        session.serve()
    except OSError:
        pass
    finally:
        session.running = False
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=1234)
    ap.add_argument("--stations", default="96200000,101200000")
    ap.add_argument("--noise", type=float, default=0.05)
    ap.add_argument("--seed", type=int, default=12345)
    args = ap.parse_args()

    stations = [int(s) for s in args.stations.split(",") if s.strip()]

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(4)
    print(
        "fake rtl_tcp listening on 127.0.0.1:%d with stations %s"
        % (args.port, stations),
        flush=True,
    )

    while True:
        conn, _ = srv.accept()
        threading.Thread(
            target=handle, args=(conn, stations, args.noise, args.seed), daemon=True
        ).start()


if __name__ == "__main__":
    main()
