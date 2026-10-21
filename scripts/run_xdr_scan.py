#!/usr/bin/env python3
"""Run an XDR band scan against a running fm-sdr-tuner and print the spectrum.

The scan is driven over the XDR wire protocol (port 7373) — there is no CLI
flag or ini key for it. This script speaks the reference client's handshake:
read the server's salt, answer with SHA1(salt + password), send "x" to start
the tuner, then arm and start the sweep:

    Sa<start_kHz>  Sb<stop_kHz>  Sc<step_kHz>  Sw<bandwidth_Hz>  S

Every "U<freq_kHz>=<level>,..." line the server publishes is printed as a
text bar chart. The server now publishes one line per retune, so the chart
fills in as the sweep runs.

Usage:
    scripts/run_xdr_scan.py                        # full band, 500 kHz step
    scripts/run_xdr_scan.py --start 100000 --stop 104000 --step 200
    scripts/run_xdr_scan.py --password secret123
"""

import argparse
import hashlib
import socket
import sys
import time


def connect(host, port, timeout):
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def send(sock, line):
    sock.sendall((line + "\n").encode())


def readline(sock, timeout=300):
    sock.settimeout(float(timeout))
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            raise EOFError("server closed the connection")
        buf += chunk
    return buf.decode(errors="replace").rstrip("\r\n")


def drain(sock, seconds=1.5):
    """Read and discard whatever the server sends until it goes quiet."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            readline(sock, timeout=0.5)
        except socket.timeout:
            break
        except EOFError:
            break


def authenticate(sock, password):
    salt = readline(sock)
    send(sock, hashlib.sha1((salt + password).encode()).hexdigest())
    reply = readline(sock)
    if reply == "a0":
        raise SystemExit("authentication failed (wrong password)")
    if reply == "a1":
        print("note: connected as a guest — the server refuses scans for "
              "guests, so the sweep will not run", file=sys.stderr)
    return reply


def parse_scan_line(line):
    """Parse "U87500=12.3,87600=0.0," into [(87500, 12.3), ...]."""
    entries = []
    for item in line[1:].split(","):
        if not item:
            continue
        freq, _, level = item.partition("=")
        entries.append((int(freq), float(level)))
    return entries


def render(entries, width=64):
    """A text bar chart of the spectrum, one row per channel."""
    if not entries:
        return
    peak = max(level for _, level in entries) or 1.0
    for freq, level in entries:
        filled = int(round((level / peak) * width))
        bar = "#" * filled
        print("  %7.1f kHz |%-*s| %5.1f" % (freq / 1000.0, width, bar, level))


def write_plot(entries, path, title):
    """Write the spectrum to a PNG. Needs matplotlib."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not available — install it with:\n"
              "    uv pip install matplotlib", file=sys.stderr)
        return False

    freqs = [f / 1000.0 for f, _ in entries]
    levels = [level for _, level in entries]

    fig, ax = plt.subplots(figsize=(12, 5))
    ax.plot(freqs, levels, linewidth=1.4, color="#1f6feb")
    ax.fill_between(freqs, levels, alpha=0.18, color="#1f6feb")
    ax.scatter(freqs, levels, s=16, color="#1f6feb", zorder=3)

    ax.set_xlabel("Frequency (MHz)")
    ax.set_ylabel("Level (0-120 dBf)")
    ax.set_title(title)
    ax.grid(True, alpha=0.3)
    ax.set_ylim(bottom=0)
    if freqs:
        ax.set_xlim(freqs[0], freqs[-1])
    fig.tight_layout()
    fig.savefig(path, dpi=140)
    plt.close(fig)
    print("wrote %s" % path)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7373)
    parser.add_argument("--password", default="",
                        help="XDR server password (default: empty, as configured)")
    parser.add_argument("--start", type=int, default=87500, help="start kHz")
    parser.add_argument("--stop", type=int, default=108000, help="stop kHz")
    parser.add_argument("--step", type=int, default=500, help="step kHz (min 5)")
    parser.add_argument("--bandwidth", type=int, default=56000,
                        help="measurement bandwidth in Hz")
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds to wait for the sweep")
    parser.add_argument("--plot", metavar="PNG",
                        help="write the spectrum to this PNG file (needs matplotlib)")
    args = parser.parse_args()

    sock = connect(args.host, args.port, args.timeout)
    try:
        reply = authenticate(sock, args.password)
        print("connected (auth reply %s)" % reply)
        drain(sock)

        send(sock, "x")
        time.sleep(0.8)
        send(sock, "Sa%d" % args.start)
        send(sock, "Sb%d" % args.stop)
        send(sock, "Sc%d" % args.step)
        send(sock, "Sw%d" % args.bandwidth)
        send(sock, "S")
        print("sweep armed: %d..%d kHz, step %d kHz, bw %d Hz"
              % (args.start, args.stop, args.step, args.bandwidth))
        print("(the sweep owns the radio — no audio, meter or RDS until it ends)")
        print()

        lines = []
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            try:
                line = readline(sock, timeout=max(1, int(deadline - time.time())))
            except socket.timeout:
                break
            except EOFError:
                break
            if not line.startswith("U"):
                continue
            lines.append(line)
            entries = parse_scan_line(line)
            measured = sum(1 for _, level in entries if level > 0)
            print("\rframe %d: %d/%d channels measured" %
                  (len(lines), measured, len(entries)), end="", flush=True)
        print()

        if not lines:
            print("no scan data received", file=sys.stderr)
            return 1

        final = parse_scan_line(lines[-1])
        print("\n%d frames, %d channels, %d..%d kHz"
              % (len(lines), len(final), final[0][0], final[-1][0]))
        print()
        render(final)
        if args.plot:
            write_plot(final, args.plot,
                       "FM band scan %d-%d kHz (step %d kHz, bw %d Hz)"
                       % (args.start, args.stop, args.step, args.bandwidth))
        return 0
    finally:
        try:
            send(sock, "X")
        except OSError:
            pass
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
