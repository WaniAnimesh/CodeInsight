#include "codeinsight/analysis/dead_code.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace codeinsight;

namespace {

void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}

std::string json_escape(std::string value){std::string out;for(char c:value){if(c=='\\'||c=='"')out+='\\';out+=c;}return out;}

void write_compile_commands(const std::filesystem::path& root,bool include_b=true,std::string_view define="DEBUG=1",std::string_view filename="compile_commands.json"){
    std::ofstream out(root/std::string(filename),std::ios::binary|std::ios::trunc);
    const auto dir=json_escape(root.generic_string());
    auto entry=[&](std::string_view file){const auto source=json_escape((root/std::string(file)).generic_string());out<<"{\"directory\":\""<<dir<<"\",\"file\":\""<<source<<"\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-I\",\""<<dir<<"\",\"-D"<<define<<"\",\"-c\",\""<<source<<"\"]}";};
    out<<'[';entry("a.cpp");if(include_b){out<<',';entry("b.cpp");}out<<']';
}

std::filesystem::path fixture_copy(){
    const auto source=std::filesystem::path(CODEINSIGHT_SOURCE_DIR)/"tests"/"fixtures"/"basic";
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto target=std::filesystem::temp_directory_path()/("codeinsight-test-"+std::to_string(stamp));
    std::filesystem::create_directories(target);for(const auto& entry:std::filesystem::directory_iterator(source))std::filesystem::copy_file(entry.path(),target/entry.path().filename());
    write_compile_commands(target);return target;
}

const Symbol* find_symbol(const SemanticModel& model,std::string_view qualified){for(const auto&[_,symbol]:model.symbols)if(symbol.qualified_name==qualified)return &symbol;return nullptr;}

bool has_candidate(const SemanticModel& model,std::string_view qualified){for(const auto& candidate:analyze_dead_code(model))if(model.symbols.at(candidate.symbol).qualified_name==qualified)return true;return false;}

void append(const std::filesystem::path& path,std::string_view text){std::ofstream out(path,std::ios::binary|std::ios::app);out<<text;}

void write_text(const std::filesystem::path& path,std::string_view text){std::filesystem::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;}

const CompilationCommand& command_for(const std::vector<CompilationCommand>& commands,std::string_view filename){for(const auto& command:commands)if(command.source_path.filename()==filename)return command;throw std::runtime_error("missing compilation command");}

void write_fidelity_commands(const std::filesystem::path& root,std::string_view output,std::string_view define="DEBUG=1"){
    std::ofstream out(root/"fidelity.json",std::ios::binary|std::ios::trunc);const auto directory=json_escape(root.generic_string());const auto include=json_escape((root/"include"/"..").generic_string());
    auto entry=[&](std::string_view file,bool wrapped,std::string_view object){const auto source=json_escape((root/std::string(file)).generic_string());out<<"{\"directory\":\""<<directory<<"\",\"file\":\""<<source<<"\",\"arguments\":[";if(wrapped)out<<"\"ccache\",";out<<"\"clang++\",\"-std=c++20\",\"-I\",\""<<include<<"\",\"-D"<<define<<"\",\"-MF\",\""<<file<<".d\",\"-MT\",\""<<object<<"\",\"-c\",\""<<source<<"\",\"-o\",\""<<object<<"\"]}";};
    out<<'[';entry("a.cpp",true,output);out<<',';entry("b.cpp",false,"b.obj");out<<']';
}

