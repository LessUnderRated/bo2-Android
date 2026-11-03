// Compile with clang++ -O2 -std=c++17 -pthread -I runtime/win32.
// Names have process-long lifetime and intentionally collide in the cache.
#include "bo2_getenv.h"
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>
#include <atomic>
alignas(4096) static char names[8192];
int main() {
  std::strcpy(names, "BO2_CACHE_TEST_A");
  std::strcpy(names + 4096, "BO2_CACHE_TEST_B");
  setenv(names, "alpha", 1); setenv(names + 4096, "beta", 1);
  std::atomic<unsigned long> wrong{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> ts;
  for (int t=0;t<4;t++) ts.emplace_back([&,t] {
    const char* key=names+(t%2)*4096;
    const char* expected=t%2 ? "beta" : "alpha";
    while(!start.load()) {}
    for(int i=0;i<1000000;i++) {
      const char* got=bo2_getenv(key);
      if(!got || std::strcmp(got,expected)) ++wrong;
    }
  });
  start=true;
  for(auto& t:ts)t.join();
  std::printf("wrong=%lu / 4000000\n", wrong.load());
  const char* missing = "BO2_CACHE_TEST_ABSENT_91AA";
  if (bo2_getenv(missing) || bo2_getenv(missing)) return 2;
  return wrong ? 1 : 0;
}
