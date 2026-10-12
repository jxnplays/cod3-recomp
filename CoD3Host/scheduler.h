#pragma once

// Which of the console's six hardware threads a guest thread runs on, and the
// consequence of that: two guest threads on the same one never run at the same
// instant. See scheduler.cpp for why that matters here.

namespace Scheduler
{
    // Called once, when a guest thread starts running guest code.
    void Attach(int hardwareThread);
    void Detach();

    // Moves a thread to another hardware thread. The thread itself picks the
    // change up the next time it takes or hands over its slot, because a slot
    // cannot be released from another thread.
    void Reassign(unsigned long osThreadId, int hardwareThread);

    // Around anything that blocks. A thread waiting for another must not be
    // holding the hardware thread they share.
    void Release();
    void Acquire();

    // For a thread that is spinning rather than blocking: stands aside briefly
    // so anything else on the same hardware thread can run.
    void Checkpoint();

    void SetEnabled(bool enabled);
    void Report();
}
