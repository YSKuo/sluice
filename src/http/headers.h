#ifndef SLUICE_HTTP_HEADERS_H_
#define SLUICE_HTTP_HEADERS_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sluice::http {

// Compares two ASCII strings ignoring case, as HTTP field names require.
bool EqualsIgnoreCase(std::string_view a, std::string_view b);

// Whether a comma-separated field value (e.g. "keep-alive, Upgrade")
// contains `token`, ignoring case and surrounding whitespace.
bool HasToken(std::string_view list, std::string_view token);

// The header fields of a request or response, in the order received.
// Names are matched case-insensitively and may repeat. Lookups are linear:
// the parser caps the field count, so the list is always short.
class Headers {
 public:
  struct Field {
    std::string name;
    std::string value;
  };

  void Add(std::string_view name, std::string_view value);

  // The value of the first field named `name`, or nullopt if there is none.
  [[nodiscard]] std::optional<std::string_view> Get(
      std::string_view name) const;
  // How many fields are named `name`.
  [[nodiscard]] size_t Count(std::string_view name) const;

  [[nodiscard]] const std::vector<Field>& fields() const noexcept {
    return fields_;
  }
  [[nodiscard]] size_t size() const noexcept { return fields_.size(); }

 private:
  std::vector<Field> fields_;
};

}  // namespace sluice::http

#endif  // SLUICE_HTTP_HEADERS_H_
