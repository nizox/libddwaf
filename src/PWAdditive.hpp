// Unless explicitly stated otherwise all files in this repository are
// dual-licensed under the Apache-2.0 License or BSD-3-Clause License.
//
// This product includes software developed at Datadog (https://www.datadoghq.com/).
// Copyright 2021 Datadog, Inc.

#ifndef PWAdditive_hpp
#define PWAdditive_hpp

#include <memory>

#include <PWProcessor.hpp>
#include <PowerWAF.hpp>
#include <ddwaf.h>
#include <utils.h>

class PWAdditive
{
    std::shared_ptr<PowerWAF> wafReference;
    const PowerWAF* wafHandle;

    std::vector<ddwaf_object> argCache;

    PWRetriever retriever;
    PWProcessor processor;
    ddwaf_object_free_fn obj_free;

public:
    PWAdditive(std::shared_ptr<PowerWAF>);
    PWAdditive(const ddwaf_handle, ddwaf_object_free_fn free_fn);

    PWAdditive(const PWAdditive&) = delete;

    ddwaf_result run(ddwaf_object, uint64_t);

    void flushCaches();

#ifdef TESTING
    FRIEND_TEST(TestPWProcessor, TestCache);
    FRIEND_TEST(TestPWManifest, TestUnknownArgID);
#endif
};

#endif /* PWAdditive_hpp */
