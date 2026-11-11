#pragma once

// Where a thread was when one of its sections of work took far too long:
// a watchdog thread looks at every watched section that has been running
// for over 100 ms, every 50 ms (up to 16 times a section). Each look takes
// how much processor time the thread used since the section began and, on
// Windows, the process's page faults and the memory free; in a build made
// with GHR_STALL_SAMPLES (cmake -DGHR_STALL_SAMPLES=ON, for chasing a stall;
// antivirus heuristics dislike it in an unsigned exe) also where the thread
// was (its instruction pointer and the return addresses on its stack, as
// module offsets, for a map file to name).
// A section that ends after more than the caller's threshold gets those
// looks back as one line of text. Costs two clock reads a section.

#include <cstdint>
#include <string>

namespace stallprobe
{
    struct Slot;

    // The calling thread's slot for one kind of section (name: for the
    // report). Made on first use; one thread per slot.
    Slot *slot(const char *name);

    void begin(Slot *slot);

    // Ends the section. If it took more than minMs, describes it in
    // report and returns its length in milliseconds; otherwise 0.
    double end(Slot *slot, double minMs, std::string &report);
}
