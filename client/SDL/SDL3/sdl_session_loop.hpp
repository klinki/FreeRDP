/** Reconnect loop policy. Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0. */
#pragma once

enum class SdlSessionStep { Continue, FocusWriteFailure, TransportFailure, LocalFailure, Stopped };
enum class SdlSessionResult { Stopped, Failed };

// Callbacks keep the event-loop decisions deterministic under test. Transport
// recovery itself remains owned by client_auto_reconnect_ex and its policy.
template <typename Stop, typename Abort, typename Step, typename Recover>
SdlSessionResult sdl_run_session_loop(Stop stop, Abort abort, Step step, Recover recover)
{
    while (!stop())
    {
        const auto result = abort() ? SdlSessionStep::TransportFailure : step();
        if (stop() || result == SdlSessionStep::Stopped)
            return SdlSessionResult::Stopped;
        if (result == SdlSessionStep::Continue)
            continue;
        if (result == SdlSessionStep::LocalFailure)
            return SdlSessionResult::Failed;
        if (!recover())
            return stop() ? SdlSessionResult::Stopped : SdlSessionResult::Failed;
    }
    return SdlSessionResult::Stopped;
}
