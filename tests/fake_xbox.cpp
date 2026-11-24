// Pretend Xbox-only library. If the loader ever calls this real implementation
// instead of stubbing it, fake_game notices because it returns non-zero.
extern "C" __declspec(dllexport) int XFakeXboxOnly(int x) { return x * 2 + 1; }
