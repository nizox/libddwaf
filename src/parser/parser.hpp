// Unless explicitly stated otherwise all files in this repository are
// dual-licensed under the Apache-2.0 License or BSD-3-Clause License.
//
// This product includes software developed at Datadog (https://www.datadoghq.com/).
// Copyright 2021 Datadog, Inc.

#ifndef PARSER_HPP
#define PARSER_HPP

#include <PWManifest.h>
#include <PWRetriever.hpp>
#include <PWRuleManager.hpp>
#include <parameter.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace ddwaf::parser
{

void parse(parameter ruleset, PWRuleManager& ruleManager, PWManifest& manifest,
           std::unordered_map<std::string, std::vector<std::string>>& flows);

}

#endif