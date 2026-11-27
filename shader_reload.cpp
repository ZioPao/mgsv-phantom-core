// Development builds only: every shader made from the checkout's .hlsl follows its edits. A worker thread
// looks at each shader's files a few times a second, compiles the ones that changed and makes them on the
// device, which Direct3D 11 allows from any thread. The context's PSSetShader, VSSetShader and CSSetShader
// then bind the newest in place of the one a feature holds, so no feature needs reload code of its own. A
// failed compile is logged and the shader in use stays. Only a shader's body reloads: a changed constant
// buffer layout or binding still needs a rebuild of the code that fills it.
#include "d3d11_helpers.hpp"
#include "game/game_code.hpp"
#include "log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <format>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phantom_fx
{
namespace
{
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

struct Watched
{
    Shader shader;
    ComPtr<ID3D11Device> device;
    // Held, so no other object takes an address the swap below names.
    std::vector<ComPtr<ID3D11DeviceChild>> made;
    // Filled by the worker's first look: the source and what it includes, and their newest write then.
    std::vector<fs::path> files;
    std::optional<fs::file_time_type> seen;
};

std::mutex watchedMutex; // Guards `watched` and `worker`.
std::list<Watched> watched; // A list: the worker keeps pointers to entries while it compiles.
std::jthread worker;

std::shared_mutex swapMutex; // Guards `newest`.
std::unordered_map<ID3D11DeviceChild*, ComPtr<ID3D11DeviceChild>> newest;
std::atomic<bool> anySwapped{false};
// What the hooks have done, for the log: shaders bound, and those bound in another's place.
std::atomic<std::uint64_t> bound{0}, swapped{0};

using SetPixelShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using SetVertexShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
using SetComputeShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
// One set a table: the immediate context's, and the deferred contexts' the game records most of its frame on,
// which are another class with functions of their own.
constexpr std::size_t Tables = 2;
SetPixelShader originalPixel[Tables]{};
SetVertexShader originalVertex[Tables]{};
SetComputeShader originalCompute[Tables]{};
bool hooked{}; // Under watchedMutex.

template<class Object> Object* newestOf(Object* shader) noexcept
{
    // Every draw the game makes passes here too: nothing to look up until a file has changed.
    bound.fetch_add(1, std::memory_order_relaxed);
    if (!shader || !anySwapped.load(std::memory_order_acquire)) return shader;

    std::shared_lock lock(swapMutex);
    const auto found = newest.find(shader);
    if (found == newest.end()) return shader;
    swapped.fetch_add(1, std::memory_order_relaxed);

    return static_cast<Object*>(found->second.Get());
}

template<std::size_t Table>
void STDMETHODCALLTYPE setPixelShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader, ID3D11ClassInstance* const* instances, UINT count)
{
    originalPixel[Table](context, newestOf(shader), instances, count);
}

template<std::size_t Table>
void STDMETHODCALLTYPE setVertexShader(ID3D11DeviceContext* context, ID3D11VertexShader* shader, ID3D11ClassInstance* const* instances, UINT count)
{
    originalVertex[Table](context, newestOf(shader), instances, count);
}

template<std::size_t Table>
void STDMETHODCALLTYPE setComputeShader(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, ID3D11ClassInstance* const* instances, UINT count)
{
    originalCompute[Table](context, newestOf(shader), instances, count);
}

// The module a function lives in, for the log.
std::string moduleOf(const void* function)
{
    HMODULE module{};
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(function), &module)
        || !GetModuleFileNameW(module, path, MAX_PATH))
        return "an unknown module";

    return fs::path(path).filename().string();
}

// Slots in ID3D11DeviceContext's table, as d3d11.h orders it: IUnknown's 3 and ID3D11DeviceChild's 4 come first.
constexpr std::size_t PixelSlot = 9, VertexSlot = 11, ComputeSlot = 69;

void* const* tableOf(ID3D11DeviceContext* context) noexcept
{
    return *reinterpret_cast<void* const* const*>(context);
}

// Hooks `table`'s Set*Shader; a function an earlier table has too is hooked already, and its detour serves both.
template<std::size_t Table> bool hookTable(void* const* table, void* const* earlier) noexcept
{
    const auto fresh = [&](std::size_t slot) { return !earlier || table[slot] != earlier[slot]; };

    return (!fresh(PixelSlot) || game_code::hookAt(table[PixelSlot], &setPixelShader<Table>, originalPixel[Table]) == MH_OK)
        && (!fresh(VertexSlot) || game_code::hookAt(table[VertexSlot], &setVertexShader<Table>, originalVertex[Table]) == MH_OK)
        && (!fresh(ComputeSlot) || game_code::hookAt(table[ComputeSlot], &setComputeShader<Table>, originalCompute[Table]) == MH_OK);
}

// Hooks the immediate context's functions and a deferred context's; call with watchedMutex held.
bool hookContext(ID3D11DeviceContext* context)
{
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;

    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    ComPtr<ID3D11DeviceContext> deferred;
    if (!device || FAILED(device->CreateDeferredContext(0, deferred.GetAddressOf()))) return false;

    auto* const* immediate = tableOf(context);
    const bool done = hookTable<0>(immediate, nullptr) && hookTable<1>(tableOf(deferred.Get()), immediate);
    if (done)
        log("Shader reload: watching the shaders' files; Set*Shader hooked in " + moduleOf(immediate[ComputeSlot]) + " (immediate) and "
            + moduleOf(tableOf(deferred.Get())[ComputeSlot]) + " (deferred)");

    return done;
}

