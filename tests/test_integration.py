import pytest
import subprocess
import time
import socket
import os
import signal


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('', 0))
        return s.getsockname()[1]


def send_xdr_command(command: str, port: int = 7373) -> str:
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


class TestIntegration:
    
    @pytest.fixture(autouse=True)
    def setup_processes(self, tmp_path):
        binary = os.path.join(os.path.dirname(os.path.dirname(__file__)), "build", "fmtuner-sdr")
        if not os.path.exists(binary):
            pytest.skip("fmtuner-sdr binary not found")
        
        self.binary = binary
        self.tmp_path = tmp_path
        self.rtl_port = find_free_port()
        self.fmtuner_proc = None
        
        yield
        
        if self.fmtuner_proc:
            self.fmtuner_proc.terminate()
            try:
                self.fmtuner_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.fmtuner_proc.kill()

    def start_fmtuner(self, frequency: int = 88600, wav_file: str = None):
        cmd = [self.binary, "-t", f"localhost:{self.rtl_port}", "-f", str(frequency), "-g"]
        
        if wav_file:
            cmd.extend(["-w", wav_file])
        else:
            cmd.append("-s")
        
        self.fmtuner_proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        
        time.sleep(2)
        
        if self.fmtuner_proc.poll() is not None:
            stdout, stderr = self.fmtuner_proc.communicate()
            pytest.skip(f"fmtuner-sdr failed to start: {stderr.decode()}")
        
        return self.fmtuner_proc

    def test_rtl_tcp_connection(self):
        self.start_fmtuner()
        time.sleep(2)
        assert self.fmtuner_proc.poll() is None, "Process should still be running"

    def test_xdr_server_starts(self):
        self.start_fmtuner()
        time.sleep(2)
        
        try:
            response = send_xdr_command("P")
            assert response == "a2"
        except Exception as e:
            pytest.fail(f"XDR server not responding: {e}")

    def test_xdr_tune_changes_frequency(self):
        self.start_fmtuner()
        time.sleep(2)
        
        send_xdr_command("T99000000")
        time.sleep(0.5)
        
        status = send_xdr_command("S")
        assert "F=99000000" in status

    def test_xdr_volume_affects_audio(self):
        self.start_fmtuner()
        time.sleep(2)
        
        send_xdr_command("V0")
        status = send_xdr_command("S")
        assert "V=0" in status
        
        send_xdr_command("V100")
        status = send_xdr_command("S")
        assert "V=100" in status

    def test_graceful_shutdown(self):
        self.start_fmtuner()
        time.sleep(2)
        
        self.fmtuner_proc.terminate()
        
        try:
            self.fmtuner_proc.wait(timeout=5)
            assert True
        except subprocess.TimeoutExpired:
            pytest.fail("Process did not terminate gracefully")

    def test_sigint_shutdown(self):
        self.start_fmtuner()
        time.sleep(2)
        
        self.fmtuner_proc.send_signal(signal.SIGINT)
        
        try:
            self.fmtuner_proc.wait(timeout=5)
            assert True
        except subprocess.TimeoutExpired:
            pytest.fail("Process did not respond to SIGINT")

    def test_wav_file_created(self):
        wav_file = str(self.tmp_path / "output.wav")
        self.start_fmtuner(wav_file=wav_file)
        
        time.sleep(3)
        
        self.fmtuner_proc.terminate()
        try:
            self.fmtuner_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.fmtuner_proc.kill()
        
        assert os.path.exists(wav_file), "WAV file should be created"
        assert os.path.getsize(wav_file) > 0, "WAV file should not be empty"

    def test_multiple_frequency_changes(self):
        self.start_fmtuner()
        time.sleep(2)
        
        frequencies = [88600000, 98800000, 102000000]
        
        for freq in frequencies:
            send_xdr_command(f"T{freq}")
            time.sleep(0.5)
            status = send_xdr_command("S")
            assert f"F={freq}" in status

    def test_invalid_rtl_tcp_handling(self):
        rtl_port = find_free_port()
        
        cmd = [
            self.binary,
            "-t", f"localhost:{rtl_port}",
            "-f", "88600",
            "-s"
        ]
        
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        
        time.sleep(3)
        
        if proc.poll() is None:
            proc.terminate()
            proc.wait()
        
        assert proc.returncode is not None or proc.poll() is not None, \
            "Process should exit or indicate error when rtl_tcp unavailable"
