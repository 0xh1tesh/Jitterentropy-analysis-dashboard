#include "bridge_json.h"
#include <cstdio>
#include <limits>
using namespace bridge;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static bool P(const char *j, Object &o) { return ParseFlatObject(j, o); }

int main()
{
	Object o;
	long long n = 0;
	std::string s;

	/* accepted, including whitespace variants the old substring parser missed */
	CHECK(P("{\"action\":\"generate\",\"bytes\":512}", o));
	CHECK(GetString(o, "action", s) == kOk && s == "generate");
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kOk && n == 512);
	CHECK(P("  { \"action\" : \"generate\" , \"bytes\" : 512 }  ", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kOk && n == 512);
	CHECK(P("{\"action\":\"clearHistory\"}", o));
	CHECK(GetInt(o, "bytes", 1, 9, n) == kMissing);
	CHECK(P("{}", o));

	/* the old parser's failure modes */
	CHECK(P("{\"action\":\"generate\",\"bytes\":-5}", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kInvalid);   /* was SIZE_MAX */
	CHECK(P("{\"action\":\"generate\",\"bytes\":1e9}", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kInvalid);   /* was 1 */
	CHECK(P("{\"action\":\"generate\",\"bytes\":1.5}", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kInvalid);
	CHECK(P("{\"action\":\"generate\",\"bytes\":500000000}", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kInvalid);   /* out of range */
	CHECK(P("{\"action\":\"generate\",\"bytes\":99999999999999999999999}", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kInvalid);
	CHECK(P("{\"action\":\"generate\",\"bytes\":007}", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kInvalid);
	CHECK(P("{\"action\":\"generate\",\"bytes\":\"512\"}", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kInvalid);   /* string, not number */
	CHECK(P("{\"action\":\"generate\",\"bytes\":true}", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kInvalid);
	CHECK(P("{\"action\":\"generate\",\"bytes\":0}", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kInvalid);   /* below range */

	/* "bytes": hidden in a string or nested object must NOT be picked up */
	CHECK(P("{\"action\":\"benchmark\",\"note\":\"x \\\"bytes\\\":999 y\"}", o));
	CHECK(GetInt(o, "bytes", 1, 65536, n) == kMissing);
	CHECK(!P("{\"action\":\"generate\",\"opts\":{\"bytes\":999}}", o));
	CHECK(!P("{\"action\":\"generate\",\"opts\":[1,2]}", o));

	/* a string that merely contains an action must not become the action */
	CHECK(P("{\"note\":\"\\\"action\\\":\\\"generate\\\"\"}", o));
	CHECK(GetString(o, "action", s) == kMissing);

	/* malformed / hostile */
	CHECK(!P("", o));
	CHECK(!P("null", o));
	CHECK(!P("[]", o));
	CHECK(!P("{", o));
	CHECK(!P("{\"a\":1", o));
	CHECK(!P("{\"a\":1,}", o));
	CHECK(!P("{\"a\":1}x", o));
	CHECK(!P("{\"a\":1,\"a\":2}", o));                    /* duplicate key */
	CHECK(!P("{\"a\":\"\\u0041\"}", o));
	CHECK(!P("{\"a\":\"unterminated}", o));
	CHECK(!P("{a:1}", o));
	CHECK(!P("{\"a\":}", o));
	CHECK(!P("{\"a\" 1}", o));

	/* escaping */
	CHECK(JsonEscape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001");
	CHECK(JsonEscape("deadbeef") == "deadbeef");
	{
		std::string raw = "q\"\\\n\t/x";
		std::string j = "{\"k\":\"" + JsonEscape(raw) + "\"}";
		Object r;
		CHECK(P(j.c_str(), r));
		CHECK(GetString(r, "k", s) == kOk && s == raw);   /* round trip */
	}

	/* finite guard */
	CHECK(Finite(std::numeric_limits<double>::quiet_NaN()) == 0.0);
	CHECK(Finite(std::numeric_limits<double>::infinity()) == 0.0);
	CHECK(Finite(1.5) == 1.5);

	std::printf(fails ? "FAILED (%d)\n" : "ALL PASSED\n", fails);
	return fails ? 1 : 0;
}
