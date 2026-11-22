#include "core/gpu_timer.hpp"

#include "clock.hpp"
#include "core/command_list_notes.hpp"
#include "log.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <mutex>
#include <utility>

namespace fxcore::gpu
{
namespace
{
using Microsoft::WRL::ComPtr;
using Milliseconds = std::array<float, MaxPasses>;

// What a frame took on the GPU.
struct FrameCost
{
    float gpuFrame{}; // 0 when no command list executed
    Milliseconds passes{};
};

// A frame on the immediate context: the disjoint query around it, timestamps at its first command list and at its
// present, and the passes its command lists ran.
struct Frame
{
    ComPtr<ID3D11Query> disjoint, start, end;
    std::vector<TimedRun> runs;
    Milliseconds added{}; // Passes timed elsewhere (addTime)
    bool started{};       // A command list executed, so `start` was ended
    bool pending{};       // Closed and not read yet
};

// The runs a deferred context recorded since its last FinishCommandList, then those of its command list.
struct RecordedRuns
{
    std::vector<TimedRun> runs;

    void merge(RecordedRuns later)
    {
        runs.insert(runs.end(), std::make_move_iterator(later.runs.begin()), std::make_move_iterator(later.runs.end()));
    }
};

// Frames the GPU may still be working on, and the one being recorded.
constexpr std::size_t FrameCount = 5;
constexpr float AverageMilliseconds = 500;

// ---- The passes' names ----
std::mutex passMutex;
std::array<const char*, MaxPasses> passNames{};
std::size_t passCount{};

// ---- The timer: taken from any thread, and nothing else is locked while it is held ----
std::mutex timerMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> immediate;
std::array<Frame, FrameCount> frames;
std::size_t current{};
bool open{}; // frames[current] takes the runs of the command lists executed now
std::vector<TimedRun> spare;

fxcore::CommandListNotes<RecordedRuns> recordedRuns;

// Read on every recording thread.
std::atomic<bool> timing{};
std::atomic<std::uint64_t> watchedAt{};
std::atomic<bool> overlay{};

// ---- On the presenting thread ----
Costs costs;
Milliseconds averaged{};
bool averaging{}; // `averaged` has a GPU sample since the timing began
LONGLONG lastPresent{}, lastSample{};

std::mutex statusMutex;
Status published;

// Call with timerMutex held. Null when the device refuses.
ComPtr<ID3D11Query> query(D3D11_QUERY type)
{
    ComPtr<ID3D11Query> result;
    const D3D11_QUERY_DESC description{type, 0};
    if (device && FAILED(device->CreateQuery(&description, result.GetAddressOf()))) result.Reset();

    return result;
}

// Call with timerMutex held.
void giveBack(std::vector<TimedRun>& runs)
{
    spare.insert(spare.end(), std::make_move_iterator(runs.begin()), std::make_move_iterator(runs.end()));
    runs.clear();
}

std::optional<TimedRun> acquire(Pass pass)
{
    if (!timing.load(std::memory_order_relaxed)) return std::nullopt;

    std::lock_guard lock(timerMutex);
    if (!device) return std::nullopt;

    TimedRun run;
    if (!spare.empty())
    {
        run = std::move(spare.back());
        spare.pop_back();
    }
    else run = {pass, query(D3D11_QUERY_TIMESTAMP), query(D3D11_QUERY_TIMESTAMP)};

    if (!run.start || !run.end) return std::nullopt;

    run.pass = pass;

    return run;
}

// A run the immediate context recorded counts for the frame now executing; a deferred context's waits for its command
// list.
void finish(ID3D11DeviceContext* context, TimedRun run)
{
    {
        std::lock_guard lock(timerMutex);
        if (context == immediate.Get())
        {
            if (open) frames[current].runs.push_back(std::move(run));
            else spare.push_back(std::move(run));

            return;
        }
    }

    recordedRuns.note(reinterpret_cast<std::uint64_t>(context), [&](RecordedRuns& contextRuns) { contextRuns.runs.push_back(std::move(run)); });
}

// Call with timerMutex held.
std::optional<float> millisecondsBetween(ID3D11Query* start, ID3D11Query* end, UINT64 frequency)
{
    UINT64 from{}, to{};
    if (immediate->GetData(start, &from, sizeof(from), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
        || immediate->GetData(end, &to, sizeof(to), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || to < from)
        return std::nullopt;

    return static_cast<float>(static_cast<double>(to - from) * 1000.0 / static_cast<double>(frequency));
}

// Call with timerMutex held. Nullopt while the GPU has not finished the frame, or when its clock changed speed during it
// (a power state change), which makes its times meaningless.
std::optional<FrameCost> read(Frame& frame)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    if (immediate->GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return std::nullopt;

    frame.pending = false;
    if (disjoint.Disjoint || !disjoint.Frequency)
    {
        giveBack(frame.runs);
        return std::nullopt;
    }

    FrameCost sample{.passes = frame.added};
    if (frame.started) sample.gpuFrame = millisecondsBetween(frame.start.Get(), frame.end.Get(), disjoint.Frequency).value_or(0.0f);
    for (const auto& run : frame.runs)
        if (const auto time = millisecondsBetween(run.start.Get(), run.end.Get(), disjoint.Frequency))
            sample.passes[std::min<std::size_t>(run.pass, MaxPasses - 1)] += *time;

    giveBack(frame.runs);

    return sample;
}

// Closes the frame being recorded, then returns the newest frame the GPU has finished since the last call, if any. Oldest
// first; the frame about to be recorded into again is given up if the GPU has not finished it.
std::optional<FrameCost> closeAndRead()
{
    std::lock_guard lock(timerMutex);
    if (!immediate) return std::nullopt;

    if (std::exchange(open, false))
    {
        auto& frame = frames[current];
        if (frame.started) immediate->End(frame.end.Get());
        immediate->End(frame.disjoint.Get());
        frame.pending = true;
        current = (current + 1) % FrameCount;
    }

    std::optional<FrameCost> newest;
    for (std::size_t i = 0; i < FrameCount; ++i)
    {
        auto& candidate = frames[(current + i) % FrameCount];
        if (!candidate.pending) continue;

        if (auto sample = read(candidate)) newest = sample;
        else if (candidate.pending && &candidate == &frames[current])
        {
            candidate.pending = false;
            giveBack(candidate.runs);
        }
    }

    return newest;
}

// Starts timing the next frame, or stops when no one looks.
void openFrame(bool wanted)
{
    std::lock_guard lock(timerMutex);
    auto& frame = frames[current];
    if (!wanted || !immediate || frame.pending)
    {
        timing = false;
        return;
    }

    if (!frame.disjoint)
    {
        frame.disjoint = query(D3D11_QUERY_TIMESTAMP_DISJOINT);
        frame.start = query(D3D11_QUERY_TIMESTAMP);
        frame.end = query(D3D11_QUERY_TIMESTAMP);
    }

    if (!frame.disjoint || !frame.start || !frame.end)
    {
        timing = false;
        return;
    }

    immediate->Begin(frame.disjoint.Get());
    frame.added = {};
    frame.started = false;
    open = true;
    timing = true;
}

// Adds the frame to the averages, which follow about the last half second.
void average(LONGLONG now, const std::optional<FrameCost>& sample)
{
    if (lastPresent)
    {
        const float frame = static_cast<float>(phantom_fx::clock::millisecondsOf(now - lastPresent));
        costs.frame += (frame - costs.frame) * (1 - std::exp(-frame / AverageMilliseconds));
    }

    lastPresent = now;
    if (!sample) return;

    // The first sample after the timing began replaces whatever was left from before.
    const float weight = averaging ? 1 - std::exp(-static_cast<float>(phantom_fx::clock::millisecondsOf(now - lastSample)) / AverageMilliseconds) : 1;
    averaging = true;
    lastSample = now;
    costs.gpuFrame += (sample->gpuFrame - costs.gpuFrame) * weight;
    for (std::size_t i = 0; i < MaxPasses; ++i) averaged[i] += (sample->passes[i] - averaged[i]) * weight;
}

void publish()
{
    std::size_t count{};
    std::array<const char*, MaxPasses> names{};
    {
        std::lock_guard lock(passMutex);
        count = passCount;
        names = passNames;
    }

    std::lock_guard lock(statusMutex);
    published.overlay = overlay;
    published.costs.frame = costs.frame;
    published.costs.gpuFrame = costs.gpuFrame;
    published.costs.passes.clear();
    for (std::size_t i = 0; i < count; ++i) published.costs.passes.push_back({names[i], averaged[i]});
}
}

Pass registerPass(const char* name) noexcept
{
    std::lock_guard lock(passMutex);
    for (std::size_t i = 0; i < passCount; ++i)
        if (std::strcmp(passNames[i], name) == 0) return static_cast<Pass>(i);

    if (passCount == MaxPasses) return static_cast<Pass>(MaxPasses - 1);

    passNames[passCount] = name;

    return static_cast<Pass>(passCount++);
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* gameImmediate) noexcept
{
    std::lock_guard lock(timerMutex);
    device = gameDevice;
    immediate = gameImmediate;
}

void detach() noexcept
{
    timing = false;
    recordedRuns.clear();

    std::lock_guard lock(timerMutex);
    frames = {};
    current = 0;
    open = false;
    spare.clear();
    immediate.Reset();
    device.Reset();
    averaging = false;
}

Timed::Timed(ID3D11DeviceContext* context, Pass pass) noexcept : context(context)
{
    next(pass);
}

void Timed::next(Pass pass) noexcept
{
    end();
    try
    {
        run = acquire(pass);
    }
    catch (...)
    {
        run.reset();
    }

    if (run) context->End(run->start.Get());
}

void Timed::end() noexcept
{
    if (!run) return;

    context->End(run->end.Get());
    try
    {
        finish(context, std::move(*run));
    }
    catch (...)
    {
    }

    run.reset();
}

void addTime(Pass pass, float milliseconds) noexcept
{
    if (!timing.load(std::memory_order_relaxed)) return;

    try
    {
        std::lock_guard lock(timerMutex);
        if (open) frames[current].added[std::min<std::size_t>(pass, MaxPasses - 1)] += milliseconds;
    }
    catch (...)
    {
    }
}

void recorded(std::uint64_t into, std::uint64_t from) noexcept
{
    recordedRuns.recorded(into, from);
}

void forget(std::uint64_t contextOrList) noexcept
{
    // A list gone before it ran gives its queries back.
    auto listRuns = recordedRuns.take(contextOrList);
    recordedRuns.forget(contextOrList);
    if (!listRuns) return;

    try
    {
        std::lock_guard lock(timerMutex);
        giveBack(listRuns->runs);
    }
    catch (...)
    {
    }
}

void executing(std::uint64_t list) noexcept
{
    try
    {
        auto listRuns = recordedRuns.take(list);

        std::lock_guard lock(timerMutex);
        if (open && !frames[current].started)
        {
            immediate->End(frames[current].start.Get());
            frames[current].started = true;
        }

        if (!listRuns) return;

        auto& runs = frames[current].runs;
        if (open) runs.insert(runs.end(), std::make_move_iterator(listRuns->runs.begin()), std::make_move_iterator(listRuns->runs.end()));
        else giveBack(listRuns->runs);
    }
    catch (...)
    {
    }
}

void present() noexcept
{
    recordedRuns.present();

    try
    {
        const auto now = phantom_fx::clock::ticksNow();
        const auto sample = closeAndRead();
        average(now, sample);

        const bool wanted = overlay || GetTickCount64() - watchedAt.load() < 1000;
        if (!wanted) averaging = false;
        openFrame(wanted);
        publish();
    }
    catch (const std::exception& error)
    {
        phantom_fx::log(std::string("GPU timer: ") + error.what());
    }
}

Status status()
{
    std::lock_guard lock(statusMutex);
    return published;
}

void watch() noexcept
{
    watchedAt = GetTickCount64();
}

void showOverlay(bool show) noexcept
{
    overlay = show;
}

std::string report(const Costs& shown)
{
    float timed = 0;
    for (const auto& pass : shown.passes) timed += pass.milliseconds;

    const float share = shown.frame > 0 ? 100.0f * timed / shown.frame : 0.0f;
    std::string text = std::format("{:.2f} ms of GPU work in the passes ({:.1f}% of a {:.1f} ms frame); the GPU's frame {:.1f} ms", timed, share,
        shown.frame, shown.gpuFrame);
    for (const auto& pass : shown.passes)
        if (pass.milliseconds > 0) text += std::format("; {} {:.2f}", pass.name, pass.milliseconds);

    return text;
}
}
