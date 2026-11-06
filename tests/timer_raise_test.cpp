// The begin / end pairing of the timer raise (src/timer_raise.h). Host only.
#include "../src/timer_raise.h"

#include <cstdio>

static int failures = 0;

static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

int main()
{
    int begins = 0, ends = 0;
    bool accept = true;
    const auto begin = [&] { ++begins; return accept; };
    const auto end = [&] { ++ends; };

    {
        timerraise::Hold h;
        h.Set(false, begin, end);
        Check(begins == 0 && ends == 0 && !h.Held(), "off while not held does nothing (no end without a begin)");
        h.Set(true, begin, end);
        h.Set(true, begin, end);
        Check(begins == 1 && ends == 0 && h.Held(), "on twice is one begin");
        h.Set(false, begin, end);
        h.Set(false, begin, end);
        Check(begins == 1 && ends == 1 && !h.Held(), "off twice is one end");
    }

    begins = ends = 0;
    {
        timerraise::Hold h;
        for (int i = 0; i < 5; ++i)
        {
            h.Set(true, begin, end);
            h.Set(false, begin, end);
        }
        Check(begins == 5 && ends == 5, "every toggle stays paired");
    }

    begins = ends = 0;
    accept = false;
    {
        timerraise::Hold h;
        h.Set(true, begin, end);
        Check(begins == 1 && !h.Held(), "a refused begin holds nothing");
        h.Set(false, begin, end);
        Check(ends == 0, "a refused begin gets no end");
        accept = true;
        h.Set(true, begin, end);
        Check(begins == 2 && h.Held(), "the next enable asks again");
        h.Set(false, begin, end);
        Check(ends == 1, "and that one is ended");
    }

    if (failures == 0)
        std::printf("timer_raise_test PASS\n");
    return failures == 0 ? 0 : 1;
}
