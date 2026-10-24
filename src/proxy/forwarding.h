#ifndef SLUICE_PROXY_FORWARDING_H_
#define SLUICE_PROXY_FORWARDING_H_

#include <string>
#include <string_view>

#include "http/headers.h"
#include "http/request.h"
#include "http/response_parser.h"
#include "net/inet_address.h"

// How a request and a response are rewritten as they pass through the
// gateway (RFC 9110 section 7.6).
namespace sluice::proxy {

// Whether `name` is a hop-by-hop field: one that describes a single
// connection and so must not be forwarded to the next hop. That is the
// fixed list (Connection, Keep-Alive, Proxy-Connection, TE, Trailer,
// Transfer-Encoding, Upgrade) plus every field the message's own
// Connection header names (e.g. "Connection: close, X-Secret").
bool IsHopByHop(std::string_view name, const http::Headers& headers);

// Whether the path of `target` contains a "." or ".." segment. Such
// requests are rejected (400) instead of forwarded: the gateway routes on
// the raw path, and an upstream that normalizes "/public/../admin" would
// otherwise serve a route the gateway never checked.
bool HasDotSegment(std::string_view target);

// The bytes sent to the upstream for `request`:
// - HTTP/1.1 request line with the original method and target;
// - the client's fields minus hop-by-hop ones, Content-Length (re-added
//   from the actual body), and Expect (the body is already buffered, so
//   "100-continue" has nothing to wait for);
// - Host from the client, or `upstream_host` if an HTTP/1.0 client sent none;
// - X-Forwarded-For with the client's address appended;
// - "Connection: close": every request uses a fresh upstream connection
//   (connection pooling is out of scope), which also delimits the reply.
std::string BuildUpstreamRequest(const http::Request& request,
                                 const net::InetAddress& client,
                                 std::string_view upstream_host);

// The head sent to the client for an upstream's `head`: always HTTP/1.1,
// the upstream's status and reason, its fields minus hop-by-hop ones, its
// Content-Length (if any, once), and our own Connection field.
std::string BuildClientResponseHead(const http::ResponseHead& head,
                                    bool keep_alive);

}  // namespace sluice::proxy

#endif  // SLUICE_PROXY_FORWARDING_H_
