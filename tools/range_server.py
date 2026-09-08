# Minimal static HTTP server WITH Range support (python's http.server ignores
# Range, which makes every backward seek impossible and broke the FLAC seek
# tests). Usage: python3 range_server.py <port> <directory>
import os, sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

class RangeHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            return super().do_GET()
        size = os.path.getsize(path)
        rng = self.headers.get('Range')
        start, end = 0, size - 1
        status = 200
        if rng and rng.startswith('bytes='):
            a, _, b = rng[6:].partition('-')
            if a:
                start = int(a)
                if b:
                    end = min(int(b), size - 1)
            elif b:
                start = max(size - int(b), 0)
            if start > end or start >= size:
                self.send_response(416)
                self.send_header('Content-Range', 'bytes */%d' % size)
                self.end_headers()
                return
            status = 206
        length = end - start + 1
        self.send_response(status)
        self.send_header('Content-Type', self.guess_type(path))
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(length))
        if status == 206:
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        self.end_headers()
        with open(path, 'rb') as f:
            f.seek(start)
            left = length
            while left > 0:
                chunk = f.read(min(1 << 16, left))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                left -= len(chunk)

    def log_message(self, *a):
        pass

if __name__ == '__main__':
    port = int(sys.argv[1]); os.chdir(sys.argv[2])
    ThreadingHTTPServer(('127.0.0.1', port), RangeHandler).serve_forever()
