#include "codeinsight/codeinsight.hpp"

#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace codeinsight {
namespace {

std::string record_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for(const char c:value) {
        if(c=='\\'||c=='|')result.push_back('\\');
        if(c=='\n')result += "\\n";
        else if(c=='\r')result += "\\r";
        else result.push_back(c);
    }
    return result;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size()+8);
    for(const unsigned char c:value) {
        switch(c) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if(c<0x20) {
                static constexpr char digits[]="0123456789abcdef";
                result += "\\u00";
                result.push_back(digits[c>>4]);
                result.push_back(digits[c&15]);
            } else result.push_back(static_cast<char>(c));
        }
    }
    return result;
}

std::string symbol_name(const SemanticModel& model,const SymbolID& id) {
    const auto it=model.symbols.find(id);
    return it==model.symbols.end()?std::string{}:it->second.qualified_name;
}

std::string target_name(const SemanticModel& model,const Relationship& relationship) {
    if(relationship.target_symbol)return symbol_name(model,*relationship.target_symbol);
    if(relationship.target_type) {
        const auto type=model.types.find(*relationship.target_type);
        if(type!=model.types.end())return type->second.canonical_spelling;
    }
    return relationship.unresolved_target;
}

std::string configuration_for(const SemanticModel& model,const TranslationUnitRevisionID& revision_id) {
    const auto revision=model.revisions.find(revision_id);
    if(revision==model.revisions.end())return {};
    const auto tu=model.translation_units.find(revision->second.tu);
    return tu==model.translation_units.end()?std::string{}:tu->second.configuration.value;
}

void emit_symbol(std::ostringstream& out,const Symbol& symbol,bool json_lines) {
    if(json_lines) {
        out<<"{\"record\":\"symbol\",\"id\":\""<<json_escape(symbol.id.value)
           <<"\",\"logical_symbol_id\":\""<<(symbol.logical_symbol?symbol.logical_symbol->value:std::string{})
           <<"\",\"kind\":\""<<to_string(symbol.kind)<<"\",\"name\":\""
           <<json_escape(symbol.qualified_name)<<"\",\"exported\":"<<(symbol.exported?"true":"false")<<"}\n";
    } else {
        out<<"symbol|"<<to_string(symbol.kind)<<'|'<<record_escape(symbol.qualified_name)<<'|'<<symbol.id.value<<"|logical:"<<(symbol.logical_symbol?symbol.logical_symbol->value:std::string{})<<"|exported:"<<symbol.exported<<'\n';
    }
}

void emit_relationship(std::ostringstream& out,const SemanticModel& model,const Relationship& relationship,
                       std::size_t evidence_count,const std::set<std::string>& configurations,bool json_lines) {
    std::string configuration_list;
    for(const auto& configuration:configurations) {
        if(!configuration_list.empty())configuration_list.push_back(',');
        configuration_list+=configuration;
    }
    const auto source=symbol_name(model,relationship.source);
    const auto target=target_name(model,relationship);
    if(json_lines) {
        out<<"{\"record\":\"relationship\",\"kind\":\""<<to_string(relationship.kind)
           <<"\",\"source_id\":\""<<relationship.source.value<<"\",\"source\":\""<<json_escape(source)
           <<"\",\"target_symbol_id\":\""<<(relationship.target_symbol?relationship.target_symbol->value:std::string{})
           <<"\",\"target_type_id\":\""<<(relationship.target_type?relationship.target_type->value:std::string{})
           <<"\",\"target\":\""<<json_escape(target)<<"\",\"resolution\":\""<<to_string(relationship.resolution)
           <<"\",\"observed_target_usr\":\""<<json_escape(relationship.observed_target_usr)<<"\",\"observed_target_spelling\":\""<<json_escape(relationship.observed_target_spelling)
           <<"\",\"target_domain\":\""<<to_string(relationship.target_domain)<<"\",\"resolution_failure\":\""<<to_string(relationship.resolution_failure)
           <<"\",\"dispatch\":\""<<to_string(relationship.dispatch)<<"\",\"origin\":\""<<to_string(relationship.origin)
           <<"\",\"evidence_count\":"<<evidence_count<<",\"configurations\":\""<<configuration_list<<"\"}\n";
    } else {
        out<<"relationship|"<<to_string(relationship.kind)<<'|'<<record_escape(source)<<'|'<<record_escape(target)
           <<'|'<<to_string(relationship.resolution)<<'|'<<to_string(relationship.dispatch)<<'|'<<to_string(relationship.origin)
           <<"|domain:"<<to_string(relationship.target_domain)<<"|failure:"<<to_string(relationship.resolution_failure)<<"|observed-usr:"<<record_escape(relationship.observed_target_usr)<<"|observed-spelling:"<<record_escape(relationship.observed_target_spelling)<<"|evidence:"<<evidence_count<<"|configurations:"<<configuration_list<<'\n';
    }
}

using EdgeKey=std::tuple<RelationshipKind,SymbolID,std::optional<SymbolID>,std::optional<TypeID>,std::string,std::string,std::string,
                         ResolutionStatus,DispatchKind,EvidenceOrigin,TargetDomain,ResolutionFailure,AccessSpecifier,bool,bool>;

