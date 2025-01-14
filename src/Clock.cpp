// Unless explicitly stated otherwise all files in this repository are
// dual-licensed under the Apache-2.0 License or BSD-3-Clause License.
//
// This product includes software developed at Datadog (https://www.datadoghq.com/).
// Copyright 2021 Datadog, Inc.

#include "Clock.hpp"

#ifdef __linux__

#include <system_error>

#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <log.hpp>
#include <time.h>

namespace SQPowerWAF
{
typedef int (*clock_gettime_t)(clockid_t, timespec* ts);

static clock_gettime_t clock_gettime = &::clock_gettime;

monotonic_clock::time_point monotonic_clock::now() noexcept
{
    struct timespec ts;
    int ret = SQPowerWAF::clock_gettime(CLOCK_MONOTONIC, &ts);
    if (ret < 0)
    {
#ifndef TESTING
        bool expected = false;
        if (warning_issued.compare_exchange_strong(expected, true))
        {
            DDWAF_ERROR("clock_gettime failed. Errno %d}", errno);
        }
        return time_point(std::chrono::seconds(0));
#endif
    }
    return time_point(std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec));
}

std::atomic_bool monotonic_clock::warning_issued {};

struct VdsoInitializer
{
    VdsoInitializer()
    {
        handle = dlopen("linux-vdso.so.1", RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
        if (handle)
        {
            void* p = dlsym(handle, "__vdso_clock_gettime");
            if (p)
            {
                SQPowerWAF::clock_gettime = reinterpret_cast<clock_gettime_t>(p);
            }
        }
    }

    ~VdsoInitializer()
    {
        if (handle)
        {
            SQPowerWAF::clock_gettime = &::clock_gettime;
            dlclose(handle);
        }
    }

private:
    void* handle;
};

static const VdsoInitializer vdso_initializer;
}
#endif