void test_compilation_fidelity(){
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();const auto root=std::filesystem::temp_directory_path()/("codeinsight-command-"+std::to_string(stamp));std::filesystem::create_directories(root);
    try{
        write_text(root/"a.cpp","int a();\n");write_text(root/"b.cpp","int b();\n");write_fidelity_commands(root,"a-one.obj");
        const WorkspaceID workspace{sha256("workspace|"+normalize_path(root))};CompileCommandsProvider provider;const auto first=provider.load(root/"fidelity.json",workspace);require(first.size()==2,"compilation database lost commands");
        const auto& a=command_for(first,"a.cpp");const auto& b=command_for(first,"b.cpp");require(a.arguments.front()=="ccache"&&a.arguments[1]=="clang++","original wrapper argv was not preserved");require(a.compiler=="clang++","compiler driver was not resolved behind wrapper");require(a.configuration==b.configuration&&a.fingerprint==b.fingerprint,"source/output operands contaminated configuration identity");require(a.invocation!=b.invocation,"distinct original invocations collapsed");
        for(const auto& argument:a.semantic_arguments)require(argument.find("a-one.obj")==std::string::npos&&argument.find("a.cpp.d")==std::string::npos&&normalize_path(argument,root)!=normalize_path(root/"a.cpp"),"output, dependency, or source operand leaked into semantic argv");
        require(std::find(a.semantic_arguments.begin(),a.semantic_arguments.end(),normalize_path(root))!=a.semantic_arguments.end(),"relative include path was not canonicalized");
        write_fidelity_commands(root,"a-two.obj");const auto output_changed=provider.load(root/"fidelity.json",workspace);const auto& changed=command_for(output_changed,"a.cpp");require(changed.configuration==a.configuration&&changed.fingerprint==a.fingerprint,"output-only change invalidated semantic configuration");require(changed.invocation!=a.invocation,"output-only invocation change was not retained");
        write_fidelity_commands(root,"a-two.obj","DEBUG=2");const auto semantic_change=provider.load(root/"fidelity.json",workspace);require(command_for(semantic_change,"a.cpp").configuration!=a.configuration,"macro change did not change configuration identity");
        write_text(root/"args.rsp","-std=c++20 -DRESPONSE=1 -I include");const auto source=json_escape((root/"a.cpp").generic_string()),directory=json_escape(root.generic_string());write_text(root/"response.json","[{\"directory\":\""+directory+"\",\"file\":\""+source+"\",\"arguments\":[\"clang++\",\"@args.rsp\",\"-c\",\""+source+"\",\"-o\",\"a.obj\"]}]");
        const auto response_one=provider.load(root/"response.json",workspace);write_text(root/"args.rsp","-std=c++20 -DRESPONSE=2 -I include");const auto response_two=provider.load(root/"response.json",workspace);require(response_one.front().configuration!=response_two.front().configuration,"response-file content change did not invalidate configuration");
    }catch(...){std::filesystem::remove_all(root);throw;}std::filesystem::remove_all(root);
}

void test_project_boundary(){
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();const auto root=std::filesystem::temp_directory_path()/("codeinsight-boundary-"+std::to_string(stamp));const auto workspace=root/"workspace",dependency=root/"dependency";std::filesystem::create_directories(workspace);std::filesystem::create_directories(dependency);
    try{
        write_text(dependency/"external.hpp","#pragma once\nnamespace dep { struct ExternalBase {}; struct ExternalDerived : ExternalBase {}; inline int external_value() { return 7; } }\n");write_text(workspace/"main.cpp","#include <external.hpp>\nnamespace project { int own() { return dep::external_value(); } }\n");
        const auto directory=json_escape(workspace.generic_string()),source=json_escape((workspace/"main.cpp").generic_string()),include=json_escape(dependency.generic_string());write_text(workspace/"compile_commands.json","[{\"directory\":\""+directory+"\",\"file\":\""+source+"\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-I\",\""+include+"\",\"-c\",\""+source+"\"]}]");
        IndexOptions bounded{workspace,workspace/"compile_commands.json",workspace/"bounded.db",1,true,{}};const auto project_only=IncrementalExecutor{}.build(bounded);require(find_symbol(project_only.model,"project::own"),"project symbol was excluded by boundary");require(!find_symbol(project_only.model,"dep::external_value"),"dependency declaration flooded project model");
        bool external_call{};for(const auto&[_,relationship]:project_only.model.relationships)external_call|=relationship.kind==RelationshipKind::Calls&&relationship.target_domain==TargetDomain::External&&relationship.observed_target_spelling=="external_value";require(external_call,"external declaration pruning lost the project call-site boundary observation");
        bool external_file{};for(const auto&[_,file]:project_only.model.files)if(file.path==normalize_path(dependency/"external.hpp"))external_file=file.external;require(external_file,"dependency include target was not retained as external evidence");
        bounded.clean=false;const auto unchanged=IncrementalExecutor{}.build(bounded);require(unchanged.extracted_tus==0&&unchanged.reused_tus==1,"external include version was discarded and forced a no-op rebuild");
        bounded.project_roots={dependency};const auto expanded=IncrementalExecutor{}.build(bounded);require(expanded.extracted_tus==1,"project-boundary change did not invalidate TU extraction");require(find_symbol(expanded.model,"dep::external_value"),"explicit project root was not extracted");const auto* derived=find_symbol(expanded.model,"dep::ExternalDerived");require(derived,"explicit project-root derived class was not extracted");bool inheritance{};for(const auto&[_,relationship]:expanded.model.relationships)inheritance|=relationship.kind==RelationshipKind::Inherits&&relationship.source==derived->id;require(inheritance,"owned external root lost its inheritance relationship");bool promoted{};for(const auto&[_,file]:expanded.model.files)if(file.path==normalize_path(dependency/"external.hpp"))promoted=!file.external;require(promoted,"explicit project-root file remained external");
    }catch(...){std::filesystem::remove_all(root);throw;}std::filesystem::remove_all(root);
}

