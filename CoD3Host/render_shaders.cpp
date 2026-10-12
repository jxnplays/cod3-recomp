#include "render_shaders.h"
#include "render_stats.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <Windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

namespace
{
    using namespace RenderShaders;

    ID3D11Device* g_device = nullptr;

    typedef HRESULT (WINAPI* CompileFunction)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
        ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    CompileFunction g_compile = nullptr;

    // The programs, by handle less one; a deque so an entry never moves,
    // which the worker threads holding a pointer to theirs rely on.
    std::deque<Program> g_programs;
    struct Words { std::vector<uint32_t> words; };
    std::deque<Words> g_microcode;   // alongside, host order, for the translator

    // Hash to handle: open addressing over a power of two.
    std::vector<uint64_t> g_keys;
    std::vector<Handle> g_handles;
    uint32_t g_used = 0;

    Handle g_current[2] = { 0, 0 };
    uint64_t g_currentHash[2] = { 0, 0 };

    std::atomic<uint64_t> g_loads{ 0 }, g_fromDisk{ 0 }, g_compiled{ 0 }, g_failed{ 0 }, g_capturedFailed{ 0 }, g_compileMilliseconds{ 0 };
    std::atomic<uint32_t> g_pending{ 0 };

    std::string g_cacheDirectory;
    std::filesystem::path g_captureDirectory;
    bool g_captureFailed = false;
    uint64_t g_translatorVersion = 0;

