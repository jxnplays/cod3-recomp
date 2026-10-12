#include "parallel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    // A batch of pieces. Workers take pieces from it with one atomic add
    // each, so handing out a piece costs nothing worth measuring.
    //
    // Each call makes a batch of its own and never changes it, and a worker
    // holds on to the one it was woken for. That is what keeps a worker
    // that is late leaving one batch from taking pieces of the next with
    // that batch's end or grain: the pieces then came out a different size
    // from the count the caller was waiting on, and the wait either never
    // ended or ended early with rows still being drawn.
    struct Batch
    {
        const std::function<void(int, int)>* work = nullptr;
        std::atomic<int> next{ 0 };
        int end = 0;
        int grain = 1;
        std::atomic<int> pending{ 0 };   // pieces taken and not yet finished
    };

    std::mutex g_mutex;
    std::condition_variable g_wake;
    std::condition_variable g_done;
    std::shared_ptr<Batch> g_current;    // the batch workers are woken for
    uint64_t g_generation = 0;
    std::atomic<bool> g_quit{ false };
    std::vector<std::thread> g_threads;
    int g_width = 1;

    // Takes pieces until the batch is exhausted. Both the workers and the
    // caller run this, so the caller is never idle while others work.
    void Drain(Batch& batch)
    {
        for (;;)
        {
            const int first = batch.next.fetch_add(batch.grain, std::memory_order_acq_rel);
            if (first >= batch.end) return;
            const int last = std::min(first + batch.grain, batch.end);
            (*batch.work)(first, last);
            if (batch.pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_done.notify_all();
            }
        }
    }

    void Worker()
    {
        uint64_t seen = 0;
        for (;;)
        {
            std::shared_ptr<Batch> batch;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_wake.wait(lock, [&] {
                    return g_quit.load() || g_generation != seen;
                });
                if (g_quit.load()) return;
                seen = g_generation;
                batch = g_current;
            }
            if (batch) Drain(*batch);
        }
    }

    void Start()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            // Two cores are left for the title's own threads and the window;
            // the rest take rows. Fewer than one worker means the caller alone.
            const int cores = int(std::thread::hardware_concurrency());
            const int workers = std::max(0, std::min(cores - 2, 16));
            g_width = workers + 1;
            for (int i = 0; i < workers; i++)
                g_threads.emplace_back(Worker);
            for (std::thread& thread : g_threads) thread.detach();
        });
    }
}

int Parallel::Width()
{
    Start();
    return g_width;
}

void Parallel::For(int begin, int end, int grain,
                   const std::function<void(int, int)>& work)
{
    if (begin >= end) return;
    Start();

    const int rows = end - begin;
    if (grain < 1) grain = 1;
    if (g_width == 1 || rows <= grain)
    {
        work(begin, end);
        return;
    }

    // One batch at a time: the caller owns it until it is drained. The
    // rasteriser only ever dispatches from its own thread, so this never
    // contends, and if it ever did the second caller simply waits its turn.
    static std::mutex owner;
    std::lock_guard<std::mutex> own(owner);

    const int pieces = (rows + grain - 1) / grain;
    auto batch = std::make_shared<Batch>();
    batch->work = &work;
    batch->end = end;
    batch->grain = grain;
    // The count of pieces goes in before the first piece is offered, so a
    // worker that takes one can never finish it before it was counted.
    batch->pending.store(pieces, std::memory_order_relaxed);
    batch->next.store(begin, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_current = batch;
        g_generation++;
        g_wake.notify_all();
    }

    Drain(*batch);

    std::unique_lock<std::mutex> lock(g_mutex);
    while (!g_done.wait_for(lock, std::chrono::seconds(2),
               [&] { return batch->pending.load(std::memory_order_acquire) == 0; }))
    {
        // Two seconds with pieces outstanding and nobody working on them
        // is not a slow piece; it is the count and the work disagreeing.
        printf("parallel: waiting on %d of %d pieces, rows %d to %d in %d, next %d\n",
            batch->pending.load(), pieces, begin, end, grain, batch->next.load());
        fflush(stdout);
    }
    // Every piece is done, so no worker will call the work again; the batch
    // itself lives on until the last worker holding it lets go.
    if (g_current == batch) g_current.reset();
}
