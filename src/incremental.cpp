#include "codeinsight/semantic/call_resolution.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace codeinsight {
namespace {

TranslationUnitID tu_id(const CompilationCommand& command){return TranslationUnitID{sha256("tu|"+command.source.value+"|"+command.configuration.value)};}

std::filesystem::path checkpoint_path(const std::filesystem::path& output){auto path=output;path+=L".checkpoint";return path;}

bool environment_flag(const char* name){
#ifdef _WIN32
    char* value{};std::size_t size{};if(_dupenv_s(&value,&size,name)!=0)return false;const bool enabled=value&&*value;std::free(value);return enabled;
#else
    const auto* value=std::getenv(name);return value&&*value;
#endif
}

BuildTargetKind manifest_target_kind(std::string_view value){
    if(value=="executable")return BuildTargetKind::Executable;if(value=="static-library")return BuildTargetKind::StaticLibrary;if(value=="shared-library")return BuildTargetKind::SharedLibrary;if(value=="object-library")return BuildTargetKind::ObjectLibrary;if(value=="test")return BuildTargetKind::Test;throw std::runtime_error("unknown target kind in target manifest: "+std::string(value));
}

std::vector<std::string> fields(std::string_view line){std::vector<std::string> result;std::size_t begin{};while(begin<=line.size()){const auto end=line.find('|',begin);result.emplace_back(line.substr(begin,end==std::string_view::npos?line.size()-begin:end-begin));if(end==std::string_view::npos)break;begin=end+1;}return result;}

void load_target_manifest(const IndexOptions& options,WorkspaceID workspace,const std::vector<CompilationCommand>& commands,std::map<BuildTargetID,BuildTarget>& targets,std::set<std::pair<BuildTargetID,TranslationUnitID>>& memberships,std::map<TargetDependencyID,TargetDependency>& dependencies){
    const auto manifest_path=normalize_path(options.target_manifest);const auto content=read_text_file(manifest_path);std::map<std::string,BuildTargetID> ids;std::vector<std::pair<std::string,std::string>> declared_memberships,declared_dependencies;
    std::size_t line_number{};for(std::size_t begin{};begin<=content.size();){const auto end=content.find('\n',begin);auto line=std::string_view(content).substr(begin,end==std::string::npos?content.size()-begin:end-begin);++line_number;if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);if(!line.empty()&&line.front()!='#'){
            const auto item=fields(line);if(item.size()==3&&item[0]=="target"){if(item[1].empty()||ids.contains(item[1]))throw std::runtime_error("duplicate or empty target at manifest line "+std::to_string(line_number));const BuildTargetID id{sha256("build-target|"+workspace.value+"|"+manifest_path+"|"+item[1])};ids.emplace(item[1],id);targets.emplace(id,BuildTarget{id,item[1],manifest_target_kind(item[2]),manifest_path,true});}
            else if(item.size()==3&&item[0]=="member")declared_memberships.emplace_back(item[1],item[2]);
            else if(item.size()==3&&item[0]=="depends")declared_dependencies.emplace_back(item[1],item[2]);
            else throw std::runtime_error("invalid target manifest record at line "+std::to_string(line_number));
        }if(end==std::string::npos)break;begin=end+1;}
    if(targets.empty())throw std::runtime_error("target manifest declares no targets");
    std::map<std::string,std::vector<TranslationUnitID>> tus_by_source;for(const auto& command:commands)tus_by_source[normalize_path(command.source_path)].push_back(tu_id(command));
    std::set<TranslationUnitID> assigned;
    for(const auto&[name,path]:declared_memberships){const auto target=ids.find(name);if(target==ids.end())throw std::runtime_error("target manifest member references unknown target: "+name);const auto normalized=normalize_path(path,options.workspace);const auto found=tus_by_source.find(normalized);if(found==tus_by_source.end())throw std::runtime_error("target manifest member has no compilation command: "+normalized);for(const auto& tu:found->second){memberships.insert({target->second,tu});assigned.insert(tu);}}
    for(const auto& command:commands)if(!assigned.contains(tu_id(command)))throw std::runtime_error("authoritative target manifest leaves a translation unit unassigned: "+normalize_path(command.source_path));
    for(const auto&[consumer_name,dependency_name]:declared_dependencies){const auto consumer=ids.find(consumer_name),dependency=ids.find(dependency_name);if(consumer==ids.end()||dependency==ids.end())throw std::runtime_error("target manifest dependency references an unknown target");const TargetDependencyID id{sha256("target-dependency|"+consumer->second.value+"|"+dependency->second.value)};dependencies.emplace(id,TargetDependency{id,consumer->second,dependency->second});}
}

