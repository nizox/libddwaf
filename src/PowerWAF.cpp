// Unless explicitly stated otherwise all files in this repository are
// dual-licensed under the Apache-2.0 License or BSD-3-Clause License.
//
// This product includes software developed at Datadog (https://www.datadoghq.com/).
// Copyright 2021 Datadog, Inc.

#include <algorithm>
#include <unordered_map>

#include "Clock.hpp"
#include <PWProcessor.hpp>
#include <PWRet.hpp>
#include <PWRetriever.hpp>
#include <PowerWAF.hpp>
#include <ddwaf.h>
#include <exception.hpp>
#include <iostream>
#include <log.hpp>
#include <parameter.hpp>
#include <stdexcept>
#include <utils.h>

using namespace ddwaf;
using namespace std::literals;

PowerWAF::PowerWAF(PWManifest&& manifest_, PWRuleManager&& ruleManager_,
                   std::unordered_map<std::string, std::vector<std::string>>&& flows_,
                   const ddwaf_config* config)
    : manifest(std::move(manifest_)),
      ruleManager(std::move(ruleManager_)),
      flows(std::move(flows_))
{
    if (config != nullptr)
    {
        if (config->maxArrayLength != 0)
        {
            maxArrayLength = config->maxArrayLength;
        }

        if (config->maxMapDepth != 0)
        {
            maxMapDepth = config->maxMapDepth;
        }

        if (config->maxTimeStore >= 0)
        {
            maxTimeStore = config->maxTimeStore;
        }
    }
}

template <typename T>
T at(parameter::map& map, const std::string& key)
{
    try
    {
        return map.at(key);
    }
    catch (const std::out_of_range& e)
    {
        throw missing_key(key);
    }
    catch (const bad_cast& e)
    {
        throw invalid_type(key, parameter_traits<T>::name());
    }
}

template <typename T>
T at(parameter::map& map, const std::string& key, const T& default_)
{
    auto it = map.find(key);
    return it == map.end() ? default_ : it->second;
}

static void validateVersion(std::string_view version)
{
    uint16_t major, minor;
    auto& ruleset_version = PowerWAF::ruleset_version;

    int ret = std::sscanf(version.data(), "%hu.%hu", &major, &minor);
    if (ret != 2)
    {
        throw parsing_error("invalid version format");
    }

    if (ruleset_version.major != major)
    {
        DDWAF_ERROR("incompatible ruleset version, required major version %u",
                    ruleset_version.major);
        throw unsupported_version();
    }

    if (ruleset_version.minor < minor)
    {
        DDWAF_WARN("incomplete support for configuration, "
                   "supported version is %u.%u",
                   ruleset_version.major, ruleset_version.minor);
    }
}

static PWRule parseCondition(parameter::map& rule, PWManifest& manifest,
                             std::vector<PW_TRANSFORM_ID>& transformers)
{
    auto operation = at<std::string_view>(rule, "operation");
    auto params    = at<parameter::map>(rule, "parameters");

    parameter::map options;
    std::unique_ptr<IPWRuleProcessor> processor;
    if (operation == "phrase_match")
    {
        auto list = at<parameter::vector>(params, "list");

        std::vector<const char*> patterns;
        std::vector<uint32_t> lengths;

        patterns.reserve(list.size());
        lengths.reserve(list.size());

        for (auto& pattern : list)
        {
            if (pattern.type != DDWAF_OBJ_STRING)
            {
                throw parsing_error("phrase_match list item not a string");
            }

            patterns.push_back(pattern.stringValue);
            lengths.push_back(pattern.nbEntries);
        }

        processor = std::make_unique<PerfMatch>(patterns, lengths);
    }
    else if (operation == "match_regex")
    {
        auto regex = at<std::string>(params, "regex");
        options    = at<parameter::map>(params, "options", options);

        bool case_sensitive = false;
        if (options.find("case_sensitive") != options.end())
        {
            std::string case_opt = options["case_sensitive"];
            std::transform(case_opt.begin(), case_opt.end(), case_opt.begin(), ::tolower);
            if (case_opt == "true")
            {
                case_sensitive = true;
            }
        }
        // TODO support min length
        processor = std::make_unique<RE2Manager>(regex, case_sensitive);
    }
    else
    {
        throw parsing_error("unknown processor: " + std::string(operation));
    }

    std::vector<PWManifest::ARG_ID> targets;
    auto inputs = at<parameter::vector>(params, "inputs");
    for (ddwaf::parameter& address_param : inputs)
    {
        std::string address { address_param };
        if (!manifest.hasTarget(address))
        {
            manifest.insert(address, PWManifest::ArgDetails(address));
        }

        targets.push_back(manifest.getTargetArgID(address));
    }

    return PWRule(std::move(targets), std::move(transformers), std::move(processor));
}

static void parseEvent(parameter::map& event, PWRuleManager& ruleManager,
                       PWManifest& manifest,
                       std::unordered_map<std::string, std::vector<std::string>>& flows)
{
    auto id = at<std::string>(event, "id");
    if (ruleManager.hasRule(id))
    {
        DDWAF_WARN("duplicate rule %s", id.c_str());
        return;
    }

    // Action is a required key, however currently ignored as the only action
    // supported is 'record'
    auto action = at<std::string>(event, "action");

    auto tags = at<parameter::map>(event, "tags");
    auto type = at<std::string>(tags, "type");

    auto& flow = flows[type];

    std::vector<PW_TRANSFORM_ID> rule_transformers;
    auto transformers = at<parameter::vector>(event, "transformers", parameter::vector());
    for (std::string_view transformer : transformers)
    {
        PW_TRANSFORM_ID transform_id = PWTransformer::getIDForString(transformer);
        if (transform_id == PWT_INVALID)
        {
            throw parsing_error("invalid transformer" + std::string(transformer));
        }
        rule_transformers.push_back(transform_id);
    }

    std::vector<PWRule> rules;
    parameter::vector conditions = event.at("conditions");
    for (parameter::map condition : conditions)
    {
        PWRule rule = parseCondition(condition, manifest, rule_transformers);
        rules.push_back(std::move(rule));
    }

    ruleManager.addRule(id, std::move(rules));
    flow.push_back(id);
}

PowerWAF* PowerWAF::fromConfig(const ddwaf_object rules_, const ddwaf_config* config)
{
    parameter::map rules = parameter(rules_);

    std::string_view version = at<std::string_view>(rules, "version");
    validateVersion(version);

    PWRuleManager ruleManager;
    PWManifest manifest;
    std::unordered_map<std::string, std::vector<std::string>> flows;

    auto events = at<parameter::vector>(rules, "events");
    for (parameter::map event : events)
    {
        try
        {
            parseEvent(event, ruleManager, manifest, flows);
        }
        catch (const std::exception& e)
        {
            DDWAF_WARN("%s", e.what());
        }
    }

    if (ruleManager.isEmpty() || flows.empty())
    {
        throw parsing_error("no valid events found");
    }

    return new PowerWAF(std::move(manifest), std::move(ruleManager),
                        std::move(flows), config);
}