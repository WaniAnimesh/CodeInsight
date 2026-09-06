#include "codeinsight/codeinsight.hpp"

#include <algorithm>
#include <fstream>
#include <queue>
#include <sstream>
#include <stdexcept>

namespace codeinsight {
namespace {

template<class Id> void collision(const Id& id, std::string_view stored, std::string_view proposed, std::string_view entity) {
    if(stored!=proposed) throw std::runtime_error(std::string(entity)+" SHA-256 collision for id "+id.value);
}

std::string tu_map_key(TranslationUnitID id) { return id.value; }

bool record_kind(SymbolKind kind) { return kind==SymbolKind::Class || kind==SymbolKind::Struct || kind==SymbolKind::Union || kind==SymbolKind::ClassTemplate; }
bool base_target_kind(SymbolKind kind) {
    return record_kind(kind) || kind==SymbolKind::Typedef || kind==SymbolKind::TypeAlias ||
           kind==SymbolKind::AliasTemplate || kind==SymbolKind::TemplateTypeParameter ||
           kind==SymbolKind::TemplateTemplateParameter;
}
bool method_kind(SymbolKind kind) { return kind==SymbolKind::Method || kind==SymbolKind::Constructor || kind==SymbolKind::Destructor || kind==SymbolKind::ConversionFunction || kind==SymbolKind::OperatorFunction; }
bool process_entry(const SymbolFact& fact) { return fact.kind==SymbolKind::Function&&(fact.name=="main"||fact.name=="wmain"||fact.name=="WinMain"||fact.name=="wWinMain"); }

std::string function_state(const FunctionProperties& p){return std::to_string(p.variadic)+std::to_string(p.is_static)+std::to_string(p.is_virtual)+std::to_string(p.is_pure_virtual)+std::to_string(p.is_override)+std::to_string(p.is_final)+std::to_string(p.is_const)+std::to_string(p.is_volatile)+std::to_string(p.ref_lvalue)+std::to_string(p.ref_rvalue)+std::to_string(p.is_constexpr)+std::to_string(p.is_consteval)+std::to_string(p.is_deleted)+std::to_string(p.is_defaulted)+std::to_string(p.is_explicit)+std::to_string(p.is_noexcept);}
std::string variable_state(const VariableProperties& p){return std::to_string(p.is_static)+std::to_string(p.is_thread_local)+std::to_string(p.is_constexpr)+std::to_string(p.is_constinit)+std::to_string(p.is_mutable);}
std::string template_state(const TemplateProperties& p){return std::to_string(p.is_primary)+std::to_string(p.is_partial_specialization)+std::to_string(p.is_explicit_specialization)+std::to_string(p.is_instantiation)+std::to_string(p.dependent);}

void merge_properties(FunctionProperties& into, const FunctionProperties& from) {
    into.variadic |= from.variadic;
    into.is_static |= from.is_static;
    into.is_virtual |= from.is_virtual;
    into.is_pure_virtual |= from.is_pure_virtual;
    into.is_override |= from.is_override;
    into.is_final |= from.is_final;
    into.is_const |= from.is_const;
    into.is_volatile |= from.is_volatile;
    into.ref_lvalue |= from.ref_lvalue;
    into.ref_rvalue |= from.ref_rvalue;
    into.is_constexpr |= from.is_constexpr;
    into.is_consteval |= from.is_consteval;
    into.is_deleted |= from.is_deleted;
    into.is_defaulted |= from.is_defaulted;
    into.is_explicit |= from.is_explicit;
    into.is_noexcept |= from.is_noexcept;
}

void merge_properties(VariableProperties& into, const VariableProperties& from) {
    into.is_static |= from.is_static;
    into.is_thread_local |= from.is_thread_local;
    into.is_constexpr |= from.is_constexpr;
    into.is_constinit |= from.is_constinit;
    into.is_mutable |= from.is_mutable;
}

void merge_properties(TemplateProperties& into, const TemplateProperties& from) {
    into.is_primary |= from.is_primary;
    into.is_partial_specialization |= from.is_partial_specialization;
    into.is_explicit_specialization |= from.is_explicit_specialization;
    into.is_instantiation |= from.is_instantiation;
    into.dependent |= from.dependent;
}

void recompute_properties(SemanticModel& model) {
    for (auto& [_, symbol] : model.symbols) {
        symbol.function = {}; symbol.variable = {}; symbol.templ = {}; symbol.exported = false; symbol.type.reset();
    }
    std::map<SymbolID,int> type_priority;
    for (const auto& [_, observation] : model.occurrences) {
        auto& symbol = model.symbols.at(observation.symbol);
        const int priority=observation.role==OccurrenceRole::Definition?3:(observation.implicit?1:2);
        if(observation.type && (!symbol.type || priority>type_priority[symbol.id])) {
            symbol.type=observation.type; type_priority[symbol.id]=priority;
        }
        merge_properties(symbol.function, observation.function);
        merge_properties(symbol.variable, observation.variable);
        merge_properties(symbol.templ, observation.templ);
        symbol.exported |= observation.exported;
    }
}

void recompute_completeness(SemanticModel& model) {
    model.completeness={};
    for(const auto& [_,revision]:model.revisions) {
        ++model.completeness.total_tus;
        switch(revision.quality) {
        case ExtractionQuality::Complete: ++model.completeness.complete_tus; break;
        case ExtractionQuality::CompleteWithWarnings: ++model.completeness.warning_tus; break;
        case ExtractionQuality::Partial: ++model.completeness.partial_tus; break;
        case ExtractionQuality::Failed: ++model.completeness.failed_tus; break;
        }
    }
}

} // namespace

Canonicalizer::Canonicalizer(SemanticModel& model) : model_(model) {}

void Canonicalizer::detach(TranslationUnitID tu) {
    const auto active=model_.active_revision_by_tu.find(tu_map_key(tu));
    if(active==model_.active_revision_by_tu.end()) return;
    const auto revision=active->second;
    std::erase_if(model_.occurrences,[&](const auto& item){return item.second.revision==revision;});
    std::erase_if(model_.relationships,[&](const auto& item){return item.second.observed_in==revision;});
    std::erase_if(model_.macro_definitions,[&](const auto& item){return item.second.revision==revision;});
    std::erase_if(model_.macro_expansions,[&](const auto& item){return item.second.revision==revision;});
    std::erase_if(model_.includes,[&](const auto& item){return item.second.revision==revision;});
    std::erase_if(model_.diagnostics,[&](const auto& item){return item.second.revision==revision;});
    model_.revisions.erase(revision);
    model_.active_revision_by_tu.erase(active);
    recompute_properties(model_);
    recompute_completeness(model_);
}

void Canonicalizer::apply(const FactBatch& batch) {
    const auto& command=batch.command;
    if(!command.source.valid() || !command.configuration.valid()) throw std::runtime_error("fact batch has invalid compilation identity");

    if(const auto existing=model_.configurations.find(command.configuration);existing!=model_.configurations.end())collision(command.configuration,existing->second.canonical_key,command.configuration_key,"build configuration");
    else model_.configurations.emplace(command.configuration,BuildConfiguration{command.configuration,command.configuration_key,sha256("compilation-fingerprint|"+command.configuration_key)});
    const TranslationUnitID tu{sha256("tu|"+command.source.value+"|"+command.configuration.value)};
    detach(tu);

    auto ensure_file=[&](const std::string& raw_path)->std::pair<FileID,FileVersionID> {
        if(!raw_path.empty())if(const auto cached=file_cache_.find(raw_path);cached!=file_cache_.end())return cached->second;
        const auto path=normalize_path(raw_path.empty()?command.source_path:std::filesystem::path(raw_path),command.working_directory);
        if(const auto cached=file_cache_.find(path);cached!=file_cache_.end())return cached->second;
        const FileID file_id{sha256("file|"+model_.workspace.value+"|"+path)};
        bool external=true;for(const auto& root:command.project_roots)if(path_is_within(path,root)){external=false;break;}
        if(path==normalize_path(command.source_path))external=false;
        const auto [file,inserted]=model_.files.try_emplace(file_id,File{file_id,model_.workspace,path,external});if(!inserted)file->second.external=external;
        std::error_code error; const auto fs_path=std::filesystem::path(path);
        std::string content_hash="missing:"+sha256(path); std::uint64_t size{};
        if(std::filesystem::is_regular_file(fs_path,error)) { content_hash=read_file_hash(fs_path); size=std::filesystem::file_size(fs_path,error); }
        const FileVersionID version_id{sha256("file-version|"+file_id.value+"|"+content_hash)};
        model_.file_versions.try_emplace(version_id,FileVersion{version_id,file_id,content_hash,size});
        return file_cache_.emplace(path,std::pair{file_id,version_id}).first->second;
    };

    const auto [source_file,source_version]=ensure_file(normalize_path(command.source_path));
    model_.translation_units[tu]={tu,source_file,command.configuration};
    const TranslationUnitRevisionID revision{sha256("tu-revision|"+tu.value+"|"+batch.revision_key.source_hash+"|"+batch.revision_key.compilation_fingerprint+"|"+std::to_string(batch.revision_key.extractor_version))};
    model_.revisions[revision]={revision,tu,batch.revision_key.source_hash,batch.revision_key.compilation_fingerprint,batch.revision_key.extractor_version,batch.quality};
    model_.active_revision_by_tu[tu_map_key(tu)]=revision;

    auto semantic_range=[&](const RawSourceRange& raw) {
        const auto [_,version]=ensure_file(raw.begin.path.empty()?normalize_path(command.source_path):raw.begin.path);
        return SourceRange{version,raw.begin.line,raw.begin.column,raw.begin.offset,raw.end.line,raw.end.column,raw.end.offset};
    };

    std::map<std::string,TypeID> type_ids;
    for(const auto& fact:batch.types) {
        std::string key="type|"+std::to_string(static_cast<int>(fact.kind))+"|"+fact.canonical_spelling+"|"+
            (fact.qualifiers.is_const?"c":"")+(fact.qualifiers.is_volatile?"v":"")+(fact.qualifiers.is_restrict?"r":"");
        for(const auto& child:fact.children) key+="|child:"+child;
        for(const auto& arg:fact.template_arguments) key+="|arg:"+arg;
        if(fact.array_extent) key+="|extent:"+std::to_string(*fact.array_extent);
        key+="|variadic:"+std::to_string(fact.variadic)+"|dependent:"+std::to_string(fact.dependent);
        const TypeID id{sha256(key)}; type_ids[fact.local_identity]=id;
        if(const auto existing=model_.types.find(id);existing!=model_.types.end()) collision(id,existing->second.canonical_key,key,"type");
        else {
            Type type{id,key,fact.kind,fact.qualifiers,fact.spelling,fact.canonical_spelling};
            type.template_arguments=fact.template_arguments; type.array_extent=fact.array_extent; type.variadic=fact.variadic; type.dependent=fact.dependent;
            model_.types.emplace(id,std::move(type));
        }
    }
    for(const auto& fact:batch.types) {
        auto& type=model_.types.at(type_ids.at(fact.local_identity));
        std::vector<TypeID> resolved_children;for(const auto& child:fact.children)if(const auto it=type_ids.find(child);it!=type_ids.end())resolved_children.push_back(it->second);
        if(type.children.empty())type.children=std::move(resolved_children);else if(type.children!=resolved_children)throw std::runtime_error("conflicting structural children for canonical type "+type.canonical_key);
    }

    std::map<std::string,const SymbolFact*> representative;
    for(const auto& fact:batch.symbols){
        const auto [it,inserted]=representative.emplace(fact.local_identity,&fact);
        if(!inserted){const auto& prior=*it->second;if(prior.clang_usr!=fact.clang_usr||prior.kind!=fact.kind||prior.semantic_parent!=fact.semantic_parent)throw std::runtime_error("conflicting raw symbol observations for "+fact.local_identity);}
    }
    std::map<std::string,SymbolID> symbol_ids;
    const auto configuration_marker="|configuration|"+command.configuration.value;
    auto [usr_index,inserted_usr_index]=usr_index_.try_emplace(command.configuration);
    if(inserted_usr_index)for(const auto&[id,symbol]:model_.symbols)if(!symbol.usr.empty()&&symbol.canonical_key.find(configuration_marker)!=std::string::npos)usr_index->second[symbol.usr].insert(id);
    auto& configuration_usr_ids=usr_index->second;
    auto strong_key=[&](const SymbolFact& fact,std::string_view parent){std::string key="usr|"+fact.clang_usr+"|configuration|"+command.configuration.value+"|parent|"+std::string(parent);if(fact.linkage==Linkage::Internal||fact.is_anonymous||process_entry(fact))key+="|linkage-scope|"+source_file.value;return key;};
    auto logical_key=[&](const SymbolFact& fact)->std::string{
        if(fact.clang_usr.empty())return {};
        std::string key="logical-symbol|"+model_.workspace.value+"|usr|"+fact.clang_usr+"|linkage|"+std::to_string(static_cast<int>(fact.linkage));
        if(fact.linkage==Linkage::Internal||fact.is_anonymous||process_entry(fact))key+="|file|"+source_file.value;
        return key;
    };
    auto install_symbol=[&](const SymbolFact& fact,const std::string& key){
        const SymbolID id{sha256(key)};symbol_ids[fact.local_identity]=id;if(!fact.clang_usr.empty())configuration_usr_ids[fact.clang_usr].insert(id);
        if(const auto existing=model_.symbols.find(id);existing!=model_.symbols.end())collision(id,existing->second.canonical_key,key,"symbol");
        else{
            Symbol symbol;symbol.id=id;symbol.canonical_key=key;symbol.usr=fact.clang_usr;symbol.kind=fact.kind;symbol.name=fact.name;symbol.qualified_name=fact.qualified_name;symbol.linkage=fact.linkage;symbol.visibility=fact.visibility;
            const auto logical=logical_key(fact);if(!logical.empty()){
                const LogicalSymbolID logical_id{sha256(logical)};symbol.logical_symbol=logical_id;
                auto [entry,inserted]=model_.logical_symbols.try_emplace(logical_id,LogicalSymbol{logical_id,logical,fact.clang_usr,fact.linkage,{}});
                if(!inserted)collision(logical_id,entry->second.canonical_key,logical,"logical symbol");
                entry->second.variants.insert(id);
            }
            if(const auto type=type_ids.find(fact.type_key);type!=type_ids.end())symbol.type=type->second;symbol.function=fact.function;symbol.variable=fact.variable;symbol.templ=fact.templ;symbol.exported=fact.exported;model_.symbols.emplace(id,std::move(symbol));
        }
    };
    auto global_usr=[&](std::string_view usr)->std::optional<SymbolID>{
        const auto candidates=configuration_usr_ids.find(std::string(usr));
        return candidates!=configuration_usr_ids.end()&&candidates->second.size()==1?std::optional<SymbolID>{*candidates->second.begin()}:std::nullopt;
    };
    auto parent_resolution=[&](const SymbolFact& fact)->std::pair<bool,std::optional<SymbolID>>{
        if(!fact.semantic_parent)return {true,std::nullopt};
        if(const auto local=symbol_ids.find(*fact.semantic_parent);local!=symbol_ids.end())return {true,local->second};
        if(representative.contains(*fact.semantic_parent))return {false,std::nullopt};
        if(fact.semantic_parent->starts_with("usr:"))if(auto global=global_usr(std::string_view(*fact.semantic_parent).substr(4)))return {true,global};
        return {true,std::nullopt};
    };
    std::set<std::string> pending;for(const auto&[local,_]:representative)pending.insert(local);
    while(!pending.empty()){
        bool progressed{};
        for(auto it=pending.begin();it!=pending.end();){
            const auto& fact=*representative.at(*it);const auto [ready,parent]=parent_resolution(fact);if(!ready){++it;continue;}
            std::string signature;auto type_identity=[&](const std::string& local){if(local.empty())return std::string{};if(const auto type=type_ids.find(local);type!=type_ids.end())return type->second.value;return "unresolved-type:"+local;};
            if(!fact.result_type_key.empty())signature+="result:"+type_identity(fact.result_type_key);for(const auto& parameter:fact.parameter_type_keys)signature+="|param:"+type_identity(parameter);
            signature+="|const:"+std::to_string(fact.function.is_const)+"|volatile:"+std::to_string(fact.function.is_volatile)+"|ref:"+std::to_string(fact.function.ref_lvalue)+std::to_string(fact.function.ref_rvalue);
            const auto parent_key=parent?parent->value:(fact.semantic_parent?"unresolved-parent:"+*fact.semantic_parent:"global");
            std::string key;
            if(!fact.clang_usr.empty())key=strong_key(fact,parent_key);
            else{key="fallback|"+model_.workspace.value+"|"+std::string(to_string(fact.kind))+"|parent|"+parent_key+"|"+fact.qualified_name+"|"+signature+"|"+std::to_string(static_cast<int>(fact.linkage));if(fact.linkage==Linkage::Internal||fact.is_anonymous||fact.kind==SymbolKind::LocalVariable||fact.kind==SymbolKind::Lambda)key+="|file|"+source_file.value;}
            install_symbol(fact,key);it=pending.erase(it);progressed=true;
        }
        if(!progressed)throw std::runtime_error("canonical semantic-parent cycle among raw symbol facts");
    }
    for(const auto& fact:batch.symbols) {
        auto& symbol=model_.symbols.at(symbol_ids.at(fact.local_identity));symbol.exported|=fact.exported;const auto [_,parent]=parent_resolution(fact);
        if(parent){if(symbol.semantic_parent&&*symbol.semantic_parent!=*parent)throw std::runtime_error("conflicting canonical parents for symbol "+symbol.qualified_name+" (existing "+symbol.semantic_parent->value+", observed "+parent->value+", raw "+fact.local_identity+")");symbol.semantic_parent=*parent;}
        const auto role=fact.is_definition?OccurrenceRole::Definition:(fact.is_implicit?OccurrenceRole::Implicit:OccurrenceRole::Declaration);
        const auto location=semantic_range(fact.expansion_range);
        const OccurrenceID occurrence{sha256("occurrence|"+revision.value+"|"+symbol.id.value+"|"+location.file.value+"|"+std::to_string(location.begin_offset)+"|"+std::to_string(static_cast<int>(role)))};
        auto& observation = model_.occurrences[occurrence];
        observation.id=occurrence; observation.symbol=symbol.id; observation.revision=revision;
        observation.range=location; observation.role=role; observation.implicit=fact.is_implicit;
        if(const auto type=type_ids.find(fact.type_key);type!=type_ids.end()) observation.type=type->second;
        merge_properties(observation.function, fact.function);
        merge_properties(observation.variable, fact.variable);
        merge_properties(observation.templ, fact.templ);
        observation.exported |= fact.exported;
        if(parent){
            const RelationshipID parent_edge{sha256("semantic-parent|"+revision.value+"|"+symbol.id.value+"|"+parent->value+"|"+location.file.value+"|"+std::to_string(location.begin_offset))};
            Relationship relationship;relationship.id=parent_edge;relationship.kind=RelationshipKind::SemanticParent;relationship.source=symbol.id;relationship.target_symbol=*parent;relationship.observed_in=revision;relationship.evidence=location;relationship.resolution=ResolutionStatus::Exact;relationship.origin=EvidenceOrigin::DerivedCanonicalization;relationship.target_domain=TargetDomain::Project;
            model_.relationships[parent_edge]=std::move(relationship);
        }
    }

    auto find_usr=[&](const std::string& usr)->std::optional<SymbolID> {
        return global_usr(usr);
    };
    for(const auto& fact:batch.relationships) {
        const auto source_it=symbol_ids.find(fact.source_local_key); if(source_it==symbol_ids.end()) continue;
        std::optional<SymbolID> target;
        std::optional<TypeID> target_type;
        if(const auto it=symbol_ids.find(fact.target_local_key);it!=symbol_ids.end())target=it->second;
        if(!target&&!fact.target_usr.empty())target=find_usr(fact.target_usr);
        if(const auto it=type_ids.find(fact.target_type_key);it!=type_ids.end())target_type=it->second;
        auto status=fact.resolution;auto failure=fact.resolution_failure;
        if(!target && !target_type && status==ResolutionStatus::Exact)status=ResolutionStatus::Unresolved;
        if(!target&&!target_type&&failure==ResolutionFailure::None){
            if(fact.target_domain==TargetDomain::External)failure=ResolutionFailure::ExternalBoundary;
            else if(fact.target_domain==TargetDomain::Project)failure=ResolutionFailure::NoProjectSymbol;
            else if(fact.target_domain==TargetDomain::Dependent)failure=ResolutionFailure::DependentExpression;
            else if(fact.target_domain==TargetDomain::Indirect)failure=ResolutionFailure::IndirectFlow;
            else failure=ResolutionFailure::Unknown;
        }
        const auto evidence=semantic_range(fact.evidence);
        const RelationshipID id{sha256("relationship|"+revision.value+"|"+std::to_string(static_cast<int>(fact.kind))+"|"+source_it->second.value+"|"+(target?target->value:(target_type?target_type->value:fact.unresolved_target))+"|"+evidence.file.value+"|"+std::to_string(evidence.begin_offset))};
        Relationship relationship;relationship.id=id;relationship.kind=fact.kind;relationship.source=source_it->second;relationship.target_symbol=target;relationship.target_type=target_type;relationship.unresolved_target=fact.unresolved_target;relationship.observed_target_usr=fact.target_usr;relationship.observed_target_spelling=fact.observed_target_spelling;relationship.observed_in=revision;relationship.evidence=evidence;relationship.resolution=status;relationship.dispatch=fact.dispatch;relationship.origin=fact.origin;relationship.target_domain=fact.target_domain;relationship.resolution_failure=failure;relationship.access=fact.access;relationship.is_virtual=fact.is_virtual;relationship.is_dependent=fact.is_dependent;
        model_.relationships[id]=std::move(relationship);
    }

    std::map<std::string,MacroDefinitionID> macros;
    for(const auto& fact:batch.macro_definitions) {
        const auto location=semantic_range(fact.range); const MacroDefinitionID id{sha256("macro-definition|"+revision.value+"|"+fact.name+"|"+std::to_string(location.begin_offset))};
        model_.macro_definitions[id]={id,fact.name,fact.replacement,revision,location}; macros[fact.name]=id;
    }
    for(const auto& fact:batch.macro_expansions) {
        const auto location=semantic_range(fact.range); const MacroExpansionID id{sha256("macro-expansion|"+revision.value+"|"+fact.name+"|"+std::to_string(location.begin_offset))};
        std::optional<MacroDefinitionID> definition; if(const auto it=macros.find(fact.name);it!=macros.end()) definition=it->second;
        model_.macro_expansions[id]={id,fact.name,definition,revision,location};
    }
    for(const auto& fact:batch.includes) {
        const auto [_,including_version]=ensure_file(fact.including_path); const auto [included_file,__]=ensure_file(fact.included_path);
        const RelationshipID id{sha256("include|"+revision.value+"|"+including_version.value+"|"+included_file.value+"|"+std::to_string(fact.directive.offset))};
        model_.includes[id]={id,revision,including_version,included_file,fact.directive,fact.kind,fact.resolved};
    }
    for(const auto& fact:batch.diagnostics) {
        const auto location=semantic_range(fact.range); const DiagnosticID id{sha256("diagnostic|"+revision.value+"|"+std::to_string(fact.severity)+"|"+fact.message+"|"+std::to_string(location.begin_offset))};
        model_.diagnostics[id]={id,revision,fact.severity,fact.message,fact.option,location};
    }
    recompute_properties(model_);
    recompute_completeness(model_);
}

void Canonicalizer::garbage_collect() {
    std::set<SymbolID> supported; for(const auto& [_,occurrence]:model_.occurrences) supported.insert(occurrence.symbol);
    std::erase_if(model_.symbols,[&](const auto& item){return !supported.contains(item.first);});
    for(auto&[_,logical]:model_.logical_symbols)std::erase_if(logical.variants,[&](const auto& id){return !model_.symbols.contains(id);});
    std::erase_if(model_.logical_symbols,[](const auto& item){return item.second.variants.empty();});
    std::erase_if(model_.relationships,[&](const auto& item){return !model_.symbols.contains(item.second.source) || (item.second.target_symbol && !model_.symbols.contains(*item.second.target_symbol));});
    std::set<TypeID> used; for(const auto& [_,observation]:model_.occurrences)if(observation.type)used.insert(*observation.type); for(const auto& [_,symbol]:model_.symbols) if(symbol.type) used.insert(*symbol.type);for(const auto&[_,relationship]:model_.relationships)if(relationship.target_type)used.insert(*relationship.target_type);
    std::queue<TypeID> queue; for(const auto& id:used) queue.push(id);
    while(!queue.empty()) { const auto id=queue.front();queue.pop(); if(const auto it=model_.types.find(id);it!=model_.types.end()) for(const auto& child:it->second.children) if(used.insert(child).second) queue.push(child); }
    std::erase_if(model_.types,[&](const auto& item){return !used.contains(item.first);});
    std::set<FileVersionID> versions; for(const auto& [_,o]:model_.occurrences) versions.insert(o.range.file); for(const auto& [_,r]:model_.relationships) versions.insert(r.evidence.file); for(const auto& [_,i]:model_.includes) versions.insert(i.including_file); for(const auto& [_,d]:model_.diagnostics) versions.insert(d.range.file);
    for(const auto& [_,m]:model_.macro_definitions) versions.insert(m.range.file); for(const auto& [_,m]:model_.macro_expansions) versions.insert(m.range.file);
    // Includes persist the target FileID rather than a FileVersionID. Retain
    // every observed version of active include targets so the next incremental
    // build can compare the current header hash with a known version. Dropping
    // these rows made every included header look newly changed and forced all
    // translation units to be re-extracted on every otherwise no-op build.
    std::map<FileID,std::map<std::string,FileVersionID>> versions_by_file;
    for(const auto&[id,version]:model_.file_versions)versions_by_file[version.file].emplace(version.content_hash,id);
    std::set<FileID> included_files;for(const auto&[_,include]:model_.includes)included_files.insert(include.included_file);
    for(const auto& file_id:included_files){
        const auto file=model_.files.find(file_id);if(file==model_.files.end())continue;
        std::error_code error;const auto path=std::filesystem::path(file->second.path);std::string current="missing:"+sha256(file->second.path);
        if(std::filesystem::is_regular_file(path,error))current=read_file_hash(path);
        const auto by_file=versions_by_file.find(file_id);if(by_file==versions_by_file.end())continue;
        const auto version=by_file->second.find(current);if(version!=by_file->second.end())versions.insert(version->second);
    }
    for(const auto&[_,tu]:model_.translation_units){const auto active=model_.active_revision_by_tu.find(tu.id.value);if(active==model_.active_revision_by_tu.end())continue;const auto revision=model_.revisions.find(active->second);if(revision==model_.revisions.end())continue;const auto by_file=versions_by_file.find(tu.source);if(by_file==versions_by_file.end())continue;const auto version=by_file->second.find(revision->second.source_hash);if(version!=by_file->second.end())versions.insert(version->second);}
    std::erase_if(model_.file_versions,[&](const auto& item){return !versions.contains(item.first);});
    std::set<FileID> files; for(const auto& [_,v]:model_.file_versions) files.insert(v.file); for(const auto& [_,i]:model_.includes) files.insert(i.included_file); for(const auto& [_,tu]:model_.translation_units) files.insert(tu.source);
    std::erase_if(model_.files,[&](const auto& item){return !files.contains(item.first);});
    std::erase_if(model_.entry_roots,[&](const auto& item){return !model_.symbols.contains(item.second.symbol)||(item.second.configuration&&!model_.configurations.contains(*item.second.configuration));});
    std::erase_if(model_.target_memberships,[&](const auto& item){return !model_.build_targets.contains(item.second.target)||!model_.translation_units.contains(item.second.tu);});
    std::erase_if(model_.target_dependencies,[&](const auto& item){return !model_.build_targets.contains(item.second.consumer)||!model_.build_targets.contains(item.second.dependency);});
    std::erase_if(model_.indirect_call_summaries,[&](const auto& item){const auto& summary=item.second;return !model_.relationships.contains(summary.call_observation)||!model_.revisions.contains(summary.revision)||!model_.configurations.contains(summary.configuration)||!model_.symbols.contains(summary.storage_provider)||!model_.symbols.contains(summary.index_provider)||std::any_of(summary.stored_targets.begin(),summary.stored_targets.end(),[&](const auto& id){return !model_.symbols.contains(id);})||std::any_of(summary.selectable_targets.begin(),summary.selectable_targets.end(),[&](const auto& id){return !model_.symbols.contains(id);})||std::any_of(summary.modeled_address_observations.begin(),summary.modeled_address_observations.end(),[&](const auto& id){return !model_.relationships.contains(id);});});
    std::set<BuildTargetID> used_targets;for(const auto&[_,membership]:model_.target_memberships)used_targets.insert(membership.target);
    std::erase_if(model_.build_targets,[&](const auto& item){return !used_targets.contains(item.first);});
    recompute_properties(model_);
    recompute_completeness(model_);
}

ValidationResult SemanticModelValidator::validate(const SemanticModel& model) const {
    ValidationResult result; auto issue=[&](std::string code,std::string message){result.issues.push_back({std::move(code),std::move(message)});};
    if(!model.workspace.valid()) issue("identity.workspace","workspace ID is invalid");
    for(const auto&[id,configuration]:model.configurations){if(id.value!=sha256(configuration.canonical_key))issue("identity.configuration","configuration ID does not match its canonical key: "+id.value);if(configuration.fingerprint!=sha256("compilation-fingerprint|"+configuration.canonical_key))issue("identity.configuration-fingerprint","configuration fingerprint does not match its semantic key: "+id.value);}
    std::set<BuildConfigurationID> referenced_configurations;
    for(const auto& [id,tu]:model.translation_units) {
        if(!id.valid() || id!=tu.id) issue("identity.tu","translation unit map key mismatch");
        if(id.value!=sha256("tu|"+tu.source.value+"|"+tu.configuration.value))issue("identity.tu-key","translation unit ID does not match source/configuration: "+id.value);
        if(!model.files.contains(tu.source)) issue("reference.tu-file","translation unit source file is absent: "+id.value);
        if(!model.configurations.contains(tu.configuration)) issue("reference.tu-config","translation unit configuration is absent: "+id.value);
        referenced_configurations.insert(tu.configuration);
    }
    for(const auto&[id,_]:model.configurations)if(!referenced_configurations.contains(id))issue("lifetime.orphan-configuration","configuration has no translation unit: "+id.value);
    for(const auto& [id,revision]:model.revisions){if(!model.translation_units.contains(revision.tu))issue("reference.revision-tu","revision has no translation unit: "+id.value);if(id.value!=sha256("tu-revision|"+revision.tu.value+"|"+revision.source_hash+"|"+revision.compilation_fingerprint+"|"+std::to_string(revision.extractor_version)))issue("identity.revision","revision ID does not match its inputs: "+id.value);}
    for(const auto&[id,file]:model.files)if(id.value!=sha256("file|"+file.workspace.value+"|"+file.path))issue("identity.file","file ID does not match workspace/path: "+id.value);
    for(const auto& [id,version]:model.file_versions){if(!model.files.contains(version.file))issue("reference.file-version","file version has no file: "+id.value);if(id.value!=sha256("file-version|"+version.file.value+"|"+version.content_hash))issue("identity.file-version","file-version ID does not match file/content: "+id.value);}
    for(const auto& [id,occurrence]:model.occurrences) {
        if(occurrence.type&&!model.types.contains(*occurrence.type)) issue("reference.occurrence-type","occurrence has no type: "+id.value);
        if(!model.symbols.contains(occurrence.symbol)) issue("reference.occurrence-symbol","occurrence has no symbol: "+id.value);
        if(!model.revisions.contains(occurrence.revision)) issue("reference.occurrence-revision","occurrence has no revision: "+id.value);
        if(!model.file_versions.contains(occurrence.range.file)) issue("reference.occurrence-file","occurrence has no file version: "+id.value);
    }
    std::map<SymbolID, SymbolOccurrence> properties;
    for(const auto& [_,observation]:model.occurrences) {
        auto& aggregate=properties[observation.symbol];
        merge_properties(aggregate.function,observation.function);
        merge_properties(aggregate.variable,observation.variable);
        merge_properties(aggregate.templ,observation.templ);
        aggregate.exported|=observation.exported;
    }
    for(const auto& [id,symbol]:model.symbols) {
        const auto& aggregate=properties[id];
        if(function_state(symbol.function)!=function_state(aggregate.function) ||
            variable_state(symbol.variable)!=variable_state(aggregate.variable) ||
            template_state(symbol.templ)!=template_state(aggregate.templ) || symbol.exported!=aggregate.exported)
            issue("lifetime.symbol-properties","symbol properties differ from active observation evidence: "+id.value);
    }
    for(const auto& [id,relationship]:model.relationships) {
        if(!model.symbols.contains(relationship.source)) issue("reference.edge-source","relationship source is absent: "+id.value);
        if(relationship.target_symbol && !model.symbols.contains(*relationship.target_symbol)) issue("reference.edge-target","relationship target is absent: "+id.value);
        if(relationship.target_type && !model.types.contains(*relationship.target_type)) issue("reference.edge-type","relationship type target is absent: "+id.value);
        if(relationship.resolution==ResolutionStatus::Exact && !relationship.target_symbol && !relationship.target_type) issue("resolution.exact-without-target","exact relationship has no target: "+id.value);
        if(relationship.resolution==ResolutionStatus::Unresolved&&relationship.resolution_failure==ResolutionFailure::None)issue("resolution.unclassified","unresolved relationship has no failure classification: "+id.value);
        if(!model.revisions.contains(relationship.observed_in)) issue("reference.edge-revision","relationship revision is absent: "+id.value);
        if(!model.file_versions.contains(relationship.evidence.file)) issue("reference.edge-file","relationship evidence file is absent: "+id.value);
        if(relationship.kind==RelationshipKind::Inherits && relationship.target_symbol) {
            const auto target=model.symbols.find(*relationship.target_symbol); if(target!=model.symbols.end() && !base_target_kind(target->second.kind)) issue("hierarchy.base-kind","inheritance target is not a record, type alias, or dependent type parameter: "+id.value);
        }
        if(relationship.kind==RelationshipKind::Overrides && relationship.target_symbol) {
            const auto source=model.symbols.find(relationship.source),target=model.symbols.find(*relationship.target_symbol);
            if(source!=model.symbols.end() && !method_kind(source->second.kind)) issue("hierarchy.override-source","override source is not a method: "+id.value);
            if(target!=model.symbols.end() && !method_kind(target->second.kind)) issue("hierarchy.override-target","override target is not a method: "+id.value);
        }
    }
    std::map<std::string,std::string> symbol_keys,type_keys;
    for(const auto& [id,symbol]:model.symbols) { if(symbol.canonical_key.empty()) issue("identity.symbol-key","symbol key is empty: "+id.value); const auto [it,inserted]=symbol_keys.emplace(id.value,symbol.canonical_key); if(!inserted&&it->second!=symbol.canonical_key) issue("identity.symbol-collision","symbol ID collision: "+id.value);if(symbol.logical_symbol&&!model.logical_symbols.contains(*symbol.logical_symbol))issue("reference.symbol-logical","symbol references an absent logical symbol: "+id.value); }
    for(const auto&[id,logical]:model.logical_symbols){if(id.value!=sha256(logical.canonical_key))issue("identity.logical-symbol","logical symbol ID does not match its key: "+id.value);if(logical.clang_usr.empty())issue("identity.logical-symbol-usr","logical symbol has an empty USR: "+id.value);for(const auto& variant:logical.variants){const auto found=model.symbols.find(variant);if(found==model.symbols.end())issue("reference.logical-variant","logical symbol variant is absent: "+id.value);else if(!found->second.logical_symbol||*found->second.logical_symbol!=id)issue("reference.logical-membership","logical membership is not bidirectional: "+id.value);}}
    for(const auto& [id,type]:model.types) { if(type.canonical_key.empty()) issue("identity.type-key","type key is empty: "+id.value);if(id.value!=sha256(type.canonical_key))issue("identity.type-digest","type ID does not match canonical key: "+id.value); const auto [it,inserted]=type_keys.emplace(id.value,type.canonical_key); if(!inserted&&it->second!=type.canonical_key) issue("identity.type-collision","type ID collision: "+id.value); for(const auto& child:type.children) if(!model.types.contains(child)) issue("reference.type-child","type child is absent: "+id.value); }
    for(const auto&[id,symbol]:model.symbols){if(id.value!=sha256(symbol.canonical_key))issue("identity.symbol-digest","symbol ID does not match canonical key: "+id.value);if(symbol.type&&!model.types.contains(*symbol.type))issue("reference.symbol-type","symbol type is absent: "+id.value);if(symbol.semantic_parent&&!model.symbols.contains(*symbol.semantic_parent))issue("reference.symbol-parent","symbol parent is absent: "+id.value);}
    for(const auto&[id,macro]:model.macro_definitions){if(!model.revisions.contains(macro.revision))issue("reference.macro-revision","macro definition revision is absent: "+id.value);if(!model.file_versions.contains(macro.range.file))issue("reference.macro-file","macro definition file is absent: "+id.value);}
    for(const auto&[id,macro]:model.macro_expansions){if(!model.revisions.contains(macro.revision))issue("reference.expansion-revision","macro expansion revision is absent: "+id.value);if(!model.file_versions.contains(macro.range.file))issue("reference.expansion-file","macro expansion file is absent: "+id.value);if(macro.definition&&!model.macro_definitions.contains(*macro.definition))issue("reference.expansion-definition","macro expansion definition is absent: "+id.value);}
    for(const auto&[id,include]:model.includes){if(!model.revisions.contains(include.revision))issue("reference.include-revision","include revision is absent: "+id.value);if(!model.file_versions.contains(include.including_file))issue("reference.include-source","including file version is absent: "+id.value);if(!model.files.contains(include.included_file))issue("reference.include-target","included file is absent: "+id.value);}
    for(const auto&[id,diagnostic]:model.diagnostics){if(!model.revisions.contains(diagnostic.revision))issue("reference.diagnostic-revision","diagnostic revision is absent: "+id.value);if(!model.file_versions.contains(diagnostic.range.file))issue("reference.diagnostic-file","diagnostic file is absent: "+id.value);}
    for(const auto&[id,target]:model.build_targets)if(id.value!=sha256("build-target|"+model.workspace.value+"|"+target.source+"|"+target.name))issue("identity.build-target","build target ID does not match its inputs: "+id.value);
    for(const auto&[id,membership]:model.target_memberships){if(!model.build_targets.contains(membership.target))issue("reference.target-membership-target","target membership has no target: "+id.value);if(!model.translation_units.contains(membership.tu))issue("reference.target-membership-tu","target membership has no TU: "+id.value);if(id.value!=sha256("target-membership|"+membership.target.value+"|"+membership.tu.value))issue("identity.target-membership","target membership ID does not match its inputs: "+id.value);}
    for(const auto&[id,dependency]:model.target_dependencies){if(!model.build_targets.contains(dependency.consumer)||!model.build_targets.contains(dependency.dependency))issue("reference.target-dependency","target dependency endpoint is absent: "+id.value);if(id.value!=sha256("target-dependency|"+dependency.consumer.value+"|"+dependency.dependency.value))issue("identity.target-dependency","target dependency ID does not match its inputs: "+id.value);}
    for(const auto&[id,root]:model.entry_roots){if(!model.symbols.contains(root.symbol))issue("reference.entry-root-symbol","entry root has no symbol: "+id.value);if(root.configuration&&!model.configurations.contains(*root.configuration))issue("reference.entry-root-configuration","entry root has no configuration: "+id.value);if(id.value!=sha256("entry-root|"+root.symbol.value+"|"+std::to_string(static_cast<int>(root.kind))+"|"+root.reason+"|"+(root.configuration?root.configuration->value:std::string{})))issue("identity.entry-root","entry root ID does not match its inputs: "+id.value);}
    for(const auto&[id,summary]:model.indirect_call_summaries){
        const auto key="indirect-summary|"+summary.call_observation.value+"|"+summary.revision.value+"|"+summary.configuration.value+"|"+std::to_string(summary.analyzer_version);
        if(id.value!=sha256(key))issue("identity.indirect-summary","indirect-call summary ID does not match its inputs: "+id.value);
        if(!model.relationships.contains(summary.call_observation))issue("reference.indirect-call","indirect-call summary observation is absent: "+id.value);
        if(!model.revisions.contains(summary.revision)||!model.configurations.contains(summary.configuration))issue("reference.indirect-revision","indirect-call summary revision/configuration is absent: "+id.value);
        if(!model.symbols.contains(summary.storage_provider)||!model.symbols.contains(summary.index_provider))issue("reference.indirect-provider","indirect-call summary provider is absent: "+id.value);
        for(const auto& symbol:summary.stored_targets)if(!model.symbols.contains(symbol))issue("reference.indirect-stored-target","indirect-call stored target is absent: "+id.value);
        for(const auto& symbol:summary.selectable_targets)if(!summary.stored_targets.contains(symbol))issue("reference.indirect-selectable-target","selectable target was not stored in the modeled table: "+id.value);
        for(const auto& relationship:summary.modeled_address_observations)if(!model.relationships.contains(relationship))issue("reference.indirect-address","modeled address observation is absent: "+id.value);
        if(summary.complete&&(summary.storage_escapes||summary.selectable_targets.empty()))issue("analysis.indirect-unsound-complete","complete indirect-call summary is escaping or has no targets: "+id.value);
    }
    for(const auto&[id,_]:model.revisions){bool active{};for(const auto&[__,revision]:model.active_revision_by_tu)active|=revision==id;if(!active)issue("lifetime.stale-revision","revision is not active: "+id.value);}
    for(const auto&[id,_]:model.translation_units)if(!model.active_revision_by_tu.contains(id.value))issue("revision.missing-active","translation unit has no active revision: "+id.value);
    if(model.active_revision_by_tu.size()!=model.translation_units.size()||model.revisions.size()!=model.translation_units.size())issue("revision.cardinality","every translation unit must have exactly one active revision");
    // Validate every parent chain once. Rebuilding a set and walking to the
    // root for every symbol made this O(symbols * hierarchy depth), which is
    // prohibitively expensive for large C++ models.
    std::map<SymbolID,unsigned char> parent_state;
    for(const auto&[id,_]:model.symbols){
        if(parent_state[id]==2)continue;
        std::vector<SymbolID> path;auto current=id;
        while(true){
            const auto state=parent_state[current];
            if(state==1){issue("hierarchy.parent-cycle","semantic parent cycle at: "+id.value);break;}
            if(state==2)break;
            parent_state[current]=1;path.push_back(current);
            const auto it=model.symbols.find(current);if(it==model.symbols.end()||!it->second.semantic_parent)break;
            current=*it->second.semantic_parent;
        }
        for(const auto& symbol:path)parent_state[symbol]=2;
    }
    std::set<SymbolID> observed; for(const auto& [_,occurrence]:model.occurrences) observed.insert(occurrence.symbol); for(const auto& [id,_]:model.symbols) if(!observed.contains(id)) issue("lifetime.orphan-symbol","symbol has no observation: "+id.value);
    for(const auto& [key,revision]:model.active_revision_by_tu) { const auto it=model.revisions.find(revision); if(it==model.revisions.end() || it->second.tu.value!=key) issue("revision.active-map","active revision map is inconsistent: "+key); }
    ModelCompleteness completeness{};for(const auto&[_,revision]:model.revisions){++completeness.total_tus;switch(revision.quality){case ExtractionQuality::Complete:++completeness.complete_tus;break;case ExtractionQuality::CompleteWithWarnings:++completeness.warning_tus;break;case ExtractionQuality::Partial:++completeness.partial_tus;break;case ExtractionQuality::Failed:++completeness.failed_tus;break;}}
    if(model.completeness.total_tus!=completeness.total_tus||model.completeness.complete_tus!=completeness.complete_tus||model.completeness.warning_tus!=completeness.warning_tus||model.completeness.partial_tus!=completeness.partial_tus||model.completeness.failed_tus!=completeness.failed_tus)issue("publication.completeness","published completeness does not match active revisions");
    for(const auto&[id,relationship]:model.relationships)if(relationship.kind==RelationshipKind::SemanticParent&&relationship.target_symbol){const auto source=model.symbols.find(relationship.source);if(source==model.symbols.end()||!source->second.semantic_parent||*source->second.semantic_parent!=*relationship.target_symbol)issue("hierarchy.semantic-parent-edge","semantic-parent edge disagrees with canonical symbol parent: "+id.value);}
    return result;
}

std::string normalized_snapshot(const SemanticModel& model) {
    std::ostringstream out;
    out<<"workspace|"<<model.workspace.value<<"|"<<normalize_path(model.workspace_path)<<'\n';
    for(const auto& [id,config]:model.configurations)out<<"config|"<<id.value<<'|'<<config.canonical_key<<'|'<<config.fingerprint<<'\n';
    for(const auto& [id,file]:model.files)out<<"file|"<<id.value<<'|'<<file.workspace.value<<'|'<<file.path<<'|'<<file.external<<'\n';
    for(const auto&[id,version]:model.file_versions)out<<"file-version|"<<id.value<<'|'<<version.file.value<<'|'<<version.content_hash<<'|'<<version.size<<'\n';
    for(const auto&[id,tu]:model.translation_units)out<<"tu|"<<id.value<<'|'<<tu.source.value<<'|'<<tu.configuration.value<<'\n';
    for(const auto&[id,revision]:model.revisions)out<<"revision|"<<id.value<<'|'<<revision.tu.value<<'|'<<revision.source_hash<<'|'<<revision.compilation_fingerprint<<'|'<<revision.extractor_version<<'|'<<static_cast<int>(revision.quality)<<'\n';
    for(const auto& [id,type]:model.types){out<<"type|"<<id.value<<'|'<<type.canonical_key<<'|'<<static_cast<int>(type.kind)<<'|'<<type.qualifiers.is_const<<type.qualifiers.is_volatile<<type.qualifiers.is_restrict<<'|'<<type.spelling<<'|'<<type.canonical_spelling<<'|'<<(type.named_symbol?type.named_symbol->value:"")<<'|'<<(type.array_extent?std::to_string(*type.array_extent):"")<<'|'<<type.variadic<<'|'<<type.dependent;for(const auto& child:type.children)out<<"|child:"<<child.value;for(const auto& argument:type.template_arguments)out<<"|argument:"<<argument;out<<'\n';}
    for(const auto&[id,logical]:model.logical_symbols){out<<"logical-symbol|"<<id.value<<'|'<<logical.canonical_key<<'|'<<logical.clang_usr<<'|'<<static_cast<int>(logical.linkage);for(const auto& variant:logical.variants)out<<"|variant:"<<variant.value;out<<'\n';}
    for(const auto& [id,symbol]:model.symbols)out<<"symbol|"<<id.value<<'|'<<symbol.canonical_key<<'|'<<symbol.usr<<'|'<<(symbol.logical_symbol?symbol.logical_symbol->value:"")<<'|'<<static_cast<int>(symbol.kind)<<'|'<<symbol.name<<'|'<<symbol.qualified_name<<'|'<<static_cast<int>(symbol.linkage)<<'|'<<static_cast<int>(symbol.visibility)<<'|'<<(symbol.semantic_parent?symbol.semantic_parent->value:"")<<'|'<<(symbol.type?symbol.type->value:"")<<'|'<<function_state(symbol.function)<<'|'<<variable_state(symbol.variable)<<'|'<<template_state(symbol.templ)<<'|'<<symbol.exported<<'\n';
    auto write_range=[&](const SourceRange& range){out<<range.file.value<<'|'<<range.begin_line<<':'<<range.begin_column<<':'<<range.begin_offset<<'-'<<range.end_line<<':'<<range.end_column<<':'<<range.end_offset;};
    for(const auto&[id,occurrence]:model.occurrences){out<<"occurrence|"<<id.value<<'|'<<occurrence.symbol.value<<'|'<<occurrence.revision.value<<'|';write_range(occurrence.range);out<<'|'<<static_cast<int>(occurrence.role)<<'|'<<occurrence.implicit<<'|'<<(occurrence.type?occurrence.type->value:std::string{})<<'|'<<function_state(occurrence.function)<<'|'<<variable_state(occurrence.variable)<<'|'<<template_state(occurrence.templ)<<'|'<<occurrence.exported<<'\n';}
    for(const auto&[id,relationship]:model.relationships){out<<"relationship|"<<id.value<<'|'<<static_cast<int>(relationship.kind)<<'|'<<relationship.source.value<<'|'<<(relationship.target_symbol?relationship.target_symbol->value:"")<<'|'<<(relationship.target_type?relationship.target_type->value:"")<<'|'<<relationship.unresolved_target<<'|'<<relationship.observed_target_usr<<'|'<<relationship.observed_target_spelling<<'|'<<relationship.observed_in.value<<'|';write_range(relationship.evidence);out<<'|'<<static_cast<int>(relationship.resolution)<<'|'<<static_cast<int>(relationship.dispatch)<<'|'<<static_cast<int>(relationship.origin)<<'|'<<static_cast<int>(relationship.target_domain)<<'|'<<static_cast<int>(relationship.resolution_failure)<<'|'<<static_cast<int>(relationship.access)<<'|'<<relationship.is_virtual<<'|'<<relationship.is_dependent<<'\n';}
    for(const auto&[id,macro]:model.macro_definitions){out<<"macro-definition|"<<id.value<<'|'<<macro.name<<'|'<<macro.replacement<<'|'<<macro.revision.value<<'|';write_range(macro.range);out<<'\n';}
    for(const auto&[id,macro]:model.macro_expansions){out<<"macro-expansion|"<<id.value<<'|'<<macro.name<<'|'<<(macro.definition?macro.definition->value:"")<<'|'<<macro.revision.value<<'|';write_range(macro.range);out<<'\n';}
    for(const auto&[id,include]:model.includes)out<<"include|"<<id.value<<'|'<<include.revision.value<<'|'<<include.including_file.value<<'|'<<include.included_file.value<<'|'<<include.directive.path<<':'<<include.directive.line<<':'<<include.directive.column<<':'<<include.directive.offset<<'|'<<static_cast<int>(include.kind)<<'|'<<include.resolved<<'\n';
    for(const auto&[id,diagnostic]:model.diagnostics){out<<"diagnostic|"<<id.value<<'|'<<diagnostic.revision.value<<'|'<<diagnostic.severity<<'|'<<diagnostic.message<<'|'<<diagnostic.option<<'|';write_range(diagnostic.range);out<<'\n';}
    for(const auto&[id,target]:model.build_targets)out<<"build-target|"<<id.value<<'|'<<target.name<<'|'<<static_cast<int>(target.kind)<<'|'<<target.source<<'|'<<target.topology_authoritative<<'\n';
    for(const auto&[id,membership]:model.target_memberships)out<<"target-membership|"<<id.value<<'|'<<membership.target.value<<'|'<<membership.tu.value<<'\n';
    for(const auto&[id,dependency]:model.target_dependencies)out<<"target-dependency|"<<id.value<<'|'<<dependency.consumer.value<<'|'<<dependency.dependency.value<<'\n';
    for(const auto&[id,root]:model.entry_roots)out<<"entry-root|"<<id.value<<'|'<<root.symbol.value<<'|'<<static_cast<int>(root.kind)<<'|'<<root.reason<<'|'<<(root.configuration?root.configuration->value:std::string{})<<'\n';
    for(const auto&[id,summary]:model.indirect_call_summaries){out<<"indirect-summary|"<<id.value<<'|'<<summary.call_observation.value<<'|'<<summary.revision.value<<'|'<<summary.configuration.value<<'|'<<summary.analyzer_version<<'|'<<summary.storage_provider.value<<'|'<<summary.index_provider.value<<'|'<<static_cast<int>(summary.index_domain.kind)<<'|'<<summary.index_domain.minimum<<'|'<<summary.index_domain.maximum<<'|'<<summary.index_domain.known_zero_mask<<'|'<<summary.storage_escapes<<'|'<<summary.complete<<'|'<<summary.reason;for(const auto value:summary.index_domain.exact_values)out<<"|value:"<<value;for(const auto& symbol:summary.stored_targets)out<<"|stored:"<<symbol.value;for(const auto& symbol:summary.selectable_targets)out<<"|selected:"<<symbol.value;for(const auto& relationship:summary.modeled_address_observations)out<<"|address:"<<relationship.value;out<<'\n';}
    for(const auto&[tu,revision]:model.active_revision_by_tu)out<<"active|"<<tu<<'|'<<revision.value<<'\n';
    out<<"completeness|"<<model.completeness.total_tus<<'|'<<model.completeness.complete_tus<<'|'<<model.completeness.warning_tus<<'|'<<model.completeness.partial_tus<<'|'<<model.completeness.failed_tus<<'\n';
    return out.str();
}

} // namespace codeinsight
