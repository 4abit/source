#pragma once

#include <atomic>
#include <cstdint>

namespace nl {

// Stages reported by run_inject. The window maps them to status text.
enum class InjectStage : int {
    CheckFiles = 0,
    MissingDll,
    LaunchGame,
    LaunchFailed,
    Waiting,
    Cancelled,
    Preparing,
    Injecting,
    Verifying,
    AlreadyLoaded,
    Success,
    OpenFailed,
    InjectFailed,
    ModuleRejected,
    BadPid,
    NotRunning,
};

struct InjectOptions {
    bool unload = false;
    bool wait_for_game = false;
    bool launch_game = true;
    bool hold = true;
    std::uint32_t pid = 0;
};

// progress is 0..1. failed/done end the job. status text is chosen by the caller from `stage`.
using InjectReportFn = void (*)(void* user, float progress, int stage, bool failed, bool done);

// Blocking. `report` and `cancel` may be null (console run). Returns 0 on success.
int run_inject(const InjectOptions& options, InjectReportFn report, void* user, std::atomic<bool>* cancel);

// Previous console entry: --unload, --wait, --no-launch, --no-pause, --pid.
int loader_console(int argc, wchar_t** argv);

} // namespace nl
