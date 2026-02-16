import pytest
import wave
import struct
import os
import subprocess
import time
import socket
import numpy as np
import shutil


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('', 0))
        return s.getsockname()[1]


def find_rtl_tcp():
    return shutil.which("rtl_tcp")


class TestWAVOutput:
    
    @pytest.fixture(autouse=True)
    def setup_fmtuner(self, tmp_path):
        binary = os.path.join(os.path.dirname(os.path.dirname(__file__)), "build", "fmtuner-sdr")
        rtl_tcp_path = find_rtl_tcp()
        
        if not os.path.exists(binary):
            pytest.skip("fmtuner-sdr binary not found")
        
        if not rtl_tcp_path:
            pytest.skip("rtl_tcp not found in PATH")
        
        self.wav_file = str(tmp_path / "test_output.wav")
        rtl_port = find_free_port()
        
        self.rtl_proc = subprocess.Popen(
            [rtl_tcp_path, "-p", str(rtl_port), "-f", "88600000", "-g", "20", "-s", "1020000"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        
        time.sleep(2)
        
        if self.rtl_proc.poll() is not None:
            stdout, stderr = self.rtl_proc.communicate()
            pytest.skip(f"rtl_tcp failed to start: {stderr.decode()}")
        
        self.fmtuner_proc = subprocess.Popen(
            [binary, "-t", f"localhost:{rtl_port}", "-f", "88600", "-w", self.wav_file],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        
        time.sleep(3)
        
        if self.fmtuner_proc.poll() is not None:
            stdout, stderr = self.fmtuner_proc.communicate()
            pytest.skip(f"fmtuner-sdr failed to start: {stderr.decode()}")
        
        self.fmtuner_proc.terminate()
        try:
            self.fmtuner_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.fmtuner_proc.kill()
        
        self.rtl_proc.terminate()
        try:
            self.rtl_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.rtl_proc.kill()
        
        yield
    
    def teardown_method(self):
        if hasattr(self, 'rtl_proc') and self.rtl_proc.poll() is None:
            self.rtl_proc.terminate()
            self.rtl_proc.wait(timeout=5)
        try:
            self.fmtuner_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.fmtuner_proc.kill()
        
        yield
    
    def test_wav_file_exists(self):
        assert os.path.exists(self.wav_file), "WAV file should exist"
    
    def test_wav_readable(self):
        try:
            with wave.open(self.wav_file, 'rb') as w:
                pass
        except Exception as e:
            pytest.fail(f"Failed to open WAV file: {e}")
    
    def test_wav_format(self):
        with wave.open(self.wav_file, 'rb') as w:
            channels = w.getnchannels()
            sample_width = w.getsampwidth()
            frame_rate = w.getframerate()
            n_frames = w.getnframes()
            
            assert channels == 2, f"Expected stereo (2 channels), got {channels}"
            assert sample_width == 2, f"Expected 16-bit (2 bytes), got {sample_width}"
            assert frame_rate == 32000, f"Expected 32000 Hz, got {frame_rate}"
            assert n_frames > 0, "WAV file should have audio frames"
    
    def test_wav_not_silent(self):
        with wave.open(self.wav_file, 'rb') as w:
            n_frames = w.getnframes()
            frames = w.readframes(n_frames)
            
            assert len(frames) > 0, "WAV file should contain audio data"
            
            samples = np.frombuffer(frames, dtype=np.int16)
            
            nonzero = np.count_nonzero(samples)
            assert nonzero > 0, "WAV file should not be completely silent"
    
    def test_wav_amplitude(self):
        with wave.open(self.wav_file, 'rb') as w:
            frames = w.readframes(w.getnframes())
            samples = np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0
            
            rms = np.sqrt(np.mean(samples**2))
            max_val = np.max(np.abs(samples))
            
            assert rms > 0.001, f"RMS too low: {rms} (might be silent or very quiet)"
            assert max_val > 0.01, f"Max amplitude too low: {max_val}"
            assert max_val <= 1.0, f"Max amplitude too high: {max_val} (clipping)"
    
    def test_wav_stereo_balance(self):
        with wave.open(self.wav_file, 'rb') as w:
            frames = w.readframes(w.getnframes())
            samples = np.frombuffer(frames, dtype=np.int16)
            
            left = samples[0::2].astype(np.float32)
            right = samples[1::2].astype(np.float32)
            
            left_rms = np.sqrt(np.mean(left**2))
            right_rms = np.sqrt(np.mean(right**2))
            
            assert left_rms > 0, "Left channel should have audio"
            assert right_rms > 0, "Right channel should have audio"
            
            balance = abs(left_rms - right_rms) / max(left_rms, right_rms)
            assert balance < 0.5, f"Stereo channels unbalanced: {balance:.2%}"
    
    def test_wav_spectral(self):
        with wave.open(self.wav_file, 'rb') as w:
            frames = w.readframes(w.getnframes())
            samples = np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0
            
            mono = samples[0::2] + samples[1::2]
            
            fft = np.fft.rfft(mono[:8192])
            power = np.abs(fft)**2
            
            spectral_mean = np.mean(power)
            spectral_max = np.max(power)
            
            flatness = spectral_mean / spectral_max if spectral_max > 0 else 0
            
            assert flatness > 0.01, f"Very low spectral flatness: {flatness} (might be pure tone)"
            assert flatness < 0.99, f"Very high spectral flatness: {flatness} (might be noise)"
    
    def test_wav_duration(self):
        with wave.open(self.wav_file, 'rb') as w:
            duration = w.getnframes() / w.getframerate()
            
            assert duration > 0.5, f"WAV duration too short: {duration:.2f}s"
