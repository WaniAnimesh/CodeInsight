#include "codeinsight/analysis/dead_code.hpp"

#include <queue>
#include <set>
#include <sstream>
#include <tuple>

namespace codeinsight {

std::string_view to_string(CandidateConfidence value) {
    switch (value) {
    case CandidateConfidence::Low: return "Low";
    case CandidateConfidence::Medium: return "Medium";
    case CandidateConfidence::High: return "High";
    }
    return "Unknown";
}

namespace {

bool callable(SymbolKind kind){return kind==SymbolKind::Function||kind==SymbolKind::Method||kind==SymbolKind::Constructor||kind==SymbolKind::Destructor||kind==SymbolKind::ConversionFunction||kind==SymbolKind::OperatorFunction||kind==SymbolKind::FunctionTemplate||kind==SymbolKind::Lambda;}

struct ModelIndex {
    std::set<SymbolID> definitions;
    std::set<SymbolID> explicit_definitions;
    std::set<std::string> definition_usrs;
    std::map<SymbolID,std::vector<const SymbolOccurrence*>> occurrences_by_symbol;
    std::map<TranslationUnitID,std::set<BuildTargetID>> targets_by_tu;

    explicit ModelIndex(const SemanticModel& model){
        for(const auto&[_,occurrence]:model.occurrences){
            occurrences_by_symbol[occurrence.symbol].push_back(&occurrence);
            if(occurrence.role==OccurrenceRole::Definition){definitions.insert(occurrence.symbol);if(!occurrence.implicit)explicit_definitions.insert(occurrence.symbol);}
        }
        for(const auto& id:definitions)if(const auto symbol=model.symbols.find(id);symbol!=model.symbols.end()&&!symbol->second.usr.empty())definition_usrs.insert(symbol->second.usr);
        for(const auto&[_,membership]:model.target_memberships)targets_by_tu[membership.tu].insert(membership.target);
    }
};

std::string json_escape(std::string_view value){std::string result;for(const char c:value){if(c=='\\')result+="\\\\";else if(c=='\"')result+="\\\"";else if(c=='\n')result+="\\n";else if(c=='\r')result+="\\r";else result.push_back(c);}return result;}

bool traversed(RelationshipKind kind){
    return kind==RelationshipKind::Calls||kind==RelationshipKind::Constructs||kind==RelationshipKind::Destroys||
        kind==RelationshipKind::Contains||kind==RelationshipKind::Instantiates||kind==RelationshipKind::Specializes;
}

struct Projection {
    std::map<SymbolID,std::set<SymbolID>> graph;
    std::set<SymbolID> reachable;
};

bool target_graph_authoritative(const SemanticModel& model){
    if(model.build_targets.empty())return false;for(const auto&[_,target]:model.build_targets)if(!target.topology_authoritative)return false;
    std::set<BuildTargetID> closed;std::queue<BuildTargetID> pending;for(const auto&[id,target]:model.build_targets)if(target.kind==BuildTargetKind::Executable||target.kind==BuildTargetKind::Test){closed.insert(id);pending.push(id);}if(pending.empty())return false;
    while(!pending.empty()){const auto consumer=pending.front();pending.pop();for(const auto&[_,dependency]:model.target_dependencies)if(dependency.consumer==consumer&&closed.insert(dependency.dependency).second)pending.push(dependency.dependency);}
    return closed.size()==model.build_targets.size();
}

Projection project_reachability(const SemanticModel& model,const ModelIndex& index){
    Projection projection;
    for(const auto&[_,edge]:model.relationships)if(edge.target_symbol&&traversed(edge.kind))projection.graph[edge.source].insert(*edge.target_symbol);
    for(const auto&[_,logical]:model.logical_symbols)for(const auto& source:logical.variants)for(const auto& target:logical.variants)if(source!=target)projection.graph[source].insert(target);
    // libclang does not consistently expose clang_getSpecializedCursorTemplate for
    // implicitly instantiated function templates.  Preserve the source-generating
    // primary whenever a concrete function with the same qualified name is reached.
    // This is deliberately one-way and conservative: it may retain an overloaded
    // primary, but it can never make a live primary appear dead.
    std::map<std::string,std::vector<SymbolID>> template_primaries;
    for(const auto&[id,symbol]:model.symbols)if(symbol.kind==SymbolKind::FunctionTemplate&&symbol.templ.is_primary&&!symbol.usr.empty())template_primaries[symbol.qualified_name].push_back(id);
    for(const auto&[id,symbol]:model.symbols)if(symbol.kind==SymbolKind::Function&&!symbol.usr.empty()&&symbol.usr.find("@F@")!=std::string::npos){
        if(const auto found=template_primaries.find(symbol.qualified_name);found!=template_primaries.end())for(const auto& primary:found->second)projection.graph[id].insert(primary);
    }
    // Member definitions of class-template primaries, partial specializations,
    // and concrete instantiations have different USRs even though the callable
    // suffix is identical.  Retain the whole template source family whenever
    // any member is reached.  The qualified name plus complete function suffix
    // preserves overload distinctions.
    std::map<std::string,std::vector<SymbolID>> member_template_families;
    for(const auto&[id,symbol]:model.symbols){const auto function=symbol.usr.rfind("@F@");if(function==std::string::npos)continue;const auto key=symbol.qualified_name+'|'+symbol.usr.substr(function);member_template_families[key].push_back(id);}
    for(const auto&[_,family]:member_template_families){bool templated{};for(const auto& id:family){const auto& usr=model.symbols.at(id).usr;templated|=usr.find("@ST>")!=std::string::npos||usr.find("@SP>")!=std::string::npos;}if(!templated)continue;for(const auto& source:family)for(const auto& target:family)if(source!=target)projection.graph[source].insert(target);}
    std::queue<SymbolID> pending;
    for(const auto&[_,root]:model.entry_roots)if(projection.reachable.insert(root.symbol).second)pending.push(root.symbol);
    // Until visibility/export provenance is complete, externally linked source
    // definitions remain open-world API roots even with authoritative link
    // topology. This prevents a closed set of in-repository executables from
    // making a reusable library API look dead.
    for(const auto&[id,symbol]:model.symbols)if(callable(symbol.kind)&&index.definitions.contains(id)&&symbol.visibility!=Visibility::Hidden&&(symbol.linkage==Linkage::External||symbol.linkage==Linkage::UniqueExternal)&&projection.reachable.insert(id).second)pending.push(id);
    while(!pending.empty()){
        const auto source=pending.front();pending.pop();const auto next=projection.graph.find(source);if(next==projection.graph.end())continue;
        for(const auto& target:next->second)if(projection.reachable.insert(target).second)pending.push(target);
    }
    return projection;
}

std::string group_key(const Symbol& symbol){return symbol.logical_symbol?"logical:"+symbol.logical_symbol->value:"symbol:"+symbol.id.value;}

void add_issue(AnalysisReadiness& result,std::string code,std::string message){result.issues.push_back({std::move(code),std::move(message)});}

AnalysisReadiness readiness(const SemanticModel& model,const Projection& projection,const ModelIndex& index){
    AnalysisReadiness result;
    result.parse_complete=model.completeness.total_tus>0&&model.completeness.complete_tus==model.completeness.total_tus;
    if(!result.parse_complete)add_issue(result,"coverage.incomplete","one or more translation units are incomplete");

    result.target_topology_authoritative=target_graph_authoritative(model);
    if(!result.target_topology_authoritative)add_issue(result,"target.topology-unproven","build-target membership or closure is not authoritative");

    result.roots_authoritative=!model.entry_roots.empty();
    if(!result.roots_authoritative)add_issue(result,"roots.missing","no entry roots are available");

    std::map<BuildTargetID,std::set<SymbolID>> process_entries,target_roots;
    for(const auto&[_,root]:model.entry_roots){
        const auto occurrences=index.occurrences_by_symbol.find(root.symbol);if(occurrences==index.occurrences_by_symbol.end())continue;
        for(const auto* occurrence:occurrences->second)if(occurrence->role==OccurrenceRole::Definition){
            const auto revision=model.revisions.find(occurrence->revision);if(revision==model.revisions.end())continue;
            if(const auto found=index.targets_by_tu.find(revision->second.tu);found!=index.targets_by_tu.end())for(const auto& target:found->second){target_roots[target].insert(root.symbol);if(root.kind==EntryRootKind::ProcessEntry)process_entries[target].insert(root.symbol);}
        }
    }
    for(const auto&[target,entries]:process_entries)if(entries.size()>1){result.target_topology_authoritative=false;add_issue(result,"target.multiple-process-entries","one target contains multiple distinct process-entry symbols: "+target.value);}
    for(const auto&[target_id,target]:model.build_targets)if((target.kind==BuildTargetKind::Executable||target.kind==BuildTargetKind::Test)&&target_roots[target_id].empty()){result.roots_authoritative=false;add_issue(result,"roots.target-missing","an executable or test target has no target-owned entry root: "+target.name);}

    result.cross_configuration_bindings_complete=true;
    for(const auto&[_,edge]:model.relationships){
        if(edge.kind!=RelationshipKind::Calls||!edge.target_symbol||edge.resolution==ResolutionStatus::Unresolved)continue;
        if(index.definitions.contains(*edge.target_symbol))continue;
        const auto target=model.symbols.find(*edge.target_symbol);if(target==model.symbols.end())continue;
        bool bridged{};
        if(target->second.logical_symbol)if(const auto logical=model.logical_symbols.find(*target->second.logical_symbol);logical!=model.logical_symbols.end())for(const auto& variant:logical->second.variants)bridged|=index.definitions.contains(variant);
        if(bridged)continue;
        // A declaration-only target is not itself a binding defect: pure
        // virtual methods and intentionally external APIs commonly have no
        // source definition. Diagnose only the dangerous case this readiness
        // gate is meant to catch—a compatible same-USR definition exists in
        // the model but logical projection failed to connect it.
        const bool stranded_definition=!target->second.usr.empty()&&index.definition_usrs.contains(target->second.usr);
        if(stranded_definition){result.cross_configuration_bindings_complete=false;add_issue(result,"binding.declaration-only-target","a resolved call targets a declaration while a same-USR definition is stranded outside its logical group: "+target->second.qualified_name);}
    }

    result.project_calls_resolved=true;result.indirect_dispatch_resolved=true;
    std::set<RelationshipID> completed_indirect_calls;for(const auto&[_,summary]:model.indirect_call_summaries)if(summary.complete&&!summary.storage_escapes)completed_indirect_calls.insert(summary.call_observation);
    using BindingKey=std::tuple<SymbolID,std::string,TranslationUnitRevisionID,FileVersionID,std::uint32_t>;
    std::set<BindingKey> completed_logical_calls;for(const auto&[_,edge]:model.relationships)if(edge.kind==RelationshipKind::Calls&&edge.origin==EvidenceOrigin::DerivedLogicalEquivalence&&edge.target_symbol)completed_logical_calls.emplace(edge.source,edge.observed_target_usr,edge.observed_in,edge.evidence.file,edge.evidence.begin_offset);
    std::size_t unclassified_reachable{};
    std::size_t unresolved_project{},unresolved_indirect{};
    for(const auto&[edge_id,edge]:model.relationships){
        if(edge.kind!=RelationshipKind::Calls||edge.resolution!=ResolutionStatus::Unresolved||!projection.reachable.contains(edge.source))continue;
        if(completed_indirect_calls.contains(edge_id))continue;
        if(edge.target_domain==TargetDomain::Project&&completed_logical_calls.contains(BindingKey{edge.source,edge.observed_target_usr,edge.observed_in,edge.evidence.file,edge.evidence.begin_offset}))continue;
        if(edge.target_domain==TargetDomain::Project){result.project_calls_resolved=false;++unresolved_project;}
        else if(edge.target_domain==TargetDomain::Indirect||edge.dispatch==DispatchKind::Dynamic){result.indirect_dispatch_resolved=false;++unresolved_indirect;}
        else if(edge.target_domain==TargetDomain::Unknown){++unclassified_reachable;result.project_calls_resolved=false;}
    }
    if(unresolved_project)add_issue(result,"calls.project-unresolved",std::to_string(unresolved_project)+" reachable project-owned call observation(s) are unresolved");
    if(unresolved_indirect)add_issue(result,"calls.indirect-unresolved",std::to_string(unresolved_indirect)+" reachable indirect call observation(s) have no complete target set");
    if(unclassified_reachable)add_issue(result,"calls.unclassified-unresolved",std::to_string(unclassified_reachable)+" reachable call observation(s) cannot be classified");
    return result;
}

std::string readiness_reason(const AnalysisReadiness& state){
    if(state.high_confidence_ready())return "unreachable from all authoritative roots in a complete, closed, fully resolved logical graph";
    std::string reason="unreachable from known roots; confidence gated by ";
    for(std::size_t index{};index<state.issues.size();++index){if(index)reason+=", ";reason+=state.issues[index].code;}
    return reason;
}

std::vector<DeadCodeCandidate> candidates(const SemanticModel& model,const Projection& projection,const AnalysisReadiness& state,const ModelIndex& index){
    std::map<std::string,std::vector<SymbolID>> groups;
    for(const auto& id:index.explicit_definitions)if(const auto found=model.symbols.find(id);found!=model.symbols.end()&&callable(found->second.kind)&&!found->second.exported)groups[group_key(found->second)].push_back(id);
    std::vector<DeadCodeCandidate> result;
    for(auto&[_,definitions]:groups){
        const auto representative=definitions.front();const auto& symbol=model.symbols.at(representative);
        std::vector<SymbolID> variants=definitions;
        if(symbol.logical_symbol)if(const auto logical=model.logical_symbols.find(*symbol.logical_symbol);logical!=model.logical_symbols.end())variants.assign(logical->second.variants.begin(),logical->second.variants.end());
        bool reachable{};for(const auto& variant:variants)reachable|=projection.reachable.contains(variant);if(reachable)continue;
        CandidateConfidence confidence=CandidateConfidence::Low;
        const bool closed_candidate=symbol.linkage==Linkage::None||symbol.linkage==Linkage::Internal||symbol.visibility==Visibility::Hidden;
        if(state.high_confidence_ready()&&closed_candidate)confidence=CandidateConfidence::High;
        else if(state.parse_complete&&state.roots_authoritative&&state.cross_configuration_bindings_complete&&state.project_calls_resolved)confidence=CandidateConfidence::Medium;
        DeadCodeCandidate candidate;candidate.symbol=representative;candidate.logical_symbol=symbol.logical_symbol;candidate.variants=std::move(variants);candidate.confidence=confidence;candidate.reason=readiness_reason(state);result.push_back(std::move(candidate));
    }
    return result;
}

} // namespace

AnalysisReadiness assess_dead_code_readiness(const SemanticModel& model){const ModelIndex index(model);const auto projection=project_reachability(model,index);return readiness(model,projection,index);}

std::vector<DeadCodeCandidate> analyze_dead_code(const SemanticModel& model){
    const ModelIndex index(model);const auto projection=project_reachability(model,index);const auto state=readiness(model,projection,index);return candidates(model,projection,state,index);
}

std::string export_dead_code_candidates(const SemanticModel& model,bool json_lines){
    const ModelIndex index(model);const auto projection=project_reachability(model,index);const auto state=readiness(model,projection,index);const auto dead=candidates(model,projection,state,index);
    std::ostringstream out;
    for(const auto& issue:state.issues){
        if(json_lines)out<<"{\"record\":\"analysis_readiness\",\"code\":\""<<json_escape(issue.code)<<"\",\"message\":\""<<json_escape(issue.message)<<"\"}\n";
        else out<<"readiness|"<<issue.code<<'|'<<issue.message<<'\n';
    }
    std::map<TranslationUnitID,std::set<std::string>> target_names_by_tu;for(const auto&[tu,target_ids]:index.targets_by_tu)for(const auto& target_id:target_ids)if(const auto target=model.build_targets.find(target_id);target!=model.build_targets.end())target_names_by_tu[tu].insert(target->second.name);
    for(const auto& candidate:dead){const auto found=model.symbols.find(candidate.symbol);if(found==model.symbols.end())continue;const auto& symbol=found->second;
        std::string signature;if(symbol.type)if(const auto type=model.types.find(*symbol.type);type!=model.types.end())signature=type->second.canonical_spelling;
        std::set<std::string> configurations,targets,definitions;
        for(const auto& variant:candidate.variants){const auto occurrences=index.occurrences_by_symbol.find(variant);if(occurrences==index.occurrences_by_symbol.end())continue;for(const auto* occurrence:occurrences->second){const auto revision=model.revisions.find(occurrence->revision);if(revision==model.revisions.end())continue;const auto tu=model.translation_units.find(revision->second.tu);if(tu==model.translation_units.end())continue;configurations.insert(tu->second.configuration.value);if(occurrence->role==OccurrenceRole::Definition)if(const auto version=model.file_versions.find(occurrence->range.file);version!=model.file_versions.end())if(const auto file=model.files.find(version->second.file);file!=model.files.end())definitions.insert(file->second.path+":"+std::to_string(occurrence->range.begin_line));if(const auto names=target_names_by_tu.find(tu->first);names!=target_names_by_tu.end())targets.insert(names->second.begin(),names->second.end());}}
        auto joined=[](const std::set<std::string>& values){std::string result;for(const auto& value:values){if(!result.empty())result.push_back(',');result+=value;}return result;};
        if(json_lines){out<<"{\"record\":\"dead_code_candidate\",\"symbol_id\":\""<<symbol.id.value<<"\",\"logical_symbol_id\":\""<<(candidate.logical_symbol?candidate.logical_symbol->value:std::string{})<<"\",\"variant_count\":"<<candidate.variants.size()<<",\"symbol\":\""<<json_escape(symbol.qualified_name)<<"\",\"signature\":\""<<json_escape(signature)<<"\",\"definitions\":\""<<json_escape(joined(definitions))<<"\",\"kind\":\""<<to_string(symbol.kind)<<"\",\"configurations\":\""<<joined(configurations)<<"\",\"targets\":\""<<json_escape(joined(targets))<<"\",\"template_primary\":"<<(symbol.templ.is_primary?"true":"false")<<",\"defaulted\":"<<(symbol.function.is_defaulted?"true":"false")<<",\"confidence\":\""<<to_string(candidate.confidence)<<"\",\"reason\":\""<<json_escape(candidate.reason)<<"\"}\n";}
        else out<<"candidate|"<<to_string(candidate.confidence)<<'|'<<to_string(symbol.kind)<<'|'<<symbol.qualified_name<<'|'<<signature<<'|'<<candidate.reason<<'|'<<symbol.id.value<<"|logical:"<<(candidate.logical_symbol?candidate.logical_symbol->value:std::string{})<<"|variants:"<<candidate.variants.size()<<"|definitions:"<<joined(definitions)<<"|configurations:"<<joined(configurations)<<"|targets:"<<joined(targets)<<"|template-primary:"<<symbol.templ.is_primary<<"|defaulted:"<<symbol.function.is_defaulted<<'\n';
    }return out.str();
}

} // namespace codeinsight
