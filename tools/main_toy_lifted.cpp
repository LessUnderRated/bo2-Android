// main_toy_lifted.cpp — host program for the lifted toy.
// Calls the Remill-generated wrapper call_sub_1000(EAX, EBX) and prints.
#include <cstdio>
#include <cstdint>

extern "C" int32_t call_sub_1000(int32_t eax, int32_t ebx);

int main(int argc, char** argv) {
    struct { int32_t a, b; } cases[] = {
        {5, 9}, {0, 0}, {1, 1}, {100, -7}, {-3, 13}, {0x7FFFFFFF, 0},
    };
    int fails = 0;
    for (auto& c : cases) {
        int32_t got = call_sub_1000(c.a, c.b);
        int32_t exp = ((c.a << 3) + (c.b * 7) + 1);
        // normalize to 32-bit
        uint32_t g = (uint32_t)got, e = (uint32_t)exp;
        bool ok = (g == e);
        if (!ok) fails++;
        std::printf("[%s] lifted(%d, %d) = %#010x  expected %#010x\n",
                    ok ? "PASS" : "FAIL", c.a, c.b, g, e);
    }
    std::printf("\n%s — lifted x86 IR compiled to native arm64 and executed.\n",
                fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
