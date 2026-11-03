#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace gsr_collection {
struct BuildResources {
    std::uint64_t ram = 0;
    unsigned cpus = 0;
};

inline BuildResources build_resources() {
#ifdef _WIN32
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (!GlobalMemoryStatusEx(&memory)) return {};
    return {memory.ullTotalPhys, std::thread::hardware_concurrency()};
#else
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (pages <= 0 || size <= 0 || cpus <= 0) return {};
    return {std::uint64_t(pages) * std::uint64_t(size), unsigned(cpus)};
#endif
}

// Preserve the player builder's existing 700 MiB allowance per worker.
inline int player_build_jobs(BuildResources resources, bool reduced) {
    const auto ram = reduced ? resources.ram / 2 : resources.ram;
    const auto cpus = reduced ? resources.cpus / 2 : resources.cpus;
    return int(std::max<std::uint64_t>(1, std::min<std::uint64_t>(cpus, ram / (700ull << 20))));
}

#ifdef _WIN32
inline bool build_limits_active(BuildResources resources) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory{};
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
    return resources.ram && QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
        &memory, sizeof(memory), nullptr) && QueryInformationJobObject(nullptr,
        JobObjectCpuRateControlInformation, &cpu, sizeof(cpu), nullptr) &&
        (memory.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) &&
        memory.JobMemoryLimit && memory.JobMemoryLimit <= resources.ram / 2 &&
        (cpu.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE) &&
        (cpu.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) && cpu.CpuRate && cpu.CpuRate <= 50 * 100;
}
#else
// systemd properties alone do not prove that controllers are delegated.
// Refuse to build unless the kernel's actual group limits are present.
inline bool build_limits_active(BuildResources resources) {
    std::ifstream membership("/proc/self/cgroup");
    std::string line;
    while (std::getline(membership, line)) {
        if (line.rfind("0::/", 0) != 0) continue;
        auto group = std::filesystem::path("/sys/fs/cgroup") / line.substr(4);
        std::ifstream cpu(group / "cpu.max"), memory(group / "memory.max");
        std::uint64_t quota = 0, period = 0, bytes = 0;
        if (!(cpu >> quota >> period) || !(memory >> bytes)) return false;
        return resources.cpus && resources.ram && period && quota && bytes &&
            static_cast<long double>(quota) / period <= resources.cpus / 2.0L &&
            bytes <= resources.ram / 2;
    }
    return false;
}
#endif
} // namespace gsr_collection