std::set<FileID> changed_dependencies(const SemanticModel& model){
    std::set<FileID> changed;
    for(const auto&[id,file]:model.files){
        std::error_code error;const auto path=std::filesystem::path(file.path);
        std::string current="missing:"+sha256(file.path);
        if(std::filesystem::is_regular_file(path,error))current=read_file_hash(path);
        bool known{};for(const auto&[_,version]:model.file_versions)if(version.file==id&&version.content_hash==current){known=true;break;}
        if(!known)changed.insert(id);
    }
    bool added=true;
    while(added){
        added=false;
        for(const auto&[_,include]:model.includes)if(changed.contains(include.included_file)){
            const auto version=model.file_versions.find(include.including_file);if(version!=model.file_versions.end()&&changed.insert(version->second.file).second)added=true;
        }
    }
    return changed;
}

void refresh_entry_roots(SemanticModel& model,const std::vector<std::string>& manual_roots){
    model.entry_roots.clear();std::map<SymbolID,std::set<RelationshipID>> address_observations;std::set<RelationshipID> modeled_addresses;
    for(const auto&[id,edge]:model.relationships)if(edge.kind==RelationshipKind::TakesAddress&&edge.target_symbol)address_observations[*edge.target_symbol].insert(id);
    for(const auto&[_,summary]:model.indirect_call_summaries)if(summary.complete&&!summary.storage_escapes)modeled_addresses.insert(summary.modeled_address_observations.begin(),summary.modeled_address_observations.end());
    std::map<SymbolID,std::set<BuildConfigurationID>> configurations;
    for(const auto&[_,occurrence]:model.occurrences){const auto revision=model.revisions.find(occurrence.revision);if(revision==model.revisions.end())continue;const auto tu=model.translation_units.find(revision->second.tu);if(tu!=model.translation_units.end())configurations[occurrence.symbol].insert(tu->second.configuration);}
    auto add=[&](const Symbol& symbol,EntryRootKind kind,std::string reason){
        const auto found=configurations.find(symbol.id);if(found==configurations.end()||found->second.empty())return;
        for(const auto& configuration:found->second){const EntryRootID id{sha256("entry-root|"+symbol.id.value+"|"+std::to_string(static_cast<int>(kind))+"|"+reason+"|"+configuration.value)};model.entry_roots[id]={id,symbol.id,kind,reason,configuration};}
    };
    for(const auto&[_,symbol]:model.symbols){
        if(symbol.kind==SymbolKind::Function&&(symbol.name=="main"||symbol.name=="wmain"||symbol.name=="WinMain"||symbol.name=="wWinMain"))add(symbol,EntryRootKind::ProcessEntry,"process entry point");
        if(symbol.exported)add(symbol,EntryRootKind::Export,"compiler-observed export");
        if(const auto found=address_observations.find(symbol.id);found!=address_observations.end()&&!std::includes(modeled_addresses.begin(),modeled_addresses.end(),found->second.begin(),found->second.end()))add(symbol,EntryRootKind::AddressTaken,"address taken; at least one indirect flow is unresolved or escaping");
        if(std::find(manual_roots.begin(),manual_roots.end(),symbol.qualified_name)!=manual_roots.end())add(symbol,EntryRootKind::Manual,"configured entry root");
    }
}

} // namespace