FactBatch synthetic_batch(const std::filesystem::path& source,WorkspaceID workspace,std::string parent_local){
    CompilationCommand command;command.source_path=source;command.working_directory=source.parent_path();command.source=FileID{sha256("file|"+workspace.value+"|"+normalize_path(source))};command.compiler="synthetic";command.compiler_identity="synthetic-compiler";command.configuration_key="synthetic-configuration";command.configuration=BuildConfigurationID{sha256(command.configuration_key)};command.fingerprint=sha256("compilation-fingerprint|"+command.configuration_key);command.project_roots={source.parent_path()};
    const TranslationUnitID tu{sha256("tu|"+command.source.value+"|"+command.configuration.value)};FactBatch batch;batch.command=command;batch.revision_key={tu,read_file_hash(source),command.fingerprint,extraction_version};batch.quality=ExtractionQuality::Complete;
    RawSourceRange parent_range{{normalize_path(source),1,1,0},{normalize_path(source),1,12,11}},child_range{{normalize_path(source),2,1,12},{normalize_path(source),2,10,21}};
    SymbolFact parent;parent.local_identity=parent_local;parent.kind=SymbolKind::Namespace;parent.name="N";parent.qualified_name="N";parent.expansion_range=parent.spelling_range=parent_range;parent.linkage=Linkage::External;parent.is_declaration=parent.is_definition=true;
    SymbolFact child;child.local_identity="child-"+parent_local;child.kind=SymbolKind::Function;child.name="f";child.qualified_name="N::f";child.semantic_parent=parent_local;child.expansion_range=child.spelling_range=child_range;child.linkage=Linkage::External;child.is_declaration=child.is_definition=true;
    batch.symbols={parent,child};return batch;
}

void test_fallback_parent_canonicalization(){
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();const auto root=std::filesystem::temp_directory_path()/("codeinsight-parent-"+std::to_string(stamp));std::filesystem::create_directories(root);write_text(root/"a.cpp","namespace N { void f() {} }\n");write_text(root/"b.cpp","namespace N { void f(); }\n");
    try{SemanticModel model;model.workspace_path=normalize_path(root);model.workspace=WorkspaceID{sha256("workspace|"+model.workspace_path)};Canonicalizer canonicalizer(model);canonicalizer.apply(synthetic_batch(root/"a.cpp",model.workspace,"parent-a"));canonicalizer.apply(synthetic_batch(root/"b.cpp",model.workspace,"parent-b"));canonicalizer.garbage_collect();
        std::vector<const Symbol*> namespaces,functions;for(const auto&[_,symbol]:model.symbols){if(symbol.qualified_name=="N")namespaces.push_back(&symbol);if(symbol.qualified_name=="N::f")functions.push_back(&symbol);}require(namespaces.size()==1&&functions.size()==1,"fallback symbols did not merge through canonical parent");require(functions.front()->semantic_parent&&*functions.front()->semantic_parent==namespaces.front()->id,"fallback child has wrong canonical parent");require(SemanticModelValidator{}.validate(model).ok(),"synthetic parent model failed validation");
    }catch(...){std::filesystem::remove_all(root);throw;}std::filesystem::remove_all(root);
}

