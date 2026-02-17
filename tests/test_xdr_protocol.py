import pytest
import socket
import subprocess
import time
import os
import signal


XDR_PORT = 7373
RTL_TCP_PORT = 1234


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('', 0))
        return s.getsockname()[1]


def send_xdr_command(command: str, port: int = XDR_PORT) -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    try:
        sock.connect(("localhost", port))
        
        response = sock.recv(1024).decode().strip()
        if '\n' in response:
            salt, _ = response.split('\n', 1)
        else:
            salt = response
        
        sock.send(("0" * 40 + "\n").encode())
        
        response = sock.recv(1024).decode().strip()
        
        sock.send((command + "\n").encode())
        response = sock.recv(1024).decode()
        return response.strip()
    finally:
        sock.close()


class TestXDRProtocol:
    
    @pytest.fixture(autouse=True)
    def setup_xdr_server(self):
        binary = os.path.join(os.path.dirname(os.path.dirname(__file__)), "build", "fm-tuner-sdr")
        if not os.path.exists(binary):
            pytest.skip("fm-tuner-sdr binary not found")
        
        rtl_port = find_free_port()
        
        self.fmtuner_proc = subprocess.Popen(
            [binary, "-t", f"localhost:{rtl_port}", "-f", "88600", "-s", "-g"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        
        time.sleep(2)
        
        if self.fmtuner_proc.poll() is not None:
            stdout, stderr = self.fmtuner_proc.communicate()
            pytest.skip(f"fm-tuner-sdr failed to start: {stderr.decode()}")
        
        yield
        
        self.fmtuner_proc.terminate()
        try:
            self.fmtuner_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.fmtuner_proc.kill()

    def test_authentication(self):
        response = send_xdr_command("P")
        assert response == "a2", f"Expected 'a2', got '{response}'"

    def test_status(self):
        response = send_xdr_command("S")
        assert response.startswith("F="), f"Expected status response, got '{response}'"
        assert "V=" in response
        assert "G=" in response
        assert "A=" in response

    def test_status_parsing(self):
        response = send_xdr_command("S")
        parts = response.split()
        status = {}
        for part in parts:
            if '=' in part:
                key, value = part.split('=', 1)
                status[key] = value
        
        assert "F" in status, "Frequency should be in status"
        assert "V" in status, "Volume should be in status"
        assert "G" in status, "Gain should be in status"
        assert "A" in status, "AGC should be in status"

    def test_tune_frequency(self):
        response = send_xdr_command("T99000000")
        assert response == "OK", f"Expected 'OK', got '{response}'"
        
        status = send_xdr_command("S")
        assert "F=99000000" in status, f"Frequency should be 99000000, got '{status}'"

    def test_tune_frequency_invalid(self):
        response = send_xdr_command("T")
        assert response == "ERR", f"Expected 'ERR' for empty frequency, got '{response}'"

    def test_volume(self):
        response = send_xdr_command("V50")
        assert response == "OK", f"Expected 'OK', got '{response}'"
        
        status = send_xdr_command("S")
        assert "V=50" in status, f"Volume should be 50, got '{status}'"

    def test_volume_clamping(self):
        response = send_xdr_command("V200")
        assert response == "OK", f"Expected 'OK', got '{response}'"
        
        status = send_xdr_command("S")
        assert "V=100" in status, f"Volume should be clamped to 100, got '{status}'"

    def test_volume_negative(self):
        response = send_xdr_command("V-10")
        assert response == "OK", f"Expected 'OK', got '{response}'"
        
        status = send_xdr_command("S")
        assert "V=0" in status, f"Volume should be clamped to 0, got '{status}'"

    def test_gain(self):
        response = send_xdr_command("G30")
        assert response == "OK", f"Expected 'OK', got '{response}'"
        
        status = send_xdr_command("S")
        assert "G=30" in status, f"Gain should be 30, got '{status}'"

    def test_agc_enable(self):
        response = send_xdr_command("A1")
        assert response == "OK", f"Expected 'OK', got '{response}'"
        
        status = send_xdr_command("S")
        assert "A=1" in status, f"AGC should be enabled, got '{status}'"

    def test_agc_disable(self):
        send_xdr_command("A1")
        response = send_xdr_command("A0")
        assert response == "OK", f"Expected 'OK', got '{response}'"
        
        status = send_xdr_command("S")
        assert "A=0" in status, f"AGC should be disabled, got '{status}'"

    def test_invalid_command(self):
        response = send_xdr_command("X")
        assert response == "ERR", f"Expected 'ERR' for invalid command, got '{response}'"

    def test_multiple_commands(self):
        send_xdr_command("T98800000")
        send_xdr_command("V75")
        send_xdr_command("G25")
        
        status = send_xdr_command("S")
        assert "F=98800000" in status
        assert "V=75" in status
        assert "G=25" in status
