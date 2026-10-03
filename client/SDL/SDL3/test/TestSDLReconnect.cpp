/** Deterministic event-loop fault injection. Copyright 2026 FreeRDP contributors.
 * Licensed under Apache-2.0. */
#include "../sdl_session_loop.hpp"
#include <cstdio>

struct Fixture
{
    bool stopped = false, aborted = false, enabled = true, serverLogoff = false;
    bool authenticationRejected = false, cancelDuringRetry = false, retrySucceeds = true;
    unsigned decisions = 0, attempts = 0, maximum = 2;
    SdlSessionStep injected = SdlSessionStep::TransportFailure;
    SdlSessionResult run()
    {
        return sdl_run_session_loop([&] { return stopped; }, [&] { return aborted; },
            [&] {
                if (attempts != 0) { stopped = true; return SdlSessionStep::Stopped; }
                aborted = true;
                return injected;
            }, [&] {
                ++decisions;
                if (!enabled || serverLogoff || authenticationRejected || stopped) return false;
                for (unsigned i = 0; i < maximum; ++i)
                {
                    ++attempts;
                    if (cancelDuringRetry) { stopped = true; return false; }
                    if (retrySucceeds) { aborted = false; return true; }
                }
                return false;
            });
    }
};

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    Fixture focus; focus.injected = SdlSessionStep::FocusWriteFailure;
    check(focus.run() == SdlSessionResult::Stopped && focus.attempts == 1,
          "initial focus-sync write failure reaches recovery");
    Fixture abort; abort.aborted = true;
    check(abort.run() == SdlSessionResult::Stopped && abort.attempts == 1,
          "transport abort before loop entry reaches recovery");
    Fixture read;
    check(read.run() == SdlSessionResult::Stopped && read.attempts == 1,
          "established-session transport failure recovers");
    Fixture exhausted; exhausted.retrySucceeds = false;
    check(exhausted.run() == SdlSessionResult::Failed && exhausted.attempts == 2,
          "retry exhaustion remains failure when server error information is absent");
    Fixture cancelled; cancelled.cancelDuringRetry = true;
    check(cancelled.run() == SdlSessionResult::Stopped && cancelled.attempts == 1,
          "cancellation during recovery prevents further attempts");
    Fixture stopped; stopped.stopped = true;
    check(stopped.run() == SdlSessionResult::Stopped && stopped.decisions == 0,
          "explicit stop never enters recovery");
    Fixture logoff; logoff.serverLogoff = true;
    check(logoff.run() == SdlSessionResult::Failed && logoff.attempts == 0,
          "server logoff remains governed by recovery eligibility");
    Fixture auth; auth.authenticationRejected = true;
    check(auth.run() == SdlSessionResult::Failed && auth.attempts == 0,
          "authentication rejection remains governed by recovery eligibility");
    Fixture disabled; disabled.enabled = false;
    check(disabled.run() == SdlSessionResult::Failed && disabled.attempts == 0,
          "disabled auto-reconnect remains disabled");
    Fixture local; local.injected = SdlSessionStep::LocalFailure;
    check(local.run() == SdlSessionResult::Failed && local.decisions == 0,
          "local wait failure is not treated as recoverable transport loss");
    return failures ? 1 : 0;
}