    // The same hash the captured files have been named by all along: FNV-1a
    // over the bytes as the console stores them.
    uint64_t HashBytes(const uint8_t* bytes, size_t count)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < count; i++) { hash ^= bytes[i]; hash *= 1099511628211ull; }
        return hash;
    }

    Handle FindHandle(uint64_t hash, size_t& slot)
    {
        const size_t mask = g_keys.size() - 1;
        slot = size_t(hash ^ (hash >> 29)) & mask;
        for (;;)
        {
            if (g_handles[slot] == 0) return 0;
            if (g_keys[slot] == hash) return g_handles[slot];
            slot = (slot + 1) & mask;
        }
    }

    void InsertHandle(uint64_t hash, Handle handle)
    {
        if ((g_used + 1) * 2 > g_keys.size())
        {
            std::vector<uint64_t> keys; std::vector<Handle> handles;
            keys.swap(g_keys); handles.swap(g_handles);
            g_keys.assign(keys.size() * 2, 0);
            g_handles.assign(handles.size() * 2, 0);
            for (size_t i = 0; i < keys.size(); i++)
            {
                if (handles[i] == 0) continue;
                size_t at;
                FindHandle(keys[i], at);
                g_keys[at] = keys[i];
                g_handles[at] = handles[i];
            }
        }
        size_t at;
        FindHandle(hash, at);
        g_keys[at] = hash;
        g_handles[at] = handle;
        g_used++;
    }

    // --- the disk cache ---------------------------------------------------------------

    // One file a program: the translation's metadata and the bytecode.
    struct CacheHeader
    {
        uint32_t magic;            // 'C3SH'
        uint32_t version;          // of this layout
        uint64_t translator;       // the translator's version, and its knobs
        uint32_t colourTargets;
        uint32_t writesDepth;
        uint32_t vertexFetches;
        uint32_t textureFetches;
        uint32_t bytecodeBytes;
        uint32_t hlslBytes;
        uint32_t constantMap;      // entries of the packed constant map, after the fetches
        uint32_t reserved;
    };
    constexpr uint32_t CacheMagic = 0x48533343u;
    constexpr uint32_t CacheVersion = 3;

    std::string CachePath(uint64_t hash, bool pixel)
    {
        char name[96];
        snprintf(name, sizeof(name), "\\%016llx.%s.%08llx.bin", (unsigned long long)hash, pixel ? "ps" : "vs",
            (unsigned long long)(g_translatorVersion & 0xFFFFFFFFull));
        return g_cacheDirectory + name;
    }

    bool ReadCache(Program& program)
    {
        if (g_cacheDirectory.empty()) return false;
        const std::string path = CachePath(program.hash, program.pixel);
        FILE* in = fopen(path.c_str(), "rb");
        if (in == nullptr) return false;
        CacheHeader header{};
        bool ok = fread(&header, sizeof(header), 1, in) == 1 && header.magic == CacheMagic &&
                  header.version == CacheVersion && header.translator == g_translatorVersion &&
                  header.vertexFetches <= 96 && header.textureFetches <= 32 && header.constantMap <= 256 && header.bytecodeBytes < (16u << 20);
        if (ok)
        {
            program.vertexFetches.resize(header.vertexFetches);
            program.textureFetches.resize(header.textureFetches);
            program.constantMap.resize(header.constantMap);
            program.bytecode.resize(header.bytecodeBytes);
            ok = (header.vertexFetches == 0 || fread(program.vertexFetches.data(), sizeof(XenosHlsl::VertexFetch), header.vertexFetches, in) == header.vertexFetches) &&
                 (header.textureFetches == 0 || fread(program.textureFetches.data(), sizeof(XenosHlsl::TextureFetch), header.textureFetches, in) == header.textureFetches) &&
                 (header.constantMap == 0 || fread(program.constantMap.data(), sizeof(uint16_t), header.constantMap, in) == header.constantMap) &&
                 fread(program.bytecode.data(), 1, header.bytecodeBytes, in) == header.bytecodeBytes;
            program.colourTargets = header.colourTargets;
            program.writesDepth = header.writesDepth != 0;
            program.hlslBytes = header.hlslBytes;
        }
        fclose(in);
        if (!ok) { program.bytecode.clear(); return false; }
        g_fromDisk.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void WriteCache(const Program& program)
    {
        if (g_cacheDirectory.empty()) return;
        const std::string path = CachePath(program.hash, program.pixel);
        // Written whole under another name, then renamed, so a run cut
        // short leaves no half file.
        const std::string partial = path + ".part";
        FILE* out = fopen(partial.c_str(), "wb");
        if (out == nullptr) return;
        CacheHeader header{};
        header.magic = CacheMagic;
        header.version = CacheVersion;
        header.translator = g_translatorVersion;
        header.colourTargets = program.colourTargets;
        header.writesDepth = program.writesDepth ? 1 : 0;
        header.vertexFetches = uint32_t(program.vertexFetches.size());
        header.textureFetches = uint32_t(program.textureFetches.size());
        header.bytecodeBytes = uint32_t(program.bytecode.size());
        header.hlslBytes = program.hlslBytes;
        header.constantMap = uint32_t(program.constantMap.size());
        bool written = fwrite(&header, sizeof(header), 1, out) == 1;
        if (!program.vertexFetches.empty()) written = written && fwrite(program.vertexFetches.data(), sizeof(XenosHlsl::VertexFetch), program.vertexFetches.size(), out) == program.vertexFetches.size();
        if (!program.textureFetches.empty()) written = written && fwrite(program.textureFetches.data(), sizeof(XenosHlsl::TextureFetch), program.textureFetches.size(), out) == program.textureFetches.size();
        if (!program.constantMap.empty()) written = written && fwrite(program.constantMap.data(), sizeof(uint16_t), program.constantMap.size(), out) == program.constantMap.size();
        written = written && fwrite(program.bytecode.data(), 1, program.bytecode.size(), out) == program.bytecode.size();
        fclose(out);
        if (written) MoveFileExA(partial.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        else DeleteFileA(partial.c_str());
    }

    // --- the capture -------------------------------------------------------------------

    // The microcode as the console stores it, for the next run's precompile
    // and for the tools. Once per program, and never on the draw path.
    void Capture(uint64_t hash, bool pixel, const uint8_t* bytes, size_t count)
    {
        if (g_captureFailed || g_captureDirectory.empty()) return;
        char name[64];
        snprintf(name, sizeof(name), "%s_%016llx.bin", pixel ? "ps" : "vs", (unsigned long long)hash);
        const std::filesystem::path path = g_captureDirectory / name;
        std::error_code error;
        if (std::filesystem::exists(path, error)) return;
        std::filesystem::create_directories(g_captureDirectory, error);
        if (error) { g_captureFailed = true; return; }
        FILE* file = fopen(path.string().c_str(), "wb");
        if (file == nullptr) { g_captureFailed = true; return; }
        fwrite(bytes, 1, count, file);
        fclose(file);
    }

    // --- the workers ---------------------------------------------------------------------

    std::mutex g_queueMutex;
    std::condition_variable g_queueWake;
    // A job carries pointers: the deques the entries live in are appended
    // by the command thread while the workers run, and an entry once made
    // never moves.
    struct Job { Program* program; const std::vector<uint32_t>* words; };
    std::deque<Job> g_queue;
    std::vector<std::thread> g_workers;

    bool MakeShaderObject(Program& program)
    {
        HRESULT hr;
        if (program.pixel)
        {
            ID3D11PixelShader* shader = nullptr;
            hr = g_device->CreatePixelShader(program.bytecode.data(), program.bytecode.size(), nullptr, &shader);
            program.shader = shader;
        }
        else
        {
            ID3D11VertexShader* shader = nullptr;
            hr = g_device->CreateVertexShader(program.bytecode.data(), program.bytecode.size(), nullptr, &shader);
            program.shader = shader;
        }
        return SUCCEEDED(hr) && program.shader != nullptr;
    }

    void Build(const Job& job)
    {
        Program& program = *job.program;
        const char* what = program.pixel ? "pixel program" : "vertex program";
        if (ReadCache(program))
        {
            RenderStats::NoteProgram(true);
            program.state.store(MakeShaderObject(program) ? State::Ready : State::Failed, std::memory_order_release);
            return;
        }

        program.state.store(State::Translating, std::memory_order_relaxed);
        const XenosHlsl::Translation translation = XenosHlsl::Translate(*job.words, program.pixel);
        if (!translation.ok)
        {
            // The stream's programs are said here; a captured one is said
            // when a draw wants it, which most of the failures never are
            // (the title loads programs it does not draw with), and counted
            // for the report.
            program.problem = translation.problem;
            if (program.captured) g_capturedFailed.fetch_add(1, std::memory_order_relaxed);
            else printf("render: %s %016llx (%zu dwords) could not be translated: %s\n", what, (unsigned long long)program.hash, job.words->size(), translation.problem.c_str());
            g_failed.fetch_add(1, std::memory_order_relaxed);
            program.state.store(State::Failed, std::memory_order_release);
            return;
        }
        program.vertexFetches = translation.vertexFetches;
        program.textureFetches = translation.textureFetches;
        program.colourTargets = translation.colourTargets;
        program.writesDepth = translation.writesDepth;
        program.hlslBytes = uint32_t(translation.hlsl.size());
        program.constantMap = translation.constantMap;

        program.state.store(State::Compiling, std::memory_order_relaxed);
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const auto started = std::chrono::steady_clock::now();
        const HRESULT hr = g_compile(translation.hlsl.data(), translation.hlsl.size(), what, nullptr, nullptr, "main",
            program.pixel ? "ps_5_0" : "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        const double ms = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count() / 1000.0;
        g_compileMilliseconds.fetch_add(uint64_t(ms), std::memory_order_relaxed);
        if (ms > 500.0) printf("render: a %s of %zu bytes took %.0f ms to compile\n", what, translation.hlsl.size(), ms);

        // COD3_DUMPHLSL=directory keeps every translated program.
        static const char* const dumpTo = getenv("COD3_DUMPHLSL");
        if (dumpTo != nullptr)
        {
            char name[512];
            snprintf(name, sizeof(name), "%s/%s_%016llx%s.hlsl", dumpTo, program.pixel ? "ps" : "vs",
                (unsigned long long)program.hash, FAILED(hr) ? "-failed" : "");
            if (FILE* out = fopen(name, "wb")) { fwrite(translation.hlsl.data(), 1, translation.hlsl.size(), out); fclose(out); }
        }
        if (FAILED(hr))
        {
            g_failed.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> announced{ 0 };
            const int number = announced.fetch_add(1);
            if (number < 8)
            {
                printf("render: %s %016llx did not compile: %s\n", what, (unsigned long long)program.hash,
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
                if (number < 2) printf("%s\n", translation.hlsl.c_str());
                fflush(stdout);
            }
            if (errors) errors->Release();
            program.state.store(State::Failed, std::memory_order_release);
            return;
        }
        if (errors) errors->Release();
        g_compiled.fetch_add(1, std::memory_order_relaxed);
        RenderStats::NoteProgram(false);
        program.bytecode.assign(static_cast<const uint8_t*>(code->GetBufferPointer()),
            static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
        code->Release();
        WriteCache(program);
        program.state.store(MakeShaderObject(program) ? State::Ready : State::Failed, std::memory_order_release);
    }

    void Worker()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(g_queueMutex);
                g_queueWake.wait(lock, [] { return !g_queue.empty(); });
                job = g_queue.front();
                g_queue.pop_front();
            }
            Build(job);
            g_pending.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void Enqueue(Program* program, const std::vector<uint32_t>* words, bool front)
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (g_workers.empty())
        {
            unsigned count = std::thread::hardware_concurrency() / 2;
            if (count < 2) count = 2;
            if (count > 6) count = 6;
            for (unsigned i = 0; i < count; i++) { g_workers.emplace_back(Worker); g_workers.back().detach(); }
        }
        // The stream's programs go first: a draw is waiting on them, and
        // the precompile's can take their turn after.
        const Job job{ program, words };
        if (front) g_queue.push_front(job); else g_queue.push_back(job);
        g_pending.fetch_add(1, std::memory_order_relaxed);
        g_queueWake.notify_one();
    }

    // A program from words in the console's byte order: found, or made and
    // queued for building.
    Handle Intern(bool pixel, const uint8_t* bytes, uint32_t sizeDwords, bool fromStream)
    {
        const uint64_t hash = HashBytes(bytes, size_t(sizeDwords) * 4);
        size_t slot;
        if (const Handle found = FindHandle(hash, slot)) return found;

        g_programs.emplace_back();
        g_microcode.emplace_back();
        const Handle handle = Handle(g_programs.size());
        Program& program = g_programs.back();
        program.hash = hash;
        program.pixel = pixel;
        program.captured = !fromStream;
        std::vector<uint32_t>& words = g_microcode.back().words;
        words.resize(sizeDwords);
        for (uint32_t i = 0; i < sizeDwords; i++)
            words[i] = (uint32_t(bytes[i * 4]) << 24) | (uint32_t(bytes[i * 4 + 1]) << 16) | (uint32_t(bytes[i * 4 + 2]) << 8) | bytes[i * 4 + 3];
        InsertHandle(hash, handle);

        // COD3_D3DSKIPVS=hex,hex: the vertex programs whose microcode hash
        // starts so have their draws left out, to see what is under them.
        static const char* const skips = getenv("COD3_D3DSKIPVS");
        if (skips != nullptr && !pixel)
        {
            char name[24];
            snprintf(name, sizeof(name), "%016llx", (unsigned long long)hash);
            for (const char* at = skips; *at != 0;)
            {
                const char* end = strchr(at, ',');
                const size_t length = end != nullptr ? size_t(end - at) : strlen(at);
                if (length > 0 && strncmp(name, at, length) == 0) program.skipped = true;
                at = end != nullptr ? end + 1 : at + length;
            }
            if (program.skipped) printf("render: vertex program %s is skipped\n", name);
        }
        if (fromStream) Capture(hash, pixel, bytes, size_t(sizeDwords) * 4);
        Enqueue(&program, &words, fromStream);
        return handle;
    }
}

void RenderShaders::Initialize(ID3D11Device* device)
{
    g_device = device;
    g_keys.assign(1024, 0);
    g_handles.assign(1024, 0);
    g_translatorVersion = XenosHlsl::Version();

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler != nullptr) g_compile = reinterpret_cast<CompileFunction>(GetProcAddress(compiler, "D3DCompile"));
    if (g_compile == nullptr) printf("render: d3dcompiler_47.dll is not available; only cached programs can run\n");

    if (getenv("COD3_NOSHADERCACHE") == nullptr)
    {
        if (const char* local = getenv("LOCALAPPDATA"))
        {
            std::string directory = std::string(local) + "\\CoD3Recomp";
            CreateDirectoryA(directory.c_str(), nullptr);
            directory += "\\shaders";
            CreateDirectoryA(directory.c_str(), nullptr);
            g_cacheDirectory = directory;
        }
    }
    g_captureDirectory = std::filesystem::current_path() / "shaders";
}

RenderState::Handle RenderShaders::Loaded(bool pixel, const uint8_t* words, uint32_t sizeDwords)
{
    g_loads.fetch_add(1, std::memory_order_relaxed);
    if (sizeDwords == 0 || sizeDwords > 64 * 1024 || words == nullptr) return g_current[pixel ? 1 : 0];
    // The same program again, which the title does between most of its
    // draws: one hash and one compare.
    return Loaded(pixel, words, sizeDwords, HashBytes(words, size_t(sizeDwords) * 4));
}

RenderState::Handle RenderShaders::Loaded(bool pixel, const uint8_t* words, uint32_t sizeDwords, uint64_t hash)
{
    if (sizeDwords == 0 || sizeDwords > 64 * 1024 || words == nullptr) return g_current[pixel ? 1 : 0];
    const int stage = pixel ? 1 : 0;
    if (g_current[stage] != 0 && g_currentHash[stage] == hash) return g_current[stage];
    size_t slot;
    Handle handle = FindHandle(hash, slot);
    if (handle == 0) handle = Intern(pixel, words, sizeDwords, true);
    g_current[stage] = handle;
    g_currentHash[stage] = hash;
    return handle;
}

RenderState::Handle RenderShaders::Current(bool pixel) { return g_current[pixel ? 1 : 0]; }

const RenderShaders::Program* RenderShaders::Get(Handle handle)
{
    return handle != 0 && handle <= g_programs.size() ? &g_programs[handle - 1] : nullptr;
}

uint64_t RenderShaders::HashMicrocode(const uint8_t* words, uint32_t sizeDwords) { return HashBytes(words, size_t(sizeDwords) * 4); }

uint64_t RenderShaders::HashOf(Handle handle)
{
    const Program* program = Get(handle);
    return program != nullptr ? program->hash : 0;
}

void RenderShaders::Precompile()
{
    if (getenv("COD3_NOPRECOMPILE") != nullptr || g_compile == nullptr) return;
    std::error_code error;
    if (!std::filesystem::is_directory(g_captureDirectory, error)) return;
    uint32_t queued = 0, cached = 0;
    for (const auto& entry : std::filesystem::directory_iterator(g_captureDirectory, error))
    {
        const std::string name = entry.path().filename().string();
        if (name.size() != 23 || name.compare(19, 4, ".bin") != 0) continue;
        const bool pixel = name.compare(0, 3, "ps_") == 0;
        if (!pixel && name.compare(0, 3, "vs_") != 0) continue;
        // Named by the hash: one in the disk cache already is read from
        // there by the worker and made into a shader object now, so a
        // level's first draws find their programs ready instead of
        // waiting on the driver's own compile of them.
        const uint64_t hash = strtoull(name.c_str() + 3, nullptr, 16);
        if (!g_cacheDirectory.empty() && std::filesystem::exists(CachePath(hash, pixel), error)) cached++;
        FILE* file = fopen(entry.path().string().c_str(), "rb");
        if (file == nullptr) continue;
        std::vector<uint8_t> bytes;
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        if (size > 0 && size < (256 << 10) && (size % 4) == 0)
        {
            bytes.resize(size_t(size));
            if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
        }
        fclose(file);
        if (bytes.empty()) continue;
        Intern(pixel, bytes.data(), uint32_t(bytes.size() / 4), false);
        queued++;
    }
    if (queued != 0)
        printf("render: %u captured programs made ready in the background, %u of them from the disk cache\n", queued, cached);
}

RenderShaders::Statistics RenderShaders::Stats()
{
    Statistics s;
    s.loads = g_loads.load(std::memory_order_relaxed);
    s.programs = g_programs.size();
    s.fromDisk = g_fromDisk.load(std::memory_order_relaxed);
    s.compiled = g_compiled.load(std::memory_order_relaxed);
    s.failed = g_failed.load(std::memory_order_relaxed);
    s.compileMilliseconds = g_compileMilliseconds.load(std::memory_order_relaxed);
    s.pending = g_pending.load(std::memory_order_relaxed);
    return s;
}

void RenderShaders::Report()
{
    const Statistics s = Stats();
    if (s.loads == 0) return;
    printf("render: %llu program uploads, %llu distinct programs, %llu from the disk cache, %llu compiled (%llu failed, %.2f s), %u building\n",
        (unsigned long long)s.loads, (unsigned long long)s.programs, (unsigned long long)s.fromDisk,
        (unsigned long long)s.compiled, (unsigned long long)s.failed, s.compileMilliseconds / 1000.0, s.pending);
    // Once: the report comes every second when the statistics are on.
    static uint64_t said = 0;
    const uint64_t captured = g_capturedFailed.load(std::memory_order_relaxed);
    if (captured != said && g_pending.load(std::memory_order_relaxed) == 0 && (said = captured) != 0)
        printf("render: %llu of the captured programs could not be translated; a draw that wants one says so\n", (unsigned long long)captured);
}
