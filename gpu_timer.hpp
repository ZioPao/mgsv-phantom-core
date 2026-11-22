#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What an add-on's passes cost on the GPU. While someone looks (the overlay open, the costs on screen, or `watch` called
// each second), each timed pass ends a timestamp query before and after its work, in the context it records in. The game
// records a frame on its deferred contexts while the immediate context executes the one before, so a deferred context's
// timestamps follow its command list and count for the frame whose present follows the list's execution. They are read
// back a few frames later, without waiting, and averaged over about half a second.
//
// Nothing is timed while no one looks: a pass costs one atomic load. An add-on names its passes once (`registerPass`),
// wraps each pass's GPU work in a `Timed`, and calls `executing`, `recorded`, `forget` and `present` from the ReShade
// events named beside them; the core registers no events itself. docs/addon-core.md, "GPU timer".
namespace fxcore::gpu
{
// Passes an add-on can name; one more is counted under the last.
constexpr std::size_t MaxPasses = 32;

using Pass = std::uint16_t;

// A pass by name, once, at start-up. The same literal gives the same pass. Thread-safe.
Pass registerPass(const char* name) noexcept;

struct PassCost
{
    const char* name{};
    float milliseconds{}; // 0 where the pass did not run
};

// In milliseconds, averaged over about half a second.
struct Costs
{
    float frame{};    // Between presents: the whole frame, as the frame rate shows it
    float gpuFrame{}; // The GPU's work on a frame, from its first command list to its present
    std::vector<PassCost> passes; // Every registered pass, in the order they were registered
};

struct Status
{
    bool overlay{}; // The costs show over the game
    Costs costs;
};

// ---- Setup, on the thread running the immediate context ----
void attach(ID3D11Device* device, ID3D11DeviceContext* immediate) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;

// ---- Recording, on any thread ----
struct TimedRun
{
    Pass pass{};
    Microsoft::WRL::ComPtr<ID3D11Query> start, end;
};

// Times the GPU work recorded on `context` from its construction until end() or its destruction. While no one looks it
// does nothing.
class Timed
{
public:
    Timed(ID3D11DeviceContext* context, Pass pass) noexcept;
    ~Timed() { end(); }
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;

    // Ends the pass being timed, if any, and starts timing `pass`.
    void next(Pass pass) noexcept;
    void end() noexcept;

private:
    ID3D11DeviceContext* context;
    std::optional<TimedRun> run;
};

// Work timed elsewhere (another queue), counted for the frame being timed now.
void addTime(Pass pass, float milliseconds) noexcept;

// ---- ReShade's events, called with the contexts' and lists' native pointers ----
// execute_secondary_command_list on the immediate list: before the immediate context executes `list`.
void executing(std::uint64_t list) noexcept;
// execute_secondary_command_list on a deferred list, and finish_command_list: what `from` recorded now belongs to `into`.
void recorded(std::uint64_t into, std::uint64_t from) noexcept;
// destroy_command_list: the address may come back.
void forget(std::uint64_t contextOrList) noexcept;
// Each present, after the add-on's own present work.
void present() noexcept;

// ---- For the add-on's overlay ----
Status status();
// The costs are on screen or kept on file: frames are timed for the next second.
void watch() noexcept;
void showOverlay(bool show) noexcept;
// The costs as plain text, for a log or a file.
std::string report(const Costs& costs);
}
