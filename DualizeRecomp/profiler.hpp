#pragma once

// A sampling profiler for the host threads, for finding where frame time goes
// (DUALIZE_PROFILE=<file>). A sampler thread stops each registered thread
// about once a millisecond and records its call stack; at exit the samples go
// to the file as one line of hex return addresses each, innermost first,
// prefixed with the thread's name. scripts/profile.py turns them into
// per-function self and inclusive times. Windows only; elsewhere a no-op.

namespace dualize::profiler
{
// Samples the calling thread from now on, if profiling is on.
void RegisterThisThread(const char* name);
// Writes the samples and stops sampling. Safe to call when it never started.
void Shutdown();
}  // namespace dualize::profiler
