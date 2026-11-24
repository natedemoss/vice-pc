// Stand-in for a console title, run *through* vice-pc by the tests.
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

#include <intrin.h>

extern "C" __declspec(dllimport) int XFakeXboxOnly(int);
extern "C" unsigned long _tls_index;  // provided by the CRT's TLS support

thread_local int tls_value = 42;

static int fails = 0;
#define CHECK(cond, ...)                \
    do {                                \
        if (!(cond)) {                  \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");               \
            ++fails;                    \
        }                               \
    } while (0)

int main() {
    // 1. Implicit TLS on the main thread.
    CHECK(tls_value == 42, "main thread TLS initial value %d", tls_value);

    // 1b. Console titles read their TLS block straight from slot 0 (local-exec model),
    //     so the loader must put our block there and set _tls_index to 0.
    void** tls_array = reinterpret_cast<void**>(__readgsqword(0x58));
    CHECK(_tls_index == 0, "_tls_index is %lu, expected 0", _tls_index);
    CHECK(tls_array[0] == tls_array[_tls_index], "slot 0 does not hold the game's TLS block");
    tls_value = 7;

    // 2. Implicit TLS on new threads: each gets a fresh copy.
    std::vector<int> seen(4, -1);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([i, &seen] {
            seen[i] = tls_value;
            tls_value = 100 + i;
        });
    }
    for (auto& t : threads) t.join();
    for (int i = 0; i < 4; ++i) CHECK(seen[i] == 42, "thread %d saw TLS value %d", i, seen[i]);
    CHECK(tls_value == 7, "main TLS clobbered by worker threads: %d", tls_value);

    // 3. C++ exceptions unwind through code in the mapped image.
    bool caught = false;
    try {
        throw std::runtime_error("boom");
    } catch (const std::exception&) {
        caught = true;
    }
    CHECK(caught, "C++ exception was not caught");

    // 4. Xbox-only import is stubbed (returns 0), not the real DLL (returns 11).
    int r = XFakeXboxOnly(5);
    CHECK(r == 0, "Xbox-only import returned %d, expected stub value 0", r);

    if (fails == 0) printf("FAKE GAME: all checks passed\n");
    else printf("FAKE GAME: %d failures\n", fails);
    fflush(stdout);
    return fails;
}
