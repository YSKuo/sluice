// libFuzzer target for http::RequestParser.
//
// Splits the input at a point chosen by its first byte and feeds the halves
// the way a connection would (unconsumed bytes are passed again), with
// small limits so the limit paths are reached often. ASan/UBSan catch
// memory errors; the checks below catch logic errors the sanitizers
// cannot see.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "http/request_parser.h"

namespace {

void Check(bool condition) {
  if (!condition) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const size_t split = data[0] % size;
  const std::string_view input(reinterpret_cast<const char*>(data) + 1,
                               size - 1);

  constexpr sluice::http::ParserLimits kLimits{
      .max_request_line_bytes = 64,
      .max_header_bytes = 256,
      .max_header_count = 8,
      .max_body_bytes = 128,
  };
  using Status = sluice::http::RequestParser::Status;
  sluice::http::RequestParser parser(kLimits);
  std::string buffer;

  for (const std::string_view piece :
       {input.substr(0, split), input.substr(std::min(split, input.size()))}) {
    buffer.append(piece);
    Status status = Status::kComplete;
    while (status == Status::kComplete) {
      const sluice::http::RequestParser::FeedResult result =
          parser.Feed(buffer);
      Check(result.consumed <= buffer.size());
      buffer.erase(0, result.consumed);
      status = result.status;
      if (status == Status::kComplete) {
        const sluice::http::Request request = parser.TakeRequest();
        Check(!request.method.empty());
        Check(!request.target.empty() && request.target.front() == '/');
        Check(request.body.size() <= kLimits.max_body_bytes);
        Check(request.headers.size() <= kLimits.max_header_count);
      }
    }
    if (status == Status::kError) {
      return 0;
    }
    // Whatever stays buffered is one incomplete line, bounded by the limits.
    Check(buffer.size() <=
          std::max(kLimits.max_request_line_bytes, kLimits.max_header_bytes) +
              1);
  }
  return 0;
}
