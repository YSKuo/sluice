# ADR 0003: HTTP/1.1 request parser and keep-alive

## Status

Accepted

## Context

Week 2 turns the echo server into an HTTP/1.1 server. The parser is the
gateway's first security boundary: everything it accepts will be forwarded
upstream in Week 3, so any input the gateway and an upstream interpret
differently is a request-smuggling vector, and any input that makes the
parser buffer without bound is a memory-exhaustion attack. Bytes arrive in
arbitrary pieces, so the parser must be incremental.

## Options

- **Storage:** copy parsed strings into an owned `Request` vs keep
  `std::string_view`s into the connection's input buffer.
- **Strictness:** accept common deviations (bare LF line endings, obs-fold,
  whitespace before the colon, `Transfer-Encoding`) vs reject them.
- **Incomplete lines:** copy partial lines into parser-owned storage vs
  leave them in the connection buffer and rescan when more bytes arrive.
- **Per-connection state:** a `connection id -> RequestParser` map in
  `HttpServer` vs a generic context slot on `TcpConnection`.
- **Pipelining:** answer every buffered request immediately vs one response
  in flight at a time.

## Decision

- **Owned strings.** `Request` copies method, target, headers, and body.
  Views would dangle as soon as `Buffer` compacts or grows; the copy costs a
  few hundred bytes per request.
- **Strict parsing (RFC 9112).** Lines must end in CRLF; bare CR or bare LF
  is 400. Whitespace before a field name's colon and obs-fold are 400.
  Only origin-form targets are accepted. HTTP/1.1 requires exactly one
  `Host`. Bodies are framed only by `Content-Length`, which must be plain
  digits; conflicting values are 400. Any `Transfer-Encoding` is 501, which
  also rules out the TE + CL smuggling combination.
- **Limits checked before a line completes.** Request line 8 KiB (414),
  header section 16 KiB (431), 100 header fields (431), body 1 MiB (413).
  An incomplete line is rejected as soon as it already exceeds its limit.
- **Leave incomplete lines in the buffer.** `Feed()` consumes only complete
  lines and returns how many bytes it used; the caller passes the rest again.
  Rescanning is bounded by the limits, and the parser needs no line buffer.
- **Parser map in `HttpServer`.** No change to the net layer's ownership
  model; closed connections hand their parser to `DeleteLater`, because the
  close usually happens while `ProcessInput()` still uses it.
- **One response in flight.** While a response is still being flushed,
  further pipelined requests stay in the input buffer and reading pauses.
  This is the request-level form of Week 1's backpressure.
- **Error responses close the connection.** After a framing error the
  parser cannot know where the next request starts.

## Trade-offs

- Strictness may reject some real but sloppy clients (e.g. bare-LF test
  scripts). This is deliberate for a gateway.
- Limits bound memory, not time: a client sending one byte per second
  (slowloris) is only stopped by the Week 3 idle and header timeouts.
- One response in flight serializes pipelined requests; with a synchronous
  handler this costs nothing, and Week 3's asynchronous proxy needs strict
  ordering anyway.
- The `Handler` is synchronous. Week 3 must change it so a response can be
  produced later (after the upstream answers).

## References

- RFC 9112 (HTTP/1.1 message syntax), sections 2.2, 3, 5, 6.3, 9.3
- RFC 9110 (HTTP semantics), sections 5.5, 5.6.2, 8.6
- `src/http/request_parser.{h,cpp}`, `src/http/http_server.{h,cpp}`,
  `tests/http_request_parser_test.cpp`, `fuzz/request_parser_fuzzer.cpp`
