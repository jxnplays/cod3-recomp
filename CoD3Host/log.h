#pragma once

// The log: what the program prints also goes to CoD3.log beside the
// executable, so that a run started from Explorer leaves something behind.
// A hang is the case that matters: the watchdog reports where every thread
// stands after five seconds without a frame, and the console that report
// was printed to is gone the moment the frozen game is closed.
namespace Log
{
    // Called first thing. Does nothing when the output already goes to a
    // file or a pipe (a script's run, which keeps its own), or when
    // COD3_LOG=0. The last run's log is kept as CoD3.previous.log, so
    // starting the game again after a freeze does not wipe the report.
    void Start();

    // Gives the log a moment to take what is still on its way, before the
    // process ends.
    void Finish();
}
