// trace_ring.h: STUB. Always-on event ring buffer.
//
// This ring records continuously from process start. Probes query for a
// window of interest. They NEVER arm a trace and then run a workload. If the
// event you need isn't recorded, extend the ring by adding the new event
// class to the always-on capture path, not the probe.

#pragma once

namespace gbarecomp::debug {

class TraceRing {
public:
    TraceRing();
    ~TraceRing();
};

}  // namespace gbarecomp::debug
