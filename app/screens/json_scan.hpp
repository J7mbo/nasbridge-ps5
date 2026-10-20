// NASBridge - Reading values out of small, flat JSON replies.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ShadowMountPlus answers with flat objects and arrays of flat objects, so a
// key scanner is enough: find "key": and read the value after it. Every
// function looks only inside [from, to), which is how one array item is read
// without its neighbours.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace hui::concepts
{

// The first character of the value of "key" in [from, to), or nullptr.
const char *json_value(const char *from, const char *to, const char *key);
// A non-negative whole number; false when it's missing or not a number.
bool json_number(const char *from, const char *to, const char *key, std::uint64_t *out);
// A whole number that may be negative (statuses); `fallback` when missing.
long long json_integer(const char *from, const char *to, const char *key, long long fallback);
// A string, with \" \\ \/ \n \t and \uXXXX (as UTF-8) decoded; "" when missing.
std::string json_text(const char *from, const char *to, const char *key);
// true only for the literal true.
bool json_true(const char *from, const char *to, const char *key);
// Calls `item` with the bounds of each object in the array "key".
void json_objects(const std::string &json, const char *key,
                  const std::function<void(const char *from, const char *to)> &item);
// For writing requests: `text` as a quoted JSON string.
std::string json_quote(const std::string &text);

} // namespace hui::concepts
