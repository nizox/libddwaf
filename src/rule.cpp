// Unless explicitly stated otherwise all files in this repository are
// dual-licensed under the Apache-2.0 License or BSD-3-Clause License.
//
// This product includes software developed at Datadog (https://www.datadoghq.com/).
// Copyright 2021 Datadog, Inc.

#include <rule.hpp>

#include <IPWRuleProcessor.h>
#include <PowerWAF.hpp>

#include "Clock.hpp"
#include <log.hpp>

namespace ddwaf
{

bool condition::matchWithTransformer(const ddwaf_object* baseInput, MatchGatherer& gatherer, bool onKey, bool readOnlyArg) const
{
    const bool hasTransformation        = !transformation.empty();
    const bool canRunTransformation     = onKey || (baseInput->type == DDWAF_OBJ_STRING);
    bool transformationWillChangeString = false;

    if (hasTransformation && canRunTransformation)
    {
        // This codepath is shared with the mutable path. The structure can't be const :/
        if (onKey && baseInput != NULL)
        {
            ddwaf_object fakeArg;
            ddwaf_object_stringl_nc(&fakeArg, baseInput->parameterName, baseInput->parameterNameLength);
            transformationWillChangeString = PWTransformer::doesNeedTransform(transformation, &fakeArg);
        }
        else
        {
            transformationWillChangeString = PWTransformer::doesNeedTransform(transformation, (ddwaf_object*) baseInput);
        }
    }

    //If we don't have transformation to perform, or if they're irrelevant, no need to waste time copying and allocating data
    if (!hasTransformation || !canRunTransformation || !transformationWillChangeString)
    {
        if (onKey)
            return processor->doesMatchKey(baseInput, gatherer);
        return processor->doesMatch(baseInput, gatherer);
    }

    ddwaf_object copyInput;
    if (readOnlyArg)
    {
        // Copy the input. If we're running on the key, we copy it in the value as it's functionnaly equivalent
        if (onKey)
        {
            ddwaf_object_stringl(&copyInput, (const char*) baseInput->parameterName, baseInput->parameterNameLength);
        }
        else
        {
            ddwaf_object_stringl(&copyInput, (const char*) baseInput->stringValue, baseInput->nbEntries);
        }
    }
    else
    {
        copyInput = *baseInput;
    }

    //Transform it and pick the pointer to process
    bool transformFailed = false, matched = false;
    for (const PW_TRANSFORM_ID& transform : transformation)
    {
        //
        // Okay, this is going to look weird but it make sense:
        //	- matchInterTransformer mean we need to run the operator on each version of the parameter through the transformer chain
        //	- This mean to run on the original version, then each intermediary representation, and the final one
        //	- The simplest way to do that is actually to run _before_ the transformer, and use the "normal" run at the end
        //	- It is important to realize this run is actually the one _before_ the transformer
        //	- Therefore, if the transformer isn't going to do anything, this run can be skipped
        //

        if (options.matchInterTransformer)
        {
            // Will the transformer modify the parameter? If not, skip the iteration
            if (!PWTransformer::transform(transform, &copyInput, true))
            {
                continue;
            }

            // Run the operator
            matched |= processor->doesMatch(&copyInput, gatherer);

            // If it matched and that we don't want to do multiple runs
            if (matched && !options.keepRunningOnMatch)
            {
                break;
            }
        }

        transformFailed = !PWTransformer::transform(transform, &copyInput);
        if (transformFailed || (copyInput.type == DDWAF_OBJ_STRING && copyInput.nbEntries == 0))
        {
            break;
        }
    }

    //Run the transformed input if options.matchInterTransformer didn't already matched
    if (!matched || options.keepRunningOnMatch)
    {
        const ddwaf_object* paramToUse = transformFailed ? baseInput : &copyInput;
        matched |= processor->doesMatch(paramToUse, gatherer);
    }

    // Otherwise, the caller is in charge of freeing the pointer
    if (readOnlyArg)
    {
        ddwaf_object_free(&copyInput);
    }

    return matched;
}

condition::status condition::_matchTargets(PWRetriever& retriever, const SQPowerWAF::monotonic_clock::time_point& deadline, PWRetManager& retManager) const
{
    PWRetriever::Iterator& iterator = retriever.getIterator(targets);
    retriever.moveIteratorForward(iterator, false);

    if (iterator.isOver())
    {
        //If no BAs for this rule have resolved, we return MISSING_ARG
        //	(that is, unless the processor "match" in this case)
        if (!processor->matchIfMissing())
            return status::missing_arg;

        retManager.recordRuleMatch(processor, MatchGatherer(matchesToGather));
        return status::matched;
    }

    bool matched             = false;
    size_t counter           = 0;
    const bool savingMatches = saveParamOnMatch || !matchesToGather.empty();
    MatchGatherer gather(matchesToGather);

    do
    {
        // Only check the time every 16 runs
        if ((++counter & 0xf) == 0 && deadline <= SQPowerWAF::monotonic_clock::now())
            return status::timeout;

        bool didMatch = retriever.runIterOnLambda(iterator, saveParamOnMatch, [&gather, this](const ddwaf_object* input, DDWAF_OBJ_TYPE type, bool runOnKey, bool isReadOnlyArg) -> bool {
            if ((type & processor->expectedTypes()) == 0)
                return false;

            const uint64_t stringLength = runOnKey ? input->parameterNameLength : input->nbEntries;
            if (type == DDWAF_OBJ_STRING && stringLength < options.minLength)
                return false;

            return matchWithTransformer(input, gather, runOnKey, isReadOnlyArg);
        });

        //If this BA matched, we can stop processing
        if (didMatch)
        {
            DDWAF_TRACE("BA %d did match %s out of parameter value %s",
                        iterator.getActiveTarget(),
                        gather.matchedValue.c_str(),
                        gather.resolvedValue.c_str());
            iterator.argsIterator.getKeyPath(gather.keyPath);
            gather.dataSource  = iterator.getDataSource();
            gather.manifestKey = iterator.getManifestKey();

            retManager.recordRuleMatch(processor, gather);

            // Actually, we can only stop processing if we were not collecting matches for a further filter
            //	If we stopped, it'd open trivial bypasses of the next stage
            if (!savingMatches && !options.keepRunningOnMatch)
                return status::matched;

            retriever.commitMatch(gather);
            matched = true;
        }
    } while (retriever.moveIteratorForward(iterator));

    // Only @exist care about this branch, it's at the end to enable a better report when there is a real value
    if (!matched && processor->matchAnyInput())
    {
        retManager.recordRuleMatch(processor, MatchGatherer(matchesToGather));
        return status::matched;
    }

    //	If at least one resolved, but didn't matched, we return NO_MATCH
    return matched ? status::matched : status::no_match;
}

condition::status condition::performMatching(PWRetriever& retriever, const SQPowerWAF::monotonic_clock::time_point& deadline, PWRetManager& retManager) const
{
    bool matched = false;

    condition::status output = _matchTargets(retriever, deadline, retManager);

    if (matched && (output == status::no_match || output == status::missing_arg))
        return status::matched;

    return output;
}

bool condition::doesUseNewParameters(const PWRetriever& retriever) const
{
    for (const PWManifest::ARG_ID& target : targets)
    {
        if (retriever.isKeyInLastBatch(target))
            return true;
    }

    return false;
}

}
