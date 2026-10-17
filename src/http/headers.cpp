#include "http/headers.h"

#include <algorithm>

namespace sluice::http {

namespace {

char ToLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string_view TrimWhitespace(std::string_view s) {
  const size_t first = s.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  const size_t last = s.find_last_not_of(" \t");
  return s.substr(first, last - first + 1);
}

}  // namespace

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return std::ranges::equal(
      a, b, [](char x, char y) { return ToLowerAscii(x) == ToLowerAscii(y); });
}

bool HasToken(std::string_view list, std::string_view token) {
  while (!list.empty()) {
    const size_t comma = list.find(',');
    if (EqualsIgnoreCase(TrimWhitespace(list.substr(0, comma)), token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    list.remove_prefix(comma + 1);
  }
  return false;
}

void Headers::Add(std::string_view name, std::string_view value) {
  fields_.push_back(
      Field{.name = std::string(name), .value = std::string(value)});
}

std::optional<std::string_view> Headers::Get(std::string_view name) const {
  for (const Field& field : fields_) {
    if (EqualsIgnoreCase(field.name, name)) {
      return field.value;
    }
  }
  return std::nullopt;
}

size_t Headers::Count(std::string_view name) const {
  return static_cast<size_t>(
      std::ranges::count_if(fields_, [name](const Field& field) {
        return EqualsIgnoreCase(field.name, name);
      }));
}

}  // namespace sluice::http
