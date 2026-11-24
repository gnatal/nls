"""Tiny local HTTP server for tests/run_curl_exec.sh (stdlib only).

  /echo...        any method: replies with the request as plain text:
                    <METHOD> <path>
                    content-type: <value or ->
                    authorization: <value or ->
                    x-api-key: <value or ->
                    <body, or for multipart the uploaded file names>
  /status/<code>  replies with that status code
  /redirect       302 to /echo
  /files/<name>   serves "0123456789" * 10 (honours "Range: bytes=N-")
  /json           replies {"ok":true,"n":[1,2]}
  /slow           waits 3 seconds before replying

Prints the chosen port on stdout, then serves until killed.
"""
import http.server
import re
import sys
import time

PAYLOAD = b"0123456789" * 10


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, code, body=b"", ctype="text/plain", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def handle_any(self):
        path = self.path
        if path.startswith("/status/"):
            return self.reply(int(path.split("/")[2]), b"status\n")
        if path == "/redirect":
            return self.reply(302, b"", extra={"Location": "/echo"})
        if path == "/json":
            return self.reply(200, b'{"ok":true,"n":[1,2]}', "application/json")
        if path == "/slow":
            time.sleep(3)
            return self.reply(200, b"slow\n")
        if path.startswith("/files/"):
            rng = self.headers.get("Range", "")
            m = re.match(r"bytes=(\d+)-$", rng)
            if m:
                start = int(m.group(1))
                return self.reply(206, PAYLOAD[start:], "application/octet-stream", {
                    "Content-Range": "bytes %d-%d/%d" % (start, len(PAYLOAD) - 1, len(PAYLOAD)),
                    "Accept-Ranges": "bytes"})
            return self.reply(200, PAYLOAD, "application/octet-stream", {"Accept-Ranges": "bytes"})
        if path.startswith("/echo"):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n) if n else b""
            ctype = self.headers.get("Content-Type", "-")
            if ctype.startswith("multipart/form-data"):
                names = re.findall(rb'name="([^"]*)"; filename="([^"]*)"', body)
                body = b" ".join(k + b"=" + v for k, v in names)
                ctype = "multipart/form-data"
            lines = [
                "%s %s" % (self.command, path),
                "content-type: " + ctype,
                "authorization: " + self.headers.get("Authorization", "-"),
                "x-api-key: " + self.headers.get("X-Api-Key", "-"),
            ]
            return self.reply(200, ("\n".join(lines) + "\n").encode() + body + b"\n")
        return self.reply(404, b"not found\n")

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = handle_any


def main():
    srv = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    print(srv.server_address[1], flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
