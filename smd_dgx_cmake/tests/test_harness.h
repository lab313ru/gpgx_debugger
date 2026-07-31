#pragma once
//
// A test harness in one header, on purpose.
//
// This repository had no tests at all, and the first two bugs a test suite
// would have caught were both cheap to express and expensive to find by hand.
// Adding Catch2/GTest would mean a vcpkg dependency in the build of a project
// whose whole point is to link statically into someone else's process; sixty
// lines of asserts buy the same thing here.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace t {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& cases()
{
    static std::vector<Case> v;
    return v;
}

inline int& failures()
{
    static int n = 0;
    return n;
}

inline bool& currentFailed()
{
    static bool b = false;
    return b;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { cases().push_back({ name, std::move(fn) }); }
};

inline void fail(const char* file, int line, const std::string& what)
{
    std::printf("    FAIL %s:%d\n         %s\n", file, line, what.c_str());
    ++failures();
    currentFailed() = true;
}

inline std::string hex(uint64_t v)
{
    char b[32];
    std::snprintf(b, sizeof b, "0x%llX", (unsigned long long)v);
    return b;
}

inline std::string hexBytes(const std::vector<uint8_t>& v, size_t max = 32)
{
    std::string s;
    for (size_t i = 0; i < v.size() && i < max; ++i) {
        char b[4];
        std::snprintf(b, sizeof b, "%02x", v[i]);
        s += b;
    }
    if (v.size() > max) s += "...";
    return s;
}

inline int run(const char* filter)
{
    int ran = 0;
    for (const auto& c : cases()) {
        if (filter && *filter && std::strstr(c.name, filter) == nullptr) continue;
        currentFailed() = false;
        std::printf("  %s\n", c.name);
        const int before = failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            fail(__FILE__, __LINE__, std::string("threw: ") + e.what());
        } catch (...) {
            fail(__FILE__, __LINE__, "threw a non-std exception");
        }
        if (failures() == before) std::printf("    ok\n");
        ++ran;
    }
    std::printf("\n%d case(s), %d failure(s)\n", ran, failures());
    return failures() == 0 ? 0 : 1;
}

} // namespace t

#define TEST(name)                                                             \
    static void name();                                                        \
    static ::t::Registrar reg_##name(#name, name);                             \
    static void name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) ::t::fail(__FILE__, __LINE__, "CHECK(" #cond ")");         \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        const auto _a = (a);                                                    \
        const auto _b = (b);                                                    \
        if (!(_a == _b))                                                        \
            ::t::fail(__FILE__, __LINE__,                                       \
                      std::string(#a " == " #b "  (got ") + ::t::hex((uint64_t)_a) \
                          + ", want " + ::t::hex((uint64_t)_b) + ")");          \
    } while (0)

#define CHECK_STR(a, b)                                                        \
    do {                                                                       \
        const std::string _a = (a);                                             \
        const std::string _b = (b);                                             \
        if (_a != _b)                                                           \
            ::t::fail(__FILE__, __LINE__,                                       \
                      std::string(#a "  (got \"") + _a + "\", want \"" + _b + "\")"); \
    } while (0)

// Abort the current case: further checks would cascade from a failed setup.
#define REQUIRE(cond)                                                          \
    do {                                                                       \
        if (!(cond)) {                                                          \
            ::t::fail(__FILE__, __LINE__, "REQUIRE(" #cond ") — aborting case"); \
            return;                                                             \
        }                                                                       \
    } while (0)