void add_definition(SemanticModel& model,const SymbolID& symbol,std::string suffix){
    const OccurrenceID id{std::move(suffix)};model.occurrences.emplace(id,SymbolOccurrence{id,symbol,TranslationUnitRevisionID{"revision"},{FileVersionID{"file-version"},1,1,0,1,2,1},OccurrenceRole::Definition,false});
}

void test_logical_projection_and_template_retention(){
    SemanticModel model;model.completeness={1,1,0,0,0};
    auto symbol=[&](std::string id,std::string usr,std::string name,SymbolKind kind=SymbolKind::Function){Symbol value;value.id=SymbolID{id};value.canonical_key=id;value.usr=std::move(usr);value.kind=kind;value.name=name.substr(name.rfind("::")==std::string::npos?0:name.rfind("::")+2);value.qualified_name=std::move(name);value.linkage=Linkage::Internal;model.symbols.emplace(value.id,std::move(value));return SymbolID{id};};
    const auto root=symbol("root","c:@F@main#","main"),declaration=symbol("decl","c:@F@api#","api"),definition=symbol("def","c:@F@api#","api"),helper=symbol("helper","c:test.cpp@F@helper#","helper");
    const LogicalSymbolID logical{"logical-api"};model.logical_symbols.emplace(logical,LogicalSymbol{logical,"logical-api","c:@F@api#",Linkage::External,{declaration,definition}});model.symbols.at(declaration).logical_symbol=logical;model.symbols.at(definition).logical_symbol=logical;
    add_definition(model,root,"root-occ");add_definition(model,definition,"def-occ");add_definition(model,helper,"helper-occ");
    Relationship call_decl;call_decl.id=RelationshipID{"call-decl"};call_decl.kind=RelationshipKind::Calls;call_decl.source=root;call_decl.target_symbol=declaration;call_decl.resolution=ResolutionStatus::Exact;model.relationships.emplace(call_decl.id,call_decl);
    Relationship call_helper=call_decl;call_helper.id=RelationshipID{"call-helper"};call_helper.source=definition;call_helper.target_symbol=helper;model.relationships.emplace(call_helper.id,call_helper);
    model.entry_roots.emplace(EntryRootID{"root-entry"},EntryRoot{EntryRootID{"root-entry"},root,EntryRootKind::ProcessEntry,"test",std::nullopt});
    require(!has_candidate(model,"api")&&!has_candidate(model,"helper"),"logical declaration-to-definition projection did not retain the live implementation chain");

    const auto primary=symbol("primary","c:test.cpp@FT@>1#Tretain#t0.0#","retain",SymbolKind::FunctionTemplate),instantiation=symbol("instantiation","c:test.cpp@F@retain<#I>#I#","retain");model.symbols.at(primary).templ.is_primary=true;add_definition(model,primary,"primary-occ");add_definition(model,instantiation,"inst-occ");
    Relationship call_template=call_decl;call_template.id=RelationshipID{"call-template"};call_template.source=root;call_template.target_symbol=instantiation;model.relationships.emplace(call_template.id,call_template);
    require(!has_candidate(model,"retain"),"reachable function-template instantiation did not retain its source primary");
    const auto external_api=symbol("external-api","c:@F@public_api#","public_api");model.symbols.at(external_api).linkage=Linkage::External;model.symbols.at(external_api).visibility=Visibility::Default;add_definition(model,external_api,"external-api-occ");require(!has_candidate(model,"public_api"),"open-world external API was reported without closed visibility provenance");
    const auto pure_declaration=symbol("pure-declaration","c:@S@Base@F@describe#1","Base::describe",SymbolKind::Method);Relationship pure_call=call_decl;pure_call.id=RelationshipID{"pure-call"};pure_call.target_symbol=pure_declaration;model.relationships.emplace(pure_call.id,pure_call);require(assess_dead_code_readiness(model).cross_configuration_bindings_complete,"declaration-only virtual or external target was misclassified as a stranded cross-configuration definition");
}

void write_single_command(const std::filesystem::path& root){
    const auto directory=json_escape(root.generic_string()),source=json_escape((root/"main.cpp").generic_string());write_text(root/"compile_commands.json","[{\"directory\":\""+directory+"\",\"file\":\""+source+"\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\""+source+"\"]}]");
}