std::optional<fs::file_time_type> newestWrite(const std::vector<fs::path>& files) noexcept
{
    fs::file_time_type latest{};
    for (const auto& file : files)
    {
        std::error_code error;
        const auto written = fs::last_write_time(file, error);
        if (error) return std::nullopt; // Mid-save: look again next time.
        latest = std::max(latest, written);
    }

    return latest;
}

ComPtr<ID3D11DeviceChild> make(ID3D11Device* device, const Shader& shader, ID3DBlob* code)
{
    const auto* bytes = code->GetBufferPointer();
    const auto size = code->GetBufferSize();
    const std::string_view profile = shader.profile;
    HRESULT result = E_INVALIDARG;
    ComPtr<ID3D11DeviceChild> made;
    if (profile.starts_with("ps"))
    {
        ComPtr<ID3D11PixelShader> pixel;
        result = device->CreatePixelShader(bytes, size, nullptr, pixel.GetAddressOf());
        made = pixel;
    }
    else if (profile.starts_with("vs"))
    {
        ComPtr<ID3D11VertexShader> vertex;
        result = device->CreateVertexShader(bytes, size, nullptr, vertex.GetAddressOf());
        made = vertex;
    }
    else if (profile.starts_with("cs"))
    {
        ComPtr<ID3D11ComputeShader> compute;
        result = device->CreateComputeShader(bytes, size, nullptr, compute.GetAddressOf());
        made = compute;
    }
    if (FAILED(result)) throw std::runtime_error(std::string("The device refused ") + shader.source + ": " + hex(static_cast<std::uint32_t>(result)));

    return made;
}

// One look at one shader, on the worker. The first finds its files; a later one recompiles it once they changed.
void look(Watched& entry)
{
    Shader shader;
    ComPtr<ID3D11Device> device;
    std::vector<fs::path> files;
    std::optional<fs::file_time_type> seen;
    {
        std::lock_guard lock(watchedMutex);
        shader = entry.shader;
        device = entry.device;
        files = entry.files;
        seen = entry.seen;
    }

    const bool first = !seen;
    if (!first)
    {
        const auto latest = newestWrite(files);
        if (!latest || *latest <= *seen) return;
        seen = latest;
        // An editor saves in more than one write: let it finish.
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    ComPtr<ID3D11DeviceChild> fresh;
    try
    {
        const auto code = compileShaderFile(shader, &files);
        // The first look learns the includes only by compiling, so it dates them after: a save during that
        // compile waits for the next one.
        if (first) seen = newestWrite(files);
        else fresh = make(device.Get(), shader, code.Get());
    }
    catch (const std::exception& error)
    {
        if (first) seen = newestWrite(files); // Not again until a file changes.
        else log(std::format("Shader reload: {} kept, {}", shader.source, error.what()));
    }

    std::vector<ComPtr<ID3D11DeviceChild>> made;
    {
        std::lock_guard lock(watchedMutex);
        if (!files.empty()) entry.files = std::move(files);
        if (seen) entry.seen = seen;
        made = entry.made;
    }
    if (!fresh) return;

    {
        std::unique_lock lock(swapMutex);
        for (const auto& original : made) newest[original.Get()] = fresh;
    }
    anySwapped.store(true, std::memory_order_release);
    log(std::format("Shader reload: {} drawn from its saved source, in place of {} shader(s); so far {} shaders bound, {} of them swapped",
        shader.source, made.size(), bound.load(std::memory_order_relaxed), swapped.load(std::memory_order_relaxed)));
}

void watch(std::stop_token stop)
{
    std::mutex sleeping;
    std::condition_variable_any wake;
    while (!stop.stop_requested())
    {
        std::vector<Watched*> entries;
        {
            std::lock_guard lock(watchedMutex);
            for (auto& entry : watched) entries.push_back(&entry);
        }
        for (auto* entry : entries)
        {
            if (stop.stop_requested()) return;
            look(*entry);
        }

        std::unique_lock lock(sleeping);
        wake.wait_for(lock, stop, std::chrono::milliseconds{300}, [] { return false; });
    }
}
}

void watchShader(const Shader& shader, ID3D11Device* device, ID3D11DeviceChild* made)
{
    if (!device || !made) return;

    std::lock_guard lock(watchedMutex);
    auto entry = std::find_if(watched.begin(), watched.end(), [&](const Watched& one) {
        return std::string_view(one.shader.source) == shader.source && std::string_view(one.shader.profile) == shader.profile;
    });
    if (entry == watched.end()) entry = watched.insert(watched.end(), Watched{shader, device, {}, {}, std::nullopt});
    entry->device = device;
    entry->made.emplace_back(made);

    if (!worker.joinable()) worker = std::jthread(watch);
}

void reloadShadersOn(ID3D11DeviceContext* context)
{
    if (!context) return;

    std::lock_guard lock(watchedMutex);
    if (hooked) return;
    hooked = true; // Tried once: a failure is logged, and shaders still work without reloading.
    if (!hookContext(context)) log("Shader reload: the context's Set*Shader could not be hooked; edits to shaders need a restart");
}

void stopShaderReload() noexcept
{
    std::jthread stopping;
    {
        std::lock_guard lock(watchedMutex);
        stopping = std::move(worker);
    }
    // Joined outside the lock: the worker takes it.
    if (stopping.joinable())
    {
        stopping.request_stop();
        stopping.join();
    }

    std::lock_guard lock(watchedMutex);
    watched.clear();
    // MH_Uninitialize, after this, removes the hooks: a later load hooks again.
    hooked = false;
    anySwapped.store(false, std::memory_order_release);
    std::unique_lock swap(swapMutex);
    newest.clear();
}
}