IndexResult IncrementalExecutor::build(const IndexOptions& options)const{
    if(options.workspace.empty()||options.compile_commands.empty()||options.output.empty())throw std::invalid_argument("workspace, compile_commands, and output are required");
    const auto profile_start=std::chrono::steady_clock::now();auto profile_last=profile_start;
    const bool profile=environment_flag("CODEINSIGHT_PROFILE");
    const auto mark=[&](std::string_view phase){if(!profile)return;const auto now=std::chrono::steady_clock::now();std::cerr<<"profile|"<<phase<<"|delta-ms:"<<std::chrono::duration_cast<std::chrono::milliseconds>(now-profile_last).count()<<"|total-ms:"<<std::chrono::duration_cast<std::chrono::milliseconds>(now-profile_start).count()<<'\n';profile_last=now;};
    const auto frontend=frontend_version();
    if(!frontend.supported())throw std::runtime_error("unsupported semantic frontend: "+frontend.runtime+"; CodeInsight requires Clang 20 or newer for current MSVC toolchains");
    const auto workspace_path=normalize_path(options.workspace);
    const WorkspaceID workspace{sha256("workspace|"+workspace_path)};
    SQLiteSnapshotStore store;SemanticModel model;bool prior{},resumed_checkpoint{};const auto checkpoint=checkpoint_path(options.output);
    if(options.clean){std::error_code error;std::filesystem::remove(checkpoint,error);}
    if(!options.clean&&(std::filesystem::exists(checkpoint)||std::filesystem::exists(options.output))){
        auto source=options.output;
        if(std::filesystem::exists(checkpoint)){
            bool use_checkpoint=!std::filesystem::exists(options.output);
            if(!use_checkpoint){
                std::error_code checkpoint_error,output_error;
                const auto checkpoint_time=std::filesystem::last_write_time(checkpoint,checkpoint_error),output_time=std::filesystem::last_write_time(options.output,output_error);
                use_checkpoint=checkpoint_error||output_error||checkpoint_time>output_time;
            }
            if(use_checkpoint)source=checkpoint;
        }
        try{model=store.load_read_only(source);prior=true;resumed_checkpoint=source==checkpoint;}catch(const SnapshotVersionMismatch&){model.workspace=workspace;model.workspace_path=workspace_path;prior=false;}
        if(prior&&model.workspace!=workspace)throw std::runtime_error("existing snapshot belongs to a different workspace");
    }else{model.workspace=workspace;model.workspace_path=workspace_path;}
    mark("snapshot-load");

    CompileCommandsProvider provider;std::vector<std::filesystem::path> databases{options.compile_commands};databases.insert(databases.end(),options.additional_compile_commands.begin(),options.additional_compile_commands.end());
    std::vector<CompilationCommand> commands;std::map<BuildTargetID,BuildTarget> targets;std::set<std::pair<BuildTargetID,TranslationUnitID>> memberships;std::map<TargetDependencyID,TargetDependency> target_dependencies;
    for(std::size_t index=0;index<databases.size();++index){
        auto loaded=provider.load(databases[index],workspace);auto name=options.target_name;
        if(options.target_manifest.empty()){
            if(name.empty())name=databases.size()==1?options.workspace.filename().string():databases[index].parent_path().filename().string();
            else if(databases.size()>1)name+=':'+databases[index].parent_path().filename().string();
            const auto source=normalize_path(databases[index]);const BuildTargetID target_id{sha256("build-target|"+workspace.value+"|"+source+"|"+name)};
            targets[target_id]={target_id,name,options.target_kind,source,options.target_topology_authoritative};
            for(auto& command:loaded){memberships.insert({target_id,tu_id(command)});commands.push_back(std::move(command));}
        }else for(auto& command:loaded)commands.push_back(std::move(command));
    }
    std::sort(commands.begin(),commands.end(),[](const auto& left,const auto& right){return std::tie(left.source.value,left.configuration.value)<std::tie(right.source.value,right.configuration.value);});
    std::vector<CompilationCommand> unique_commands;for(auto& command:commands){if(!unique_commands.empty()&&unique_commands.back().source==command.source&&unique_commands.back().configuration==command.configuration){if(unique_commands.back().semantic_arguments!=command.semantic_arguments)throw std::runtime_error("conflicting commands across compilation databases for "+command.source_path.string());continue;}unique_commands.push_back(std::move(command));}commands=std::move(unique_commands);
    if(!options.target_manifest.empty())load_target_manifest(options,workspace,commands,targets,memberships,target_dependencies);
    std::vector<std::filesystem::path> project_roots{std::filesystem::path(workspace_path)};
    for(const auto& root:options.project_roots)project_roots.emplace_back(normalize_path(root,options.workspace));
    std::sort(project_roots.begin(),project_roots.end(),[](const auto& a,const auto& b){return normalize_path(a)<normalize_path(b);});
    project_roots.erase(std::unique(project_roots.begin(),project_roots.end(),[](const auto& a,const auto& b){return normalize_path(a)==normalize_path(b);}),project_roots.end());
    for(auto& command:commands){command.project_roots=project_roots;std::string extraction_view="extraction-view|"+command.fingerprint;for(const auto& root:project_roots){const auto normalized=normalize_path(root);extraction_view+=std::to_string(normalized.size())+":"+normalized+";";}command.fingerprint=sha256(extraction_view);}
    std::set<TranslationUnitID> desired;for(const auto& command:commands)desired.insert(tu_id(command));
    Canonicalizer canonicalizer(model);
    std::vector<TranslationUnitID> removed;
    for(const auto&[id,_]:model.translation_units)if(!desired.contains(id))removed.push_back(id);
    for(const auto& id:removed){canonicalizer.detach(id);model.translation_units.erase(id);}
    const auto refresh_target_topology=[&]{
        model.build_targets=targets;model.target_dependencies=target_dependencies;model.target_memberships.clear();
        for(const auto&[target,tu]:memberships)if(desired.contains(tu)&&model.translation_units.contains(tu)){const TargetMembershipID id{sha256("target-membership|"+target.value+"|"+tu.value)};model.target_memberships[id]={id,target,tu};}
    };
    refresh_target_topology();

    const auto dependency_changes=prior?changed_dependencies(model):std::set<FileID>{};
    std::set<TranslationUnitID> invalidated_by_header;
    if(!dependency_changes.empty())for(const auto&[_,include]:model.includes)if(dependency_changes.contains(include.included_file)){
        const auto revision=model.revisions.find(include.revision);if(revision!=model.revisions.end())invalidated_by_header.insert(revision->second.tu);
    }

    std::vector<CompilationRequest> requests;std::set<TranslationUnitID> replacement_tus;std::size_t reused{};
    for(const auto& command:commands){
        const auto tu=tu_id(command);std::string source_hash;
        try{source_hash=read_file_hash(command.source_path);}catch(...){source_hash="missing:"+sha256(normalize_path(command.source_path));}
        TranslationUnitRevisionKey key{tu,source_hash,command.fingerprint,extraction_version};
        bool changed=(options.clean&&!resumed_checkpoint)||invalidated_by_header.contains(tu);
        if(!changed){
            const auto active=model.active_revision_by_tu.find(tu.value);
            if(active==model.active_revision_by_tu.end())changed=true;
            else{
                const auto revision=model.revisions.find(active->second);
                changed=revision==model.revisions.end()||revision->second.source_hash!=source_hash||revision->second.compilation_fingerprint!=command.fingerprint||revision->second.extractor_version!=extraction_version;
            }
        }
        if(changed){if(model.active_revision_by_tu.contains(tu.value))replacement_tus.insert(tu);requests.push_back({command,tu,key});}else++reused;
    }
    mark("input-planning");
    const auto requested_workers=options.workers?options.workers:std::max(1u,std::thread::hardware_concurrency());
    const auto workers=std::min(requested_workers,std::max<std::size_t>(1,requests.size()));
    CompilationScheduler scheduler(workers);
    // Extract in bounded windows so the number of live AST-derived FactBatches is
    // proportional to worker count rather than project size. Each window is
    // committed in canonical TU order by this single semantic authority.
    constexpr std::size_t checkpoint_interval_tus=64;std::size_t since_checkpoint{};bool pending_garbage_collection=!removed.empty();
    for(std::size_t offset=0;offset<requests.size();offset+=workers){
        const auto end=std::min(requests.size(),offset+workers);
        std::vector<CompilationRequest> window(requests.begin()+static_cast<std::ptrdiff_t>(offset),requests.begin()+static_cast<std::ptrdiff_t>(end));
        auto batches=scheduler.extract(window,[]{return std::make_unique<LibClangFrontend>();});
        std::sort(batches.begin(),batches.end(),[](const auto&a,const auto&b){return std::tie(a.command.source.value,a.command.configuration.value)<std::tie(b.command.source.value,b.command.configuration.value);});
        for(const auto& batch:batches){canonicalizer.apply(batch);pending_garbage_collection|=replacement_tus.contains(tu_id(batch.command));}
        since_checkpoint+=batches.size();
        if(since_checkpoint>=checkpoint_interval_tus||end==requests.size()){
            refresh_target_topology();if(pending_garbage_collection){canonicalizer.garbage_collect();pending_garbage_collection=false;}refresh_target_topology();
            std::set<BuildConfigurationID> active_configurations;for(const auto&[_,tu]:model.translation_units)active_configurations.insert(tu.configuration);
            std::erase_if(model.configurations,[&](const auto& item){return !active_configurations.contains(item.first);});
            store.publish_checkpoint(model,checkpoint);since_checkpoint=0;mark("checkpoint");
        }
    }
    mark("extraction");
    resolve_call_targets(model);
    mark("call-resolution");
    refresh_entry_roots(model,options.manual_entry_roots);
    mark("entry-roots");
    if(pending_garbage_collection)canonicalizer.garbage_collect();
    mark("garbage-collection");
    std::erase_if(model.configurations,[&](const auto& item){return std::none_of(model.translation_units.begin(),model.translation_units.end(),[&](const auto& tu){return tu.second.configuration==item.first;});});
    if(options.require_complete && (model.completeness.total_tus==0 || model.completeness.complete_tus!=model.completeness.total_tus))throw std::runtime_error(
        "complete-model gate failed: total="+std::to_string(model.completeness.total_tus)+
        ", complete="+std::to_string(model.completeness.complete_tus)+
        ", warnings="+std::to_string(model.completeness.warning_tus)+
        ", partial="+std::to_string(model.completeness.partial_tus)+
        ", failed="+std::to_string(model.completeness.failed_tus));
    const auto validation=SemanticModelValidator{}.validate(model);
    if(!validation.ok())throw std::runtime_error("semantic validation failed: "+validation.issues.front().code+": "+validation.issues.front().message);
    mark("validation");
    store.publish(model,options.output);
    mark("publication");
    {std::error_code error;std::filesystem::remove(checkpoint,error);}
    return {std::move(model),requests.size(),reused};
}

} // namespace codeinsight