void test_dependent_member_pointer_type(){
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();const auto root=std::filesystem::temp_directory_path()/("codeinsight-member-pointer-"+std::to_string(stamp));std::filesystem::create_directories(root);
    try{
        write_text(root/"main.cpp",R"CPP(template<class T> struct OptionSet {
    template<class E> void define(E T::* member) { (void)member; }
};
template<class... Ts> using void_t = void;
template<class T> T instantiated_at(T input) { T local = input; return local; }
extern "C" { int c_linkage(); }
int c_linkage() { return 1; }
struct Base {};
using BaseAlias = Base;
struct Derived : BaseAlias {};
struct Settings { int value; };
int main() { OptionSet<Settings> options; options.define(&Settings::value); void_t<int>* value = nullptr; return value != nullptr || instantiated_at(1) != 1 || instantiated_at(2.0) != 2.0 || c_linkage() != 1; }
)CPP");
        write_single_command(root);IndexOptions options{root,root/"compile_commands.json",root/"model.db",1,true,{}};options.require_complete=true;
        const auto result=IncrementalExecutor{}.build(options);require(result.model.completeness.complete_tus==1,"dependent member-pointer fixture was not complete");
        bool member_pointer{};for(const auto&[_,type]:result.model.types)member_pointer|=type.kind==TypeKind::MemberPointer&&type.canonical_spelling.find("::*")!=std::string::npos;
        require(member_pointer,"dependent member-pointer type was not retained by canonical spelling");require(SemanticModelValidator{}.validate(result.model).ok(),"dependent member-pointer model failed validation");
        std::size_t alias_templates{};for(const auto&[_,symbol]:result.model.symbols)alias_templates+=symbol.name=="void_t"&&symbol.kind==SymbolKind::AliasTemplate;require(alias_templates==1,"alias-template wrapper cursors were not normalized to one source symbol");
        std::size_t c_linkage_symbols{};for(const auto&[_,symbol]:result.model.symbols)c_linkage_symbols+=symbol.name=="c_linkage";require(c_linkage_symbols==1,"extern-C declaration and definition did not unify");
        const auto* alias=find_symbol(result.model,"BaseAlias");require(alias&&alias->kind==SymbolKind::TypeAlias,"base-type alias was not extracted");
        auto alias_base=result.model;bool rewired{};for(auto&[_,relationship]:alias_base.relationships)if(relationship.kind==RelationshipKind::Inherits){relationship.target_symbol=alias->id;rewired=true;break;}require(rewired,"base-type alias fixture has no inheritance edge");require(SemanticModelValidator{}.validate(alias_base).ok(),"valid inheritance through a type alias was rejected");
    }catch(...){std::filesystem::remove_all(root);throw;}std::filesystem::remove_all(root);
}

void test_bounded_dispatch_and_escape_safety(){
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();const auto root=std::filesystem::temp_directory_path()/("codeinsight-dispatch-"+std::to_string(stamp));std::filesystem::create_directories(root);
    const std::string prefix=R"CPP(namespace std { template<class T, unsigned N> struct array {
    T slots[N]{};
    void fill(T value) { for (unsigned index = 0; index < N; ++index) slots[index] = value; }
    T& operator[](unsigned index) { return slots[index]; }
    const T& operator[](unsigned index) const { return slots[index]; }
}; }
using Callback = int (*)(int);
static int live_default(int value) { return value; }
static int live_one(int value) { return value + 1; }
static int dead_compat(int value) { return value + 173; }
static int dead_archival(int value) { return value + 239; }
static const std::array<Callback, 256>& routes() {
    static const auto values = [] {
        std::array<Callback, 256> table{};
        table.fill(&live_default);
        table[1] = &live_one;
        table[173] = &dead_compat;
        table[239] = &dead_archival;
)CPP";
    const std::string suffix=R"CPP(        return table;
    }();
    return values;
}

