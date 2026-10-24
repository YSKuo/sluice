#!/usr/bin/env python3
"""Minimal HTTP backend used as an upstream target for sluice benchmarks.

GET responds with the backend's own name; POST echoes the request body
back (read via Content-Length), to exercise request-body forwarding.
Supports injecting latency (--delay-ms) to exercise backpressure, and
injecting errors (--fail-rate) to exercise upstream timeout and passive
down-marking behavior.
"""

import argparse
import random
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    name = "backend"
    delay_ms = 0
    fail_rate = 0.0

    def do_GET(self):
        self._respond(f"{self.name}\n".encode("utf-8"))

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self._respond(self.rfile.read(length))

    def _respond(self, body):
        if self.delay_ms > 0:
            time.sleep(self.delay_ms / 1000.0)

        if random.random() < self.fail_rate:
            body = b"injected failure\n"
            self.send_response(500)
        else:
            self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format_string, *args):
        # Keep benchmark output quiet; unhandled errors still raise normally.
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default="backend", help="Name returned in the response body")
    parser.add_argument("--port", type=int, default=9000, help="Port to listen on")
    parser.add_argument(
        "--delay-ms", type=int, default=0, help="Artificial delay per request, in milliseconds"
    )
    parser.add_argument(
        "--fail-rate", type=float, default=0.0, help="Fraction of requests to fail, between 0 and 1"
    )
    args = parser.parse_args()

    Handler.name = args.name
    Handler.delay_ms = args.delay_ms
    Handler.fail_rate = args.fail_rate

    # socketserver's default listen backlog is 5. Both gateways open a new
    # upstream connection per request, so bursts overflow such a backlog;
    # the dropped SYNs are retried after 1 s and show up as ~1 s tail
    # latency that has nothing to do with the gateway under test.
    ThreadingHTTPServer.request_queue_size = 1024
    ThreadingHTTPServer.daemon_threads = True
    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
