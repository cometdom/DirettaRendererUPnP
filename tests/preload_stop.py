"""Issue Stop after both the HTTP response header and the engine's EOF wait."""
import http.server
import pathlib
import subprocess
import sys
import threading
import time
import wave

with wave.open("tone.wav", "wb") as wav:
    wav.setparams((2, 2, 44100, 0, "NONE", ""))
    wav.writeframes(bytes(44100 * 4 * 2))
payload = pathlib.Path("tone.wav").read_bytes()

class HeaderOnly(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload[:44])
        self.wfile.flush()
        self.server.opened.set()
        if self.server.deliver:
            if self.server.release.wait(5):
                try:
                    self.wfile.write(payload[44:])
                except (BrokenPipeError, ConnectionResetError):
                    pass
            return
        self.connection.settimeout(10)
        try:
            self.connection.recv(1)
        except (TimeoutError, ConnectionError):
            pass

def trial(binary, delay, deliver):
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), HeaderOnly) as server:
        server.opened = threading.Event()
        server.release = threading.Event()
        server.deliver = deliver
        threading.Thread(target=server.serve_forever, kwargs={"poll_interval":0.02}, daemon=True).start()
        proc = subprocess.Popen([binary, f"http://127.0.0.1:{server.server_port}/next.wav"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        eof = threading.Event()
        lines = []
        def observe():
            for line in proc.stdout:
                lines.append(line.strip())
                if "EOF reached, waiting for background preload" in line:
                    eof.set()
        reader = threading.Thread(target=observe)
        reader.start()
        try:
            if not server.opened.wait(4) or not eof.wait(4):
                raise RuntimeError("fixture missed boundary: " + str(lines[-5:]))
            if deliver:
                server.release.set()
            time.sleep(delay)
            proc.stdin.write("release\n" if deliver else "stop\n")
            proc.stdin.flush()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                return "HUNG after confirmed boundary"
            reader.join()
            if proc.returncode:
                return f"failed {proc.returncode}: {lines[-5:]}"
            return next((s for s in lines if s.startswith("BOUNDARY_OK")), f"failed {proc.returncode}: {lines[-5:]}")
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            reader.join()
            server.shutdown()

passed = True
for i in range(int(sys.argv[2])):
    result = trial(sys.argv[1], (0, 0.005, 0.03)[i % 3], "--deliver" in sys.argv)
    print(result, flush=True)
    passed = passed and result.startswith("BOUNDARY_OK")
sys.exit(0 if passed else 1)
