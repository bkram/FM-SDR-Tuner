import pytest
import subprocess
import socket
import time
import os
import signal
import tempfile
import shutil
from typing import Generator, Tuple
from contextlib import contextmanager


FMTUNER_BINARY = os.path.join(os.path.dirname(os.path.dirname(__file__)), "build", "fm-tuner-sdr")


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('', 0))
        return s.getsockname()[1]


@contextmanager
def rtl_tcp_server(frequency: int = 88600000, gain: int = 20, sample_rate: int = 1020000):
    port = find_free_port()
    
    env = os.environ.copy()
    rtl_tcp_path = shutil.which("rtl_tcp")
    
    if not rtl_tcp_path:
        pytest.skip("rtl_tcp not found in PATH")
    
    proc = subprocess.Popen(
        [rtl_tcp_path, "-p", str(port), "-f", str(frequency), "-g", str(gain), "-s", str(sample_rate)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    
    time.sleep(1)
    
    if proc.poll() is not None:
        stdout, stderr = proc.communicate()
        pytest.skip(f"rtl_tcp failed to start: {stderr.decode()}")
    
    try:
        yield ("localhost", port)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


@pytest.fixture
def rtl_tcp() -> Generator[Tuple[str, int], None, None]:
    with rtl_tcp_server() as addr:
        yield addr


@contextmanager
def fmtuner_process(rtl_host: str, rtl_port: int, frequency: int = 88600, wav_file: str = None):
    if not os.path.exists(FMTUNER_BINARY):
        pytest.skip(f"fm-tuner-sdr binary not found at {FMTUNER_BINARY}")
    
    cmd = [FMTUNER_BINARY, "-t", f"{rtl_host}:{rtl_port}", "-f", str(frequency)]
    
    if wav_file:
        cmd.extend(["-w", wav_file])
    else:
        cmd.append("-s")
    
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    
    time.sleep(2)
    
    if proc.poll() is not None:
        stdout, stderr = proc.communicate()
        pytest.skip(f"fm-tuner-sdr failed to start: {stderr.decode()}")
    
    try:
        yield proc
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


@pytest.fixture
def temp_wav_file() -> Generator[str, None, None]:
    fd, path = tempfile.mkstemp(suffix=".wav")
    os.close(fd)
    yield path
    if os.path.exists(path):
        os.remove(path)


@pytest.fixture
def fmtuner_with_rtl(rtl, temp_wav_file):
    host, port = rtl
    with fmtuner_process(host, port, wav_file=temp_wav_file) as proc:
        yield proc, temp_wav_file


def send_xdr_command(port: int = 7373, command: str = "") -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    try:
        sock.connect(("localhost", port))
        sock.send((command + "\n").encode())
        response = sock.recv(1024).decode()
        return response.strip()
    finally:
        sock.close()


@pytest.fixture
def xdr_client():
    return send_xdr_command
