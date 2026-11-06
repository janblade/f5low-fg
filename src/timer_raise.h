// The system timer raise of the own generator (xefg_own.h, SetTimerRaised): a 1 ms timer while it generates, as DLSS-G
// has. Without one, Sleep(1) anywhere in the process rounds up to the 15.6 ms tick. timeBeginPeriod requests stack, so the
// application's own is left alone as long as every begin that succeeded gets its one end. No Windows in here: tested on
// the host (tests/timer_raise_test.cpp).
#pragma once

namespace timerraise
{
class Hold
{
  public:
    // begin() returns true when the request was taken; end() is only called for one that was
    template <class Begin, class End> void Set(bool on, Begin begin, End end)
    {
        if (on == held)
            return;
        if (on)
            held = begin();
        else
        {
            end();
            held = false;
        }
    }
    bool Held() const { return held; }

  private:
    bool held = false;
};
} // namespace timerraise
