#pragma once

#include <string>

// Inherits from std::string (matching the project's other ArduinoJson-facing String stubs)
// so ArduinoJson's specialized std::string deserializer overload applies directly, rather
// than its generic Stream-like Reader<T> template, which would otherwise require a read().
class String : public std::string {
 public:
  using std::string::string;

  String() = default;
  String(const std::string& value) : std::string(value) {}

  bool isEmpty() const { return empty(); }
};
