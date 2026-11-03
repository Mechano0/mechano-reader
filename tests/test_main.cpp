#include <cstdio>
#include "mzr_test.h"

int main() {
    for (auto& c : mzr_test::Registry()) {
        const int before = mzr_test::Failures();
        c.fn();
        std::printf("%s  %s\n", mzr_test::Failures() == before ? "[ OK ]" : "[FAIL]", c.name);
    }
    std::printf("\n%zu test cases, %d checks, %d failures\n",
                mzr_test::Registry().size(), mzr_test::Checks(), mzr_test::Failures());
    return mzr_test::Failures() ? 1 : 0;
}
