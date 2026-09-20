#ifndef BRIDGE_JSON_H
#define BRIDGE_JSON_H

/*
 * Minimal JSON support for the JS<->native bridge. Inbound messages are flat
 * objects of scalars, so anything else (nesting, duplicate keys, \u escapes,
 * trailing text) is rejected rather than guessed at. Header-only, no Windows
 * dependency, so it can be unit-tested on its own.
 */

#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace bridge {

struct Value {
	bool is_string;
	std::string text; /* decoded for strings, raw token otherwise */
};

typedef std::map<std::string, Value> Object;

enum GetResult { kOk, kMissing, kInvalid };

namespace detail {

inline void SkipWs(const std::string &s, size_t &i)
{
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
				s[i] == '\r'))
		i++;
}

inline bool ParseString(const std::string &s, size_t &i, std::string &out)
{
	out.clear();
	if (i >= s.size() || s[i] != '"')
		return false;
	for (i++; i < s.size(); i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"') {
			i++;
			return true;
		}
		if (c < 0x20)
			return false;
		if (c != '\\') {
			out += (char)c;
			continue;
		}
		if (++i >= s.size())
			return false;
		switch (s[i]) {
		case '"': out += '"'; break;
		case '\\': out += '\\'; break;
		case '/': out += '/'; break;
		case 'b': out += '\b'; break;
		case 'f': out += '\f'; break;
		case 'n': out += '\n'; break;
		case 'r': out += '\r'; break;
		case 't': out += '\t'; break;
		default: return false; /* includes \u: not used by the bridge */
		}
	}
	return false;
}

} /* namespace detail */

inline bool ParseFlatObject(const std::string &s, Object &out)
{
	size_t i = 0;

	out.clear();
	detail::SkipWs(s, i);
	if (i >= s.size() || s[i++] != '{')
		return false;
	detail::SkipWs(s, i);
	if (i < s.size() && s[i] == '}') {
		i++;
	} else {
		for (;;) {
			std::string key;
			Value v;

			detail::SkipWs(s, i);
			if (!detail::ParseString(s, i, key))
				return false;
			detail::SkipWs(s, i);
			if (i >= s.size() || s[i++] != ':')
				return false;
			detail::SkipWs(s, i);
			if (i >= s.size())
				return false;

			if (s[i] == '"') {
				v.is_string = true;
				if (!detail::ParseString(s, i, v.text))
					return false;
			} else {
				size_t start = i;
				while (i < s.size() && s[i] != ',' && s[i] != '}' &&
				       s[i] != ' ' && s[i] != '\t' &&
				       s[i] != '\n' && s[i] != '\r')
					i++;
				v.is_string = false;
				v.text = s.substr(start, i - start);
				if (v.text.empty() || v.text[0] == '{' ||
				    v.text[0] == '[')
					return false; /* nesting is not accepted */
			}

			if (out.count(key))
				return false; /* duplicate key */
			out[key] = v;

			detail::SkipWs(s, i);
			if (i < s.size() && s[i] == ',') {
				i++;
				continue;
			}
			if (i < s.size() && s[i] == '}') {
				i++;
				break;
			}
			return false;
		}
	}
	detail::SkipWs(s, i);
	return i == s.size();
}

inline GetResult GetString(const Object &o, const char *key, std::string &out)
{
	Object::const_iterator it = o.find(key);
	if (it == o.end())
		return kMissing;
	if (!it->second.is_string)
		return kInvalid;
	out = it->second.text;
	return kOk;
}

/*
 * Strict base-10 integer within [lo, hi]: no sign other than '-', no leading
 * zeros, no fraction or exponent (so "1e9" and "1.0" are rejected, not
 * truncated), at most 18 digits so it cannot overflow long long.
 */
inline GetResult GetInt(const Object &o, const char *key, long long lo,
			long long hi, long long &out)
{
	Object::const_iterator it = o.find(key);
	if (it == o.end())
		return kMissing;
	if (it->second.is_string)
		return kInvalid;

	const std::string &t = it->second.text;
	size_t i = 0;
	bool neg = false;
	long long v = 0;

	if (i < t.size() && t[i] == '-') {
		neg = true;
		i++;
	}
	size_t digits = t.size() - i;
	if (digits == 0 || digits > 18)
		return kInvalid;
	if (t[i] == '0' && digits > 1)
		return kInvalid;
	for (; i < t.size(); i++) {
		if (t[i] < '0' || t[i] > '9')
			return kInvalid;
		v = v * 10 + (t[i] - '0');
	}
	if (neg)
		v = -v;
	if (v < lo || v > hi)
		return kInvalid;
	out = v;
	return kOk;
}

/* Escape for embedding inside a JSON string literal. */
inline std::string JsonEscape(const std::string &s)
{
	std::string r;

	r.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\') {
			r += '\\';
			r += (char)c;
		} else if (c == '\n') {
			r += "\\n";
		} else if (c == '\r') {
			r += "\\r";
		} else if (c == '\t') {
			r += "\\t";
		} else if (c < 0x20) {
			char buf[8];
			std::snprintf(buf, sizeof(buf), "\\u%04x", c);
			r += buf;
		} else {
			r += (char)c;
		}
	}
	return r;
}

/* JSON has no NaN or Infinity; a stray one must not poison the whole frame. */
inline double Finite(double v)
{
	return std::isfinite(v) ? v : 0.0;
}

} /* namespace bridge */

#endif
