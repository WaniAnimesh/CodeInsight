#include "codeinsight/semantic/call_resolution.hpp"

#include <algorithm>
#include <bit>
#include <queue>
#include <regex>
#include <tuple>

namespace codeinsight {
namespace {

void expand_virtual_dispatch(SemanticModel& model) {
    std::erase_if(model.relationships, [](const auto& item) {
        const auto& edge = item.second;
        return edge.kind == RelationshipKind::Calls && edge.dispatch == DispatchKind::Virtual &&
            edge.resolution == ResolutionStatus::Conservative && edge.origin == EvidenceOrigin::DerivedCanonicalization;
    });
    std::map<SymbolID, std::set<SymbolID>> direct_overrides;
    for (const auto& [_, edge] : model.relationships)
        if (edge.kind == RelationshipKind::Overrides && edge.target_symbol)
            direct_overrides[*edge.target_symbol].insert(edge.source);
    // Repeated calls to the same declaration share one hierarchy traversal.
    std::map<SymbolID, std::set<SymbolID>> closure;
    std::vector<Relationship> derived;
    for (const auto& [call_id, call] : model.relationships) {
        if (call.kind != RelationshipKind::Calls || call.dispatch != DispatchKind::Virtual ||
            !call.target_symbol || call.resolution != ResolutionStatus::Exact) continue;
        auto [entry, inserted] = closure.try_emplace(*call.target_symbol);
        if (inserted) {
            std::vector<SymbolID> pending{*call.target_symbol};
            std::set<SymbolID> seen{*call.target_symbol};
            while (!pending.empty()) {
                const auto base = pending.back();
                pending.pop_back();
                const auto found = direct_overrides.find(base);
                if (found == direct_overrides.end()) continue;
                for (const auto& target : found->second) if (seen.insert(target).second) {
                    pending.push_back(target);
                    entry->second.insert(target);
                }
            }
        }
        for (const auto& target : entry->second) {
            Relationship edge = call;
            edge.target_symbol = target;
            edge.resolution = ResolutionStatus::Conservative;
            edge.origin = EvidenceOrigin::DerivedCanonicalization;
            edge.is_virtual = true;
            edge.id = RelationshipID{sha256("virtual-dispatch|" + call_id.value + "|" + target.value)};
            derived.push_back(std::move(edge));
        }
    }
    for (auto& edge : derived) model.relationships.insert_or_assign(edge.id, std::move(edge));
}

void expand_cross_configuration_bindings(SemanticModel& model){
    std::erase_if(model.relationships,[](const auto& item){return item.second.kind==RelationshipKind::Calls&&item.second.origin==EvidenceOrigin::DerivedLogicalEquivalence;});
    std::set<SymbolID> definition_symbols;for(const auto&[_,occurrence]:model.occurrences)if(occurrence.role==OccurrenceRole::Definition)definition_symbols.insert(occurrence.symbol);
    std::map<std::string,std::set<SymbolID>> definitions_by_usr;for(const auto&[id,symbol]:model.symbols)if(!symbol.usr.empty()&&definition_symbols.contains(id))definitions_by_usr[symbol.usr].insert(id);
    std::vector<Relationship> derived;for(const auto&[call_id,call]:model.relationships){if(call.kind!=RelationshipKind::Calls||call.resolution!=ResolutionStatus::Unresolved||call.target_domain!=TargetDomain::Project||call.observed_target_usr.empty())continue;const auto found=definitions_by_usr.find(call.observed_target_usr);if(found==definitions_by_usr.end())continue;for(const auto& target:found->second){Relationship edge=call;edge.id=RelationshipID{sha256("logical-call|"+call_id.value+"|"+target.value)};edge.target_symbol=target;edge.unresolved_target.clear();edge.resolution=ResolutionStatus::Conservative;edge.origin=EvidenceOrigin::DerivedLogicalEquivalence;edge.resolution_failure=ResolutionFailure::CrossConfigurationVariant;derived.push_back(std::move(edge));}}
    for(auto& edge:derived)model.relationships.insert_or_assign(edge.id,std::move(edge));
}

using CallSiteKey = std::tuple<SymbolID, TranslationUnitRevisionID, SourceRange>;

CallSiteKey call_site(const Relationship& edge) {
    return {edge.source, edge.observed_in, edge.evidence};
}

void expand_indirect_calls(SemanticModel& model) {
    std::erase_if(model.relationships, [](const auto& item) {
        const auto& edge = item.second;
        return edge.kind == RelationshipKind::Calls && edge.dispatch == DispatchKind::Dynamic &&
            edge.origin == EvidenceOrigin::DerivedCanonicalization;
    });
    // A storage identity is scoped to the observation revision. Header facts
    // from another TU/configuration cannot supply this invocation's values.
    using StorageKey = std::pair<TranslationUnitRevisionID, SymbolID>;
    std::map<StorageKey, std::set<SymbolID>> values;
    std::map<StorageKey, std::set<StorageKey>> consumers;
    std::map<CallSiteKey, StorageKey> invocations;
    std::queue<std::pair<StorageKey, SymbolID>> pending;
    for (const auto& [_, edge] : model.relationships) {
        if (!edge.target_symbol || edge.origin != EvidenceOrigin::CompilerAST) continue;
        if (edge.kind == RelationshipKind::InvokesCallable) {
            invocations.emplace(call_site(edge), StorageKey{edge.observed_in, *edge.target_symbol});
        } else if (edge.kind == RelationshipKind::CallableValueFlow) {
            const StorageKey destination{edge.observed_in, edge.source};
            const auto& target = model.symbols.at(*edge.target_symbol);
            if (target.kind == SymbolKind::LocalVariable) {
                consumers[{edge.observed_in, target.id}].insert(destination);
            } else if (values[destination].insert(target.id).second) {
                pending.emplace(destination, target.id);
            }
        }
    }
    // Monotone, flow-insensitive propagation: each newly discovered value is
    // sent only to actual copy consumers, and cycles terminate by set insertion.
    while (!pending.empty()) {
        auto [storage, target] = std::move(pending.front());
        pending.pop();
        const auto found = consumers.find(storage);
        if (found == consumers.end()) continue;
        for (const auto& destination : found->second)
            if (values[destination].insert(target).second) pending.emplace(destination, target);
    }
    std::vector<Relationship> derived;
    for (const auto& [call_id, call] : model.relationships) {
        if (call.kind != RelationshipKind::Calls || call.origin != EvidenceOrigin::CompilerAST ||
            call.resolution != ResolutionStatus::Unresolved || call.dispatch != DispatchKind::Dynamic) continue;
        const auto invocation = invocations.find(call_site(call));
        if (invocation == invocations.end()) continue;
        const auto candidates = values.find(invocation->second);
        if (candidates == values.end()) continue;
        for (const auto& target : candidates->second) {
            Relationship edge = call;
            edge.id = RelationshipID{sha256("indirect-call|" + call_id.value + "|" + target.value)};
            edge.target_symbol = target;
            edge.unresolved_target.clear();
            edge.observed_target_usr = model.symbols.at(target).usr;
            edge.observed_target_spelling = model.symbols.at(target).qualified_name;
            edge.resolution = ResolutionStatus::Conservative;
            edge.origin = EvidenceOrigin::DerivedCanonicalization;
            edge.target_domain = TargetDomain::Project;
            edge.resolution_failure = ResolutionFailure::None;
            derived.push_back(std::move(edge));
        }
    }
    for (auto& edge : derived) model.relationships.insert_or_assign(edge.id, std::move(edge));
}

std::optional<std::string> source_text(const SemanticModel& model,const FileVersionID& version){
    const auto file_version=model.file_versions.find(version);if(file_version==model.file_versions.end())return std::nullopt;
    const auto file=model.files.find(file_version->second.file);if(file==model.files.end())return std::nullopt;
    try{auto content=read_text_file(file->second.path);if(sha256(content)!=file_version->second.content_hash)return std::nullopt;return content;}catch(...){return std::nullopt;}
}

std::optional<std::string_view> source_slice(const std::string& text,const SourceRange& range){
    if(range.begin_offset>range.end_offset||range.end_offset>text.size())return std::nullopt;
    return std::string_view(text).substr(range.begin_offset,range.end_offset-range.begin_offset);
}

std::optional<SourceRange> definition_range(const SemanticModel& model,const SymbolID& symbol,const TranslationUnitRevisionID& revision){
    for(const auto&[_,occurrence]:model.occurrences)if(occurrence.symbol==symbol&&occurrence.revision==revision&&occurrence.role==OccurrenceRole::Definition)return occurrence.range;
    return std::nullopt;
}

bool contained(const SourceRange& outer,const SourceRange& inner){return outer.file==inner.file&&outer.begin_offset<=inner.begin_offset&&outer.end_offset>=inner.end_offset;}

std::optional<std::uint64_t> unsigned_literal(std::string value){
    while(!value.empty()&&std::string_view("uUlL").find(value.back())!=std::string_view::npos)value.pop_back();
    try{std::size_t consumed{};const auto result=std::stoull(value,&consumed,0);if(consumed==value.size())return result;}catch(...){}return std::nullopt;
}

struct TableModel {std::uint64_t size{};std::string variable;std::string default_target;std::map<std::uint64_t,std::string> overrides;};

std::optional<TableModel> parse_closed_table(std::string_view body){
    const std::string text(body);
    const std::regex declaration(R"(std\s*::\s*array\s*<[^;{}]+,\s*([0-9]+)[uUlL]*\s*>\s*([A-Za-z_]\w*)\s*\{\s*\}\s*;)");
    std::smatch declared;if(!std::regex_search(text,declared,declaration))return std::nullopt;
    TableModel result;result.size=std::stoull(declared[1].str());result.variable=declared[2].str();if(!result.size)return std::nullopt;
    const auto escaped=result.variable;
    const std::regex fill("\\b"+escaped+R"(\s*\.\s*fill\s*\(\s*&\s*([A-Za-z_]\w*)\s*\)\s*;)");
    std::smatch filled;if(!std::regex_search(text,filled,fill))return std::nullopt;result.default_target=filled[1].str();
    const std::regex assignment("\\b"+escaped+R"(\s*\[\s*(0[xX][0-9A-Fa-f]+|[0-9]+)[uUlL]*\s*\]\s*=\s*&\s*([A-Za-z_]\w*)\s*;)");
    std::size_t assignments{};for(std::sregex_iterator it(text.begin(),text.end(),assignment),end;it!=end;++it){const auto index=unsigned_literal((*it)[1].str());if(!index||*index>=result.size||!result.overrides.emplace(*index,(*it)[2].str()).second)return std::nullopt;++assignments;}
    const std::regex returned("\\breturn\\s+"+escaped+R"(\s*;)");if(!std::regex_search(text,returned))return std::nullopt;
    const std::regex every_use("\\b"+escaped+"\\b");const auto uses=static_cast<std::size_t>(std::distance(std::sregex_iterator(text.begin(),text.end(),every_use),std::sregex_iterator()));
    // Declaration, fill, each constant override, and return are the only legal
    // uses.  Any alias, parameter pass, field store, or extra mutation fails shut.
    if(uses!=3+assignments)return std::nullopt;
    return result;
}

std::optional<IntegerValueDomain> parse_bounded_index(std::string_view body){
    const std::string text(body);const std::regex any_return(R"(\breturn\b)");if(std::distance(std::sregex_iterator(text.begin(),text.end(),any_return),std::sregex_iterator())!=1)return std::nullopt;
    const std::regex returned(R"(return\s+static_cast\s*<[^>]+>\s*\(\s*[^;]*&\s*(0[xX][0-9A-Fa-f]+|[0-9]+)[uUlL]*\s*\)\s*;)");
    std::smatch match;if(!std::regex_search(text,match,returned))return std::nullopt;const auto mask=unsigned_literal(match[1].str());if(!mask)return std::nullopt;
    IntegerValueDomain domain;domain.kind=IntegerDomainKind::BitMask;domain.minimum=0;domain.maximum=*mask;domain.known_zero_mask=~*mask;
    if(std::popcount(*mask)<=12){std::uint64_t value=*mask;while(true){domain.exact_values.insert(value);if(!value)break;value=(value-1)&*mask;}domain.kind=IntegerDomainKind::ExactSet;}
    return domain;
}

void derive_closed_dispatch_tables(SemanticModel& model){
    std::erase_if(model.relationships,[](const auto& item){return item.second.kind==RelationshipKind::Calls&&item.second.origin==EvidenceOrigin::DerivedFlowAnalysis;});
    model.indirect_call_summaries.clear();
    const std::regex dispatch_expression(R"(([A-Za-z_]\w*)\s*\(\s*\)\s*\[\s*([A-Za-z_]\w*)\s*\([^\]]*\)\s*\])");
    std::map<FileVersionID,std::optional<std::string>> source_cache;
    const auto cached_source=[&](const FileVersionID& version)->const std::string*{
        auto [found,inserted]=source_cache.try_emplace(version);
        if(inserted){
            const auto file_version=model.file_versions.find(version);
            if(file_version!=model.file_versions.end()){
                const auto file=model.files.find(file_version->second.file);
                // Flow proofs are intentionally restricted to project-owned
                // source. Template bodies in SDK headers can span megabytes,
                // are open-world, and are never safe places to discharge a
                // project function's address-taken root.
                if(file!=model.files.end()&&!file->second.external)found->second=source_text(model,version);
            }
        }
        return found->second?&*found->second:nullptr;
    };
    std::vector<Relationship> derived_calls;
    for(const auto&[call_id,call]:model.relationships){
        if(call.kind!=RelationshipKind::Calls||call.resolution!=ResolutionStatus::Unresolved||call.dispatch!=DispatchKind::Dynamic||call.origin!=EvidenceOrigin::CompilerAST)continue;
        const auto* text=cached_source(call.evidence.file);if(!text)continue;const auto expression=source_slice(*text,call.evidence);if(!expression||expression->size()>16384)continue;
        std::match_results<std::string_view::const_iterator> match;if(!std::regex_search(expression->begin(),expression->end(),match,dispatch_expression))continue;
        const std::string storage_name(match[1].first,match[1].second),index_name(match[2].first,match[2].second);
        std::optional<SymbolID> storage_provider,index_provider;
        for(const auto&[_,edge]:model.relationships){
            if(edge.kind!=RelationshipKind::Calls||edge.source!=call.source||edge.observed_in!=call.observed_in||!edge.target_symbol||!contained(call.evidence,edge.evidence))continue;
            const auto symbol=model.symbols.find(*edge.target_symbol);if(symbol==model.symbols.end())continue;
            if(symbol->second.name==storage_name){if(storage_provider&&*storage_provider!=symbol->first){storage_provider.reset();break;}storage_provider=symbol->first;}
            if(symbol->second.name==index_name){if(index_provider&&*index_provider!=symbol->first){index_provider.reset();break;}index_provider=symbol->first;}
        }
        if(!storage_provider||!index_provider)continue;const auto storage_symbol=model.symbols.find(*storage_provider);if(storage_symbol==model.symbols.end()||(storage_symbol->second.linkage!=Linkage::Internal&&storage_symbol->second.linkage!=Linkage::None))continue;
        if(std::any_of(model.relationships.begin(),model.relationships.end(),[&](const auto& item){const auto& edge=item.second;return edge.kind==RelationshipKind::TakesAddress&&edge.target_symbol&&*edge.target_symbol==*storage_provider;}))continue;
        const auto storage_range=definition_range(model,*storage_provider,call.observed_in),index_range=definition_range(model,*index_provider,call.observed_in);if(!storage_range||!index_range||storage_range->file!=call.evidence.file||index_range->file!=call.evidence.file)continue;
        const auto storage_body=source_slice(*text,*storage_range),index_body=source_slice(*text,*index_range);if(!storage_body||!index_body)continue;
        const auto table=parse_closed_table(*storage_body);const auto domain=parse_bounded_index(*index_body);if(!table||!domain||domain->exact_values.empty())continue;

        // Every call to the provider must be lexically contained by a dynamic
        // dispatch expression. Otherwise the returned table may be observed by
        // an unanalyzed consumer and its address values still escape.
        bool storage_escapes{};
        for(const auto&[_,provider_call]:model.relationships)if(provider_call.kind==RelationshipKind::Calls&&provider_call.target_symbol&&*provider_call.target_symbol==*storage_provider&&provider_call.resolution!=ResolutionStatus::Unresolved){
            bool covered{};for(const auto&[__,dynamic]:model.relationships)covered|=dynamic.kind==RelationshipKind::Calls&&dynamic.resolution==ResolutionStatus::Unresolved&&dynamic.source==provider_call.source&&dynamic.observed_in==provider_call.observed_in&&contained(dynamic.evidence,provider_call.evidence);
            if(!covered){storage_escapes=true;break;}
        }
        if(storage_escapes)continue;

        std::map<std::string,SymbolID> targets_by_name;std::set<RelationshipID> modeled_addresses;bool ambiguous{};
        for(const auto&[address_id,address]:model.relationships)if(address.kind==RelationshipKind::TakesAddress&&address.target_symbol&&address.observed_in==call.observed_in&&contained(*storage_range,address.evidence)){
            const auto target=model.symbols.find(*address.target_symbol);if(target==model.symbols.end())continue;const auto [found,inserted]=targets_by_name.emplace(target->second.name,target->first);if(!inserted&&found->second!=target->first)ambiguous=true;modeled_addresses.insert(address_id);
        }
        std::set<std::string> stored_names{table->default_target};for(const auto&[_,name]:table->overrides)stored_names.insert(name);
        if(ambiguous||modeled_addresses.empty()||std::any_of(stored_names.begin(),stored_names.end(),[&](const auto& name){return !targets_by_name.contains(name);} ))continue;
        std::set<SymbolID> stored_targets;for(const auto& name:stored_names)stored_targets.insert(targets_by_name.at(name));
        std::map<std::string,std::size_t> expected_addresses;expected_addresses[table->default_target]=1;for(const auto&[_,name]:table->overrides)++expected_addresses[name];
        bool extra_address{};for(const auto&[name,expected]:expected_addresses){const std::regex address("&\\s*"+name+"\\b");using ViewRegexIterator=std::regex_iterator<std::string_view::const_iterator>;const auto observed=static_cast<std::size_t>(std::distance(ViewRegexIterator(storage_body->begin(),storage_body->end(),address),ViewRegexIterator()));if(observed!=expected){extra_address=true;break;}}if(extra_address)continue;
        std::erase_if(modeled_addresses,[&](const auto& address_id){const auto address=model.relationships.find(address_id);return address==model.relationships.end()||!address->second.target_symbol||!stored_targets.contains(*address->second.target_symbol);});if(modeled_addresses.empty())continue;
        // An address use outside this exact, closed storage construction keeps
        // the function globally retained. Do not publish a partial proof.
        bool unmodeled_address{};for(const auto&[address_id,address]:model.relationships)if(address.kind==RelationshipKind::TakesAddress&&address.target_symbol&&stored_targets.contains(*address.target_symbol)&&!modeled_addresses.contains(address_id)){unmodeled_address=true;break;}if(unmodeled_address)continue;

        std::set<SymbolID> selected_targets;for(const auto index:domain->exact_values){if(index>=table->size){storage_escapes=true;break;}const auto override=table->overrides.find(index);selected_targets.insert(targets_by_name.at(override==table->overrides.end()?table->default_target:override->second));}if(storage_escapes||selected_targets.empty())continue;
        const auto revision=model.revisions.find(call.observed_in);if(revision==model.revisions.end())continue;const auto tu=model.translation_units.find(revision->second.tu);if(tu==model.translation_units.end())continue;
        const std::string summary_key="indirect-summary|"+call_id.value+"|"+call.observed_in.value+"|"+tu->second.configuration.value+"|"+std::to_string(flow_analysis_version);const IndirectCallSummaryID summary_id{sha256(summary_key)};
        IndirectCallSummary summary;summary.id=summary_id;summary.call_observation=call_id;summary.revision=call.observed_in;summary.configuration=tu->second.configuration;summary.storage_provider=*storage_provider;summary.index_provider=*index_provider;summary.index_domain={};summary.stored_targets=stored_targets;summary.selectable_targets=stored_targets;summary.modeled_address_observations=modeled_addresses;summary.storage_escapes=true;summary.complete=false;summary.reason="source-pattern candidates only; execution order, index semantics, and escape safety are unproved";model.indirect_call_summaries.emplace(summary_id,std::move(summary));
        for(const auto& target:stored_targets){Relationship edge=call;edge.id=RelationshipID{sha256("flow-dispatch|"+call_id.value+"|"+target.value+"|"+std::to_string(flow_analysis_version))};edge.target_symbol=target;edge.unresolved_target.clear();edge.observed_target_usr=model.symbols.at(target).usr;edge.observed_target_spelling=model.symbols.at(target).qualified_name;edge.resolution=ResolutionStatus::Conservative;edge.dispatch=DispatchKind::Dynamic;edge.origin=EvidenceOrigin::DerivedFlowAnalysis;edge.target_domain=TargetDomain::Project;edge.resolution_failure=ResolutionFailure::None;derived_calls.push_back(std::move(edge));}
    }
    for(auto& edge:derived_calls)model.relationships.insert_or_assign(edge.id,std::move(edge));
}


} // namespace

void resolve_call_targets(SemanticModel& model) {
    expand_cross_configuration_bindings(model);
    expand_virtual_dispatch(model);
    expand_indirect_calls(model);
    derive_closed_dispatch_tables(model);
}

std::vector<CallTargetSet> summarize_call_targets(const SemanticModel& model) {
    std::map<CallSiteKey, std::vector<const Relationship*>> by_site;
    for (const auto& [_, edge] : model.relationships)
        if (edge.kind == RelationshipKind::Calls) by_site[call_site(edge)].push_back(&edge);
    std::set<RelationshipID> complete_indirect;
    for (const auto& [_, summary] : model.indirect_call_summaries)
        if (summary.complete && !summary.storage_escapes && summary.analyzer_version == flow_analysis_version)
            complete_indirect.insert(summary.call_observation);
    std::vector<CallTargetSet> result;
    for (const auto& [id, call] : model.relationships) {
        if (call.kind != RelationshipKind::Calls || call.origin != EvidenceOrigin::CompilerAST) continue;
        CallTargetSet summary;
        summary.observation = id;
        for (const auto* edge : by_site.at(call_site(call))) {
            summary.evidence.insert(edge->id);
            if (edge->target_symbol) summary.targets.insert(*edge->target_symbol);
        }
        if ((call.dispatch == DispatchKind::Static && call.resolution == ResolutionStatus::Exact && call.target_symbol) ||
            complete_indirect.contains(id)) {
            summary.completeness = CallTargetCompleteness::Complete;
        } else if (call.dispatch == DispatchKind::Static && call.target_domain == TargetDomain::External && summary.targets.empty()) {
            summary.completeness = CallTargetCompleteness::ExternalOnly;
        }
        result.push_back(std::move(summary));
    }
    return result;
}
} // namespace codeinsight
