// NASBridge - Reading values out of small, flat JSON replies.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/json_scan.hpp"

#include <cstdio>
#include <cstring>

namespace hui::concepts
{

namespace
{

bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void append_utf8(std::string *out, std::uint32_t code)
{
    if (code < 0x80)
    {
        out->push_back(static_cast<char>(code));
    }
    else if (code < 0x800)
    {
        out->push_back(static_cast<char>(0xC0 | (code >> 6)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else
    {
        out->push_back(static_cast<char>(0xE0 | (code >> 12)));
        out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

} // namespace

const char *json_value(const char *from, const char *to, const char *key)
{
    const std::size_t length = std::strlen(key);
    for (const char *p = from; p + length + 2 <= to; ++p)
    {
        if (*p != '"' || std::memcmp(p + 1, key, length) != 0 || p[length + 1] != '"')
            continue;
        const char *v = p + length + 2;
        while (v < to && space(*v))
            ++v;
        if (v >= to || *v != ':')
            continue; // the key's text appeared as a value
        ++v;
        while (v < to && space(*v))
            ++v;
        return v < to ? v : nullptr;
    }
    return nullptr;
}

bool json_number(const char *from, const char *to, const char *key, std::uint64_t *out)
{
    const char *v = json_value(from, to, key);
    if (v == nullptr || *v < '0' || *v > '9')
        return false;
    std::uint64_t n = 0;
    for (; v < to && *v >= '0' && *v <= '9'; ++v)
        n = n * 10 + static_cast<std::uint64_t>(*v - '0');
    *out = n;
    return true;
}

long long json_integer(const char *from, const char *to, const char *key, long long fallback)
{
    const char *v = json_value(from, to, key);
    if (v == nullptr)
        return fallback;
    bool negative = false;
    if (*v == '-')
    {
        negative = true;
        ++v;
    }
    if (v >= to || *v < '0' || *v > '9')
        return fallback;
    long long n = 0;
    for (; v < to && *v >= '0' && *v <= '9'; ++v)
        n = n * 10 + (*v - '0');
    return negative ? -n : n;
}

std::string json_text(const char *from, const char *to, const char *key)
{
    const char *v = json_value(from, to, key);
    std::string out;
    if (v == nullptr || *v != '"')
        return out;
    for (++v; v < to && *v != '"'; ++v)
    {
        if (*v != '\\' || v + 1 >= to)
        {
            out += *v;
            continue;
        }
        ++v;
        switch (*v)
        {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case 'u':
        {
            std::uint32_t code = 0;
            bool ok = v + 4 < to;
            for (int i = 1; ok && i <= 4; ++i)
            {
                const int digit = hex_digit(v[i]);
                ok = digit >= 0;
                code = code * 16 + static_cast<std::uint32_t>(digit < 0 ? 0 : digit);
            }
            if (ok)
            {
                append_utf8(&out, code);
                v += 4;
            }
            break;
        }
        default:
            out += *v; // \" \\ \/
            break;
        }
    }
    return out;
}

bool json_true(const char *from, const char *to, const char *key)
{
    const char *v = json_value(from, to, key);
    return v != nullptr && to - v >= 4 && std::memcmp(v, "true", 4) == 0;
}

void json_objects(const std::string &json, const char *key,
                  const std::function<void(const char *from, const char *to)> &item)
{
    const char *begin = json.data();
    const char *end = begin + json.size();
    const char *p = json_value(begin, end, key);
    if (p == nullptr || *p != '[')
        return;
    ++p;
    while (p < end)
    {
        while (p < end && *p != '{' && *p != ']')
            ++p;
        if (p >= end || *p == ']')
            return;
        int depth = 0;
        bool in_text = false;
        const char *q = p;
        for (; q < end; ++q)
        {
            if (in_text)
            {
                if (*q == '\\')
                    ++q;
                else if (*q == '"')
                    in_text = false;
            }
            else if (*q == '"')
                in_text = true;
            else if (*q == '{')
                ++depth;
            else if (*q == '}' && --depth == 0)
                break;
        }
        if (q >= end)
            return;
        item(p, q + 1);
        p = q + 1;
    }
}

std::string json_quote(const std::string &text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (static_cast<unsigned char>(c) < 0x20)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(c));
            out += escaped;
        }
        else
        {
            out += c;
        }
    }
    return out + "\"";
}

} // namespace hui::concepts
