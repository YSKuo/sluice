#!/usr/bin/env python3
"""Minimal HTTP backend used as an upstream target for sluice benchmarks.

Responds with its own name on every request. Supports injecting latency
(--delay-ms) to exercise backpressure, and injecting errors (--fail-rate)
to exercise upstream timeout and passive down-marking behavior.
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
        if self.delay_ms > 0:
            time.sleep(self.delay_ms / 1000.0)

        if random.random() < self.fail_rate:
            self.send_response(500)
            self.end_headers()
            self.wfile.write(b"injected failure\n")
            return

        body = f"{self.name}\n".encode("utf-8")
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

    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