EdgeKey edge_key(const Relationship& relationship) {
    return {relationship.kind,relationship.source,relationship.target_symbol,relationship.target_type,
            relationship.unresolved_target,relationship.observed_target_usr,relationship.observed_target_spelling,relationship.resolution,relationship.dispatch,relationship.origin,relationship.target_domain,relationship.resolution_failure,
            relationship.access,relationship.is_virtual,relationship.is_dependent};
}

void emit_ownership(std::ostringstream& out,const SemanticModel& model,bool json_lines){
    for(const auto&[_,target]:model.build_targets){
        if(json_lines)out<<"{\"record\":\"build_target\",\"id\":\""<<target.id.value<<"\",\"name\":\""<<json_escape(target.name)<<"\",\"kind\":\""<<to_string(target.kind)<<"\",\"source\":\""<<json_escape(target.source)<<"\",\"topology_authoritative\":"<<(target.topology_authoritative?"true":"false")<<"}\n";
        else out<<"build-target|"<<record_escape(target.name)<<'|'<<to_string(target.kind)<<'|'<<record_escape(target.source)<<'|'<<target.id.value<<"|authoritative:"<<target.topology_authoritative<<'\n';
    }
    for(const auto&[_,dependency]:model.target_dependencies){if(json_lines)out<<"{\"record\":\"target_dependency\",\"consumer_id\":\""<<dependency.consumer.value<<"\",\"dependency_id\":\""<<dependency.dependency.value<<"\"}\n";else out<<"target-dependency|"<<dependency.consumer.value<<'|'<<dependency.dependency.value<<'\n';}
    for(const auto&[_,root]:model.entry_roots){
        const auto name=symbol_name(model,root.symbol);
        if(json_lines)out<<"{\"record\":\"entry_root\",\"symbol_id\":\""<<root.symbol.value<<"\",\"symbol\":\""<<json_escape(name)<<"\",\"kind\":\""<<to_string(root.kind)<<"\",\"reason\":\""<<json_escape(root.reason)<<"\",\"configuration\":\""<<(root.configuration?root.configuration->value:std::string{})<<"\"}\n";
        else out<<"entry-root|"<<to_string(root.kind)<<'|'<<record_escape(name)<<'|'<<record_escape(root.reason)<<"|configuration:"<<(root.configuration?root.configuration->value:std::string{})<<'\n';
    }
    for(const auto&[_,summary]:model.indirect_call_summaries){
        if(json_lines)out<<"{\"record\":\"indirect_call_summary\",\"id\":\""<<summary.id.value<<"\",\"call_observation\":\""<<summary.call_observation.value<<"\",\"analyzer_version\":"<<summary.analyzer_version<<",\"domain_minimum\":"<<summary.index_domain.minimum<<",\"domain_maximum\":"<<summary.index_domain.maximum<<",\"stored_target_count\":"<<summary.stored_targets.size()<<",\"selectable_target_count\":"<<summary.selectable_targets.size()<<",\"storage_escapes\":"<<(summary.storage_escapes?"true":"false")<<",\"complete\":"<<(summary.complete?"true":"false")<<",\"reason\":\""<<json_escape(summary.reason)<<"\"}\n";
        else out<<"indirect-call-summary|"<<summary.id.value<<"|call:"<<summary.call_observation.value<<"|version:"<<summary.analyzer_version<<"|domain:"<<summary.index_domain.minimum<<".."<<summary.index_domain.maximum<<"|stored:"<<summary.stored_targets.size()<<"|selectable:"<<summary.selectable_targets.size()<<"|escapes:"<<summary.storage_escapes<<"|complete:"<<summary.complete<<'|'<<record_escape(summary.reason)<<'\n';
    }
}

} // namespace

std::string export_observations(const SemanticModel& model,bool json_lines) {
    std::ostringstream out;
    for(const auto&[_,symbol]:model.symbols)emit_symbol(out,symbol,json_lines);
    for(const auto&[_,relationship]:model.relationships) {
        std::set<std::string> configurations;
        const auto configuration=configuration_for(model,relationship.observed_in);
        if(!configuration.empty())configurations.insert(configuration);
        emit_relationship(out,model,relationship,1,configurations,json_lines);
    }
    emit_ownership(out,model,json_lines);
    return out.str();
}

std::string export_semantic_edges(const SemanticModel& model,bool json_lines) {
    struct Aggregate { const Relationship* representative{};std::size_t count{};std::set<std::string> configurations; };
    std::map<EdgeKey,Aggregate> aggregates;
    for(const auto&[_,relationship]:model.relationships) {
        auto& aggregate=aggregates[edge_key(relationship)];
        if(!aggregate.representative)aggregate.representative=&relationship;
        ++aggregate.count;
        const auto configuration=configuration_for(model,relationship.observed_in);
        if(!configuration.empty())aggregate.configurations.insert(configuration);
    }
    std::ostringstream out;
    for(const auto&[_,symbol]:model.symbols)emit_symbol(out,symbol,json_lines);
    for(const auto&[_,aggregate]:aggregates)emit_relationship(out,model,*aggregate.representative,aggregate.count,aggregate.configurations,json_lines);
    emit_ownership(out,model,json_lines);
    return out.str();
}

} // namespace codeinsight