static unsigned char normalize(unsigned raw) { raw ^= raw >> 8U; return static_cast<unsigned char>(raw & 1U); }
int api(unsigned raw) { return routes()[normalize(raw)](3); }
int main() { return api(0) == 3 ? 0 : 1; }
)CPP";
    try{
        write_text(root/"main.cpp",prefix+suffix);write_single_command(root);write_text(root/"targets.ci","# CodeInsight target manifest v1\ntarget|dispatch|executable\nmember|dispatch|main.cpp\n");IndexOptions options{root,root/"compile_commands.json",root/"model.db",1,true,{}};options.target_manifest=root/"targets.ci";options.require_complete=true;
        const auto bounded=IncrementalExecutor{}.build(options);require(bounded.model.indirect_call_summaries.size()==1&&!bounded.model.indirect_call_summaries.begin()->second.complete,"source-pattern analysis must remain incomplete");require(!has_candidate(bounded.model,"dead_compat")&&!has_candidate(bounded.model,"dead_archival"),"unproved index semantics discarded conservative address roots");require(!has_candidate(bounded.model,"live_default")&&!has_candidate(bounded.model,"live_one"),"bounded dispatch reported a selectable table entry as dead");
        auto unknown_index=prefix+suffix;const auto mask=unknown_index.find("raw & 1U");require(mask!=std::string::npos,"dispatch fixture mask missing");unknown_index.replace(mask,std::string("raw & 1U").size(),"raw");write_text(root/"main.cpp",unknown_index);options.clean=true;const auto unknown=IncrementalExecutor{}.build(options);require(unknown.model.indirect_call_summaries.empty(),"unknown index domain incorrectly retained a complete flow proof");require(!has_candidate(unknown.model,"dead_compat")&&!has_candidate(unknown.model,"dead_archival"),"unknown index domain lost conservative address roots");
        write_text(root/"main.cpp",prefix+"        (void)&table;\n"+suffix);options.clean=true;const auto escaping=IncrementalExecutor{}.build(options);require(escaping.model.indirect_call_summaries.empty(),"escaping table incorrectly retained a complete flow proof");require(!has_candidate(escaping.model,"dead_compat")&&!has_candidate(escaping.model,"dead_archival"),"escaping table entries lost their conservative address roots");
    }catch(...){std::filesystem::remove_all(root);throw;}std::filesystem::remove_all(root);
}

void test_failed_build_resumes_from_checkpoint(){
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();const auto root=std::filesystem::temp_directory_path()/("codeinsight-checkpoint-"+std::to_string(stamp));std::filesystem::create_directories(root);
    try{
        write_text(root/"a.cpp","int a() { return 1; }\n");write_text(root/"b.cpp","int b( { return 2; }\n");
        const auto directory=json_escape(root.generic_string()),a=json_escape((root/"a.cpp").generic_string()),b=json_escape((root/"b.cpp").generic_string());write_text(root/"compile_commands.json","[{\"directory\":\""+directory+"\",\"file\":\""+a+"\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\""+a+"\"]},{\"directory\":\""+directory+"\",\"file\":\""+b+"\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\""+b+"\"]}]");
        IndexOptions options{root,root/"compile_commands.json",root/"model.db",2,true,{}};options.require_complete=true;bool failed{};try{(void)IncrementalExecutor{}.build(options);}catch(const std::exception&){failed=true;}require(failed,"incomplete build unexpectedly passed the complete-model gate");
        auto checkpoint=options.output;checkpoint+=L".checkpoint";require(std::filesystem::exists(checkpoint),"failed build did not retain its extraction checkpoint");
        write_text(root/"b.cpp","int b() { return 2; }\n");options.clean=false;const auto resumed=IncrementalExecutor{}.build(options);require(resumed.extracted_tus==1&&resumed.reused_tus==1,"checkpoint resume did not reuse the completed translation unit");require(!std::filesystem::exists(checkpoint),"successful publication did not remove the completed checkpoint");require(resumed.model.completeness.complete_tus==2,"checkpoint resume did not produce a complete model");
    }catch(...){std::filesystem::remove_all(root);throw;}std::filesystem::remove_all(root);
}

