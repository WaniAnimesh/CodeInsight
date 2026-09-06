#pragma once
#include "codeinsight/codeinsight.hpp"

namespace codeinsight {

enum class CandidateConfidence { Low, Medium, High };
struct AnalysisIssue { std::string code; std::string message; };
struct AnalysisReadiness {
    bool parse_complete{};
    bool target_topology_authoritative{};
    bool roots_authoritative{};
    bool cross_configuration_bindings_complete{};
    bool project_calls_resolved{};
    bool indirect_dispatch_resolved{};
    std::vector<AnalysisIssue> issues;
    [[nodiscard]] bool high_confidence_ready() const noexcept {
        return parse_complete && target_topology_authoritative && roots_authoritative &&
            cross_configuration_bindings_complete && project_calls_resolved && indirect_dispatch_resolved && issues.empty();
    }
};
struct DeadCodeCandidate {
    SymbolID symbol;
    std::optional<LogicalSymbolID> logical_symbol;
    std::vector<SymbolID> variants;
    CandidateConfidence confidence{CandidateConfidence::Low};
    std::string reason;
};

[[nodiscard]] std::vector<DeadCodeCandidate> analyze_dead_code(const SemanticModel& model);
[[nodiscard]] AnalysisReadiness assess_dead_code_readiness(const SemanticModel& model);
[[nodiscard]] std::string export_dead_code_candidates(const SemanticModel& model, bool json_lines = false);
[[nodiscard]] std::string_view to_string(CandidateConfidence value);

} // namespace codeinsight
