#pragma once

#include "codeinsight/codeinsight.hpp"

namespace codeinsight {

// Rebuild derived call edges from observations. Compiler edges are preserved;
// repeating this pass on an unchanged model is idempotent.
void resolve_call_targets(SemanticModel& model);

enum class CallTargetCompleteness { Complete, Incomplete, ExternalOnly };

struct CallTargetSet {
    RelationshipID observation;
    CallTargetCompleteness completeness{CallTargetCompleteness::Incomplete};
    std::set<SymbolID> targets;
    // Relationship IDs explain both the declared target and admitted candidates.
    std::set<RelationshipID> evidence;
};

// One result per compiler call observation. Known targets survive incomplete
// value flow; a virtual declared target never proves a complete runtime set.
[[nodiscard]] std::vector<CallTargetSet> summarize_call_targets(const SemanticModel& model);

} // namespace codeinsight