void run(){
    require(sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 contract failed");
    require(path_is_within("C:/project/include/a.hpp","C:/project")&&!path_is_within("C:/project-other/a.hpp","C:/project"),"project boundary path comparison is not component-safe");
    test_compilation_fidelity();test_project_boundary();test_fallback_parent_canonicalization();test_logical_projection_and_template_retention();test_dependent_member_pointer_type();test_bounded_dispatch_and_escape_safety();test_failed_build_resumes_from_checkpoint();
    const auto root=fixture_copy();const auto database=root/"model.db";const auto clean_database=root/"clean.db";
    try{
        IndexOptions options{root,root/"compile_commands.json",database,2,true,{}};
        auto first=IncrementalExecutor{}.build(options);require(first.extracted_tus==2,"clean build must extract both TUs");require(first.model.completeness.total_tus==2,"completeness TU count");require(first.model.completeness.failed_tus==0,"fixture extraction failed");
        require(SemanticModelValidator{}.validate(first.model).ok(),"clean model failed validation");
        const auto* process=find_symbol(first.model,"demo::process");require(process,"cross-TU function missing");
        std::size_t process_occurrences{};bool has_declaration{},has_definition{};for(const auto&[_,occurrence]:first.model.occurrences)if(occurrence.symbol==process->id){++process_occurrences;has_declaration|=occurrence.role==OccurrenceRole::Declaration;has_definition|=occurrence.role==OccurrenceRole::Definition;}
        require(process_occurrences>=2&&has_declaration&&has_definition,"declaration and definition were not unified");
        std::size_t helpers{};for(const auto&[_,symbol]:first.model.symbols)if(symbol.qualified_name=="demo::local_helper")++helpers;
        require(helpers==2,"internal-linkage functions were incorrectly merged");
        std::size_t header_locals{},overloads{};for(const auto&[_,symbol]:first.model.symbols){header_locals+=symbol.qualified_name=="demo::header_local";overloads+=symbol.qualified_name=="demo::overload";}require(header_locals==2,"shared-header internal linkage was incorrectly merged across TUs");require(overloads==2,"function overloads were incorrectly merged");
        require(!first.model.includes.empty(),"compiler-observed includes missing");require(!first.model.macro_definitions.empty(),"macro definitions missing");
        bool has_type_edge{},has_inheritance{},has_override{},has_call{},has_containment{},has_read{},has_write{},has_construct{},has_destroy{},has_address{};for(const auto&[_,relationship]:first.model.relationships){has_type_edge|=relationship.kind==RelationshipKind::ParameterType||relationship.kind==RelationshipKind::ReturnsType;has_inheritance|=relationship.kind==RelationshipKind::Inherits;has_override|=relationship.kind==RelationshipKind::Overrides;has_call|=relationship.kind==RelationshipKind::Calls;has_containment|=relationship.kind==RelationshipKind::Contains;has_read|=relationship.kind==RelationshipKind::Reads;has_write|=relationship.kind==RelationshipKind::Writes;has_construct|=relationship.kind==RelationshipKind::Constructs;has_destroy|=relationship.kind==RelationshipKind::Destroys;has_address|=relationship.kind==RelationshipKind::TakesAddress;}require(has_type_edge,"function type relationships missing");require(has_inheritance&&has_override&&has_call&&has_containment,"core semantic relationships missing");require(has_read,"read relationships missing");require(has_write,"write relationships missing");require(has_construct,"construct relationships missing");require(has_destroy,"destroy relationships missing");require(has_address,"address-taking relationships missing");
        require(first.model.build_targets.size()==1&&first.model.target_memberships.size()==2,"build target ownership missing");require(first.model.build_targets.begin()->second.source==normalize_path(root/"compile_commands.json"),"build target compilation-database provenance was resolved against the wrong base");bool address_root{},export_root{};for(const auto&[_,entry]:first.model.entry_roots){address_root|=entry.kind==EntryRootKind::AddressTaken;export_root|=entry.kind==EntryRootKind::Export;}require(address_root,"conservative address-taken entry root missing");require(export_root,"compiler-observed export root missing");
        const auto records=export_semantic_edges(first.model,false),jsonl=export_observations(first.model,true);require(records.find("relationship|Reads|")!=std::string::npos&&records.find("entry-root|")!=std::string::npos,"semantic record export missing data");require(jsonl.find("\"record\":\"relationship\"")!=std::string::npos,"JSONL observation export missing data");
        const auto candidates=analyze_dead_code(first.model);require(!candidates.empty(),"dead-code analysis produced no candidates");for(const auto& candidate:candidates)require(candidate.confidence!=CandidateConfidence::High,"unknown build target produced a high-confidence dead-code candidate");
        const auto loaded=SQLiteSnapshotStore{}.load_read_only(database);require(normalized_snapshot(first.model)==normalized_snapshot(loaded),"SQLite round trip changed semantics");
        write_compile_commands(root,true,"MULTI_CONFIG=1","compile_commands-alt.json");IndexOptions multi=options;multi.output=root/"multi.db";multi.clean=true;multi.additional_compile_commands={root/"compile_commands-alt.json"};multi.manual_entry_roots={"demo::process"};multi.require_complete=true;const auto merged=IncrementalExecutor{}.build(multi);require(merged.model.translation_units.size()==4&&merged.model.configurations.size()==2,"multi-configuration merge lost translation units or configurations");require(merged.model.build_targets.size()==2&&merged.model.target_memberships.size()==4,"multi-configuration target ownership missing");bool manual_root{};for(const auto&[_,entry]:merged.model.entry_roots)manual_root|=entry.kind==EntryRootKind::Manual;require(manual_root,"configured entry root missing");
        {auto invalid=loaded;++invalid.completeness.total_tus;bool rejected{};try{SQLiteSnapshotStore{}.publish(invalid,database);}catch(...){rejected=true;}require(rejected,"invalid model replaced active snapshot");require(normalized_snapshot(SQLiteSnapshotStore{}.load_read_only(database))==normalized_snapshot(loaded),"failed publication changed active snapshot");}

        options.clean=false;auto unchanged=IncrementalExecutor{}.build(options);require(unchanged.extracted_tus==0&&unchanged.reused_tus==2,"unchanged TUs were not reused");
        append(root/"a.cpp","\n// source revision\n");auto source_change=IncrementalExecutor{}.build(options);require(source_change.extracted_tus==1,"source change invalidation was not TU-local");

        append(root/"common.hpp","\n// header revision\n");auto header_change=IncrementalExecutor{}.build(options);require(header_change.extracted_tus==2,"header change did not invalidate dependent TUs");
        IndexOptions clean{root,root/"compile_commands.json",clean_database,1,true,{}};auto rebuilt=IncrementalExecutor{}.build(clean);
        {const auto incremental=normalized_snapshot(header_change.model),clean_model=normalized_snapshot(rebuilt.model);if(incremental!=clean_model){const auto mismatch=std::mismatch(incremental.begin(),incremental.end(),clean_model.begin(),clean_model.end());const auto offset=static_cast<std::size_t>(mismatch.first-incremental.begin());throw std::runtime_error("incremental model differs from clean rebuild at byte "+std::to_string(offset)+"\nincremental: "+incremental.substr(offset,240)+"\nclean: "+clean_model.substr(offset,240));}}

        write_compile_commands(root,true,"DEBUG=2");auto configuration_change=IncrementalExecutor{}.build(options);require(configuration_change.extracted_tus==2&&configuration_change.model.completeness.total_tus==2,"configuration change did not replace TU semantics");
        write_compile_commands(root,false,"DEBUG=2");auto deleted=IncrementalExecutor{}.build(options);require(deleted.model.completeness.total_tus==1,"deleted TU remained active");require(SemanticModelValidator{}.validate(deleted.model).ok(),"model invalid after TU deletion");
        append(root/"a.cpp","\nint intentionally_broken = ;\n");auto partial=IncrementalExecutor{}.build(options);require(partial.model.completeness.partial_tus==1,"broken TU was not represented as partial");require(SQLiteSnapshotStore{}.validate_file(database).ok(),"partial snapshot was not valid or publishable");
        std::filesystem::remove(root/"a.cpp");auto failed=IncrementalExecutor{}.build(options);require(failed.model.completeness.failed_tus==1,"missing source was not isolated as a failed TU");require(SQLiteSnapshotStore{}.validate_file(database).ok(),"failed-TU snapshot was not valid or publishable");
        const auto corrupt=root/"corrupt.db";std::filesystem::copy_file(database,corrupt);{std::fstream file(corrupt,std::ios::binary|std::ios::in|std::ios::out);file.write("not-sqlite",10);}require(!SQLiteSnapshotStore{}.validate_file(corrupt).ok(),"corrupt snapshot passed publication validation");
        std::filesystem::remove_all(root);
    }catch(...){std::filesystem::remove_all(root);throw;}
}

} // namespace

int main(){try{run();std::cout<<"All CodeInsight tests passed.\n";return 0;}catch(const std::exception& error){std::cerr<<"Test failure: "<<error.what()<<'\n';return 1;}}
