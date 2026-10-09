#include "param_json.h"

#include "disc_serial.h"

#include <cstring>
#include <exception>
#include <string>

#define JSON_NO_IO 1
#include <nlohmann/json.hpp>

static int copy_content_id(const std::string &cid, char *dest, size_t dest_len) {
  if (cid.size() < 16 || dest_len <= 10) {
    return 0;
  }
  std::memcpy(dest, cid.data() + 7, 9);
  dest[9] = '\0';
  hyphenate_serial(dest, dest_len);
  return dest[0] != '\0';
}

// 1: this object was readable. *matched is set for the first PS5GD row.
// 0: contentType or contentId is present but not a string.
static int read_content(const nlohmann::json &content, char *dest, size_t dest_len, int *matched) {
  const nlohmann::json *type = nullptr;
  const nlohmann::json *cid = nullptr;

  *matched = 0;
  if (!content.is_object()) {
    return 0;
  }
  auto type_it = content.find("contentType");
  if (type_it != content.end()) {
    if (!type_it->is_string()) {
      return 0;
    }
    type = &*type_it;
  }
  auto cid_it = content.find("contentId");
  if (cid_it != content.end()) {
    if (!cid_it->is_string()) {
      return 0;
    }
    cid = &*cid_it;
  }
  if (type != nullptr) {
    const std::string &name = type->get_ref<const std::string &>();
    if (name.size() >= 5 && name.compare(0, 5, "PS5GD") == 0) {
      *matched = 1;
      if (cid != nullptr) {
        copy_content_id(cid->get_ref<const std::string &>(), dest, dest_len);
      }
    }
  }
  return 1;
}

extern "C" int json_ps5_title(const unsigned char *buf, size_t n, char *dest, size_t dest_len) {
  if (!dest || dest_len == 0) {
    return 0;
  }
  dest[0] = '\0';
  if (buf == nullptr) {
    return 0;
  }
  if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
    buf += 3;
    n -= 3;
  }
  try {
    const char *text = reinterpret_cast<const char *>(buf);
    nlohmann::json doc = nlohmann::json::parse(text, text + n, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
      return 0;
    }
    auto disc_it = doc.find("disc");
    if (disc_it == doc.end() || !disc_it->is_array()) {
      return 0;
    }
    for (const auto &disc : *disc_it) {
      if (!disc.is_object()) {
        return 0;
      }
      auto contents_it = disc.find("contents");
      if (contents_it == disc.end()) {
        continue;
      }
      if (!contents_it->is_array()) {
        return 0;
      }
      for (const auto &content : *contents_it) {
        int matched = 0;
        if (!read_content(content, dest, dest_len, &matched)) {
          return 0;
        }
        if (matched) {
          return dest[0] != '\0';
        }
      }
    }
  } catch (const std::exception &) {
    return 0;
  }
  return 0;
}
