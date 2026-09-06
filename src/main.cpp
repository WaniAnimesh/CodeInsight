#include "codeinsight/codeinsight.hpp"
#include "codeinsight/semantic/call_resolution.hpp"
#ifdef CODEINSIGHT_WITH_ANALYSIS
#include "codeinsight/analysis/dead_code.hpp"
#endif

#include <cstdlib>
#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace codeinsight;

namespace {

void usage(){std::cout<<"CodeInsight canonical semantic model\n\n"
    "  codeinsight build --workspace PATH --compile-commands FILE [--compile-commands FILE ...] --output FILE [--target-manifest FILE | --target NAME --target-kind KIND [--authoritative-target]] [--entry-root QUALIFIED_NAME ...] [--project-root PATH ...] [--jobs N] [--clean] [--require-complete]\n"
    "  codeinsight validate-model --model FILE\n"
    "  codeinsight inspect --model FILE\n"
    "  codeinsight toolchain\n"
    "  codeinsight export --model FILE [--view observations|semantic] [--format records|jsonl] [--output FILE]\n"
    "  codeinsight query --model FILE --relationship KIND [--view observations|semantic] [--format records|jsonl]\n"
#ifdef CODEINSIGHT_WITH_ANALYSIS
    "  codeinsight analyze --model FILE --analysis dead-code [--format records|jsonl] [--output FILE]\n"
#endif
    "  codeinsight diagnostics --model FILE\n";}

std::string value(int& index,int argc,char** argv){if(index+1>=argc)throw std::invalid_argument(std::string("missing value for ")+argv[index]);return argv[++index];}

std::size_t worker_count(std::string_view input) {
    std::size_t count{};
    const auto [end,error]=std::from_chars(input.data(),input.data()+input.size(),count);
    if (error!=std::errc{} || end!=input.data()+input.size())
        throw std::invalid_argument("--jobs must be a nonnegative decimal integer within the platform range (0 means automatic)");
    return count;
}

void write_output(const std::string& content,const std::filesystem::path& path){
    if(path.empty()){std::cout<<content;return;}
    std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("unable to create export file: "+path.string());out<<content;out.flush();if(!out)throw std::runtime_error("unable to write export file: "+path.string());
}

BuildTargetKind target_kind(std::string_view value){
    if(value=="executable")return BuildTargetKind::Executable;if(value=="static-library")return BuildTargetKind::StaticLibrary;if(value=="shared-library")return BuildTargetKind::SharedLibrary;if(value=="object-library")return BuildTargetKind::ObjectLibrary;if(value=="test")return BuildTargetKind::Test;if(value=="unknown")return BuildTargetKind::Unknown;throw std::invalid_argument("unknown target kind: "+std::string(value));
}

std::string filter_relationship(std::string_view content,std::string_view kind,bool json_lines){
    std::istringstream input{std::string(content)};std::ostringstream output;std::string line;
    const auto marker=json_lines?std::string("\"kind\":\"")+std::string(kind)+"\"":std::string("relationship|")+std::string(kind)+"|";
    while(std::getline(input,line))if(line.find(marker)!=std::string::npos)output<<line<<'\n';return output.str();
}

} // namespace

int main(int argc,char** argv){
    try{
        if(argc<2){usage();return 2;}const std::string command=argv[1];
        if(command=="toolchain"){
            if(argc!=2)throw std::invalid_argument("toolchain does not accept options");const auto version=frontend_version();
            std::cout<<"Configured: "<<version.configured<<"\nRuntime: "<<version.runtime<<"\nMajor: "<<version.major<<"\nSupported: "<<(version.supported()?"yes":"no")<<'\n';return version.supported()?0:1;
        }
        if(command=="build"){
            IndexOptions options;
            for(int i=2;i<argc;++i){const std::string arg=argv[i];if(arg=="--workspace")options.workspace=value(i,argc,argv);else if(arg=="--compile-commands"){auto path=std::filesystem::path(value(i,argc,argv));if(options.compile_commands.empty())options.compile_commands=std::move(path);else options.additional_compile_commands.push_back(std::move(path));}else if(arg=="--output")options.output=value(i,argc,argv);else if(arg=="--target-manifest")options.target_manifest=value(i,argc,argv);else if(arg=="--target")options.target_name=value(i,argc,argv);else if(arg=="--target-kind")options.target_kind=target_kind(value(i,argc,argv));else if(arg=="--authoritative-target")options.target_topology_authoritative=true;else if(arg=="--entry-root")options.manual_entry_roots.push_back(value(i,argc,argv));else if(arg=="--project-root")options.project_roots.emplace_back(value(i,argc,argv));else if(arg=="--jobs")options.workers=worker_count(value(i,argc,argv));else if(arg=="--clean")options.clean=true;else if(arg=="--require-complete")options.require_complete=true;else throw std::invalid_argument("unknown option: "+arg);}
            if(!options.target_manifest.empty()&&(!options.target_name.empty()||options.target_kind!=BuildTargetKind::Unknown||options.target_topology_authoritative))throw std::invalid_argument("--target-manifest cannot be combined with single-target options");
            const auto result=IncrementalExecutor{}.build(options);const auto& c=result.model.completeness;
            std::cout<<"Published "<<options.output<<"\nExtracted TUs: "<<result.extracted_tus<<"; reused: "<<result.reused_tus
                     <<"\nCompleteness: total="<<c.total_tus<<", complete="<<c.complete_tus<<", warnings="<<c.warning_tus<<", partial="<<c.partial_tus<<", failed="<<c.failed_tus
                     <<"\nSymbols: "<<result.model.symbols.size()<<"; logical symbols: "<<result.model.logical_symbols.size()<<"; types: "<<result.model.types.size()<<"; relationships: "<<result.model.relationships.size()
                     <<"\nBuild targets: "<<result.model.build_targets.size()<<"; target dependencies: "<<result.model.target_dependencies.size()<<"; indirect-call summaries: "<<result.model.indirect_call_summaries.size()<<"\n";
            return c.failed_tus?3:0;
        }
        std::filesystem::path model_path,output_path;std::string view="semantic",format="records",relationship_kind,analysis;
        for(int i=2;i<argc;++i){const std::string arg=argv[i];if(arg=="--model")model_path=value(i,argc,argv);else if(arg=="--output")output_path=value(i,argc,argv);else if(arg=="--view")view=value(i,argc,argv);else if(arg=="--format")format=value(i,argc,argv);else if(arg=="--relationship")relationship_kind=value(i,argc,argv);else if(arg=="--analysis")analysis=value(i,argc,argv);else throw std::invalid_argument("unknown option: "+arg);}
        if(model_path.empty())throw std::invalid_argument("--model is required");
        if(!output_path.empty()) {
            std::error_code error;
            const bool same=std::filesystem::equivalent(model_path,output_path,error);
            if(same||normalize_path(model_path)==normalize_path(output_path))
                throw std::invalid_argument("export output must not overwrite the input model");
        }
        SQLiteSnapshotStore store;
        if(command=="validate-model"){
            const auto result=store.validate_file(model_path);if(result.ok()){std::cout<<"Model is valid.\n";return 0;}
            for(const auto& issue:result.issues)std::cerr<<issue.code<<": "<<issue.message<<'\n';return 1;
        }
        if(command=="inspect"){
            const auto model=store.load_read_only(model_path);const auto& c=model.completeness;
            std::cout<<"Workspace: "<<model.workspace_path<<"\nTUs: "<<c.total_tus<<" (complete "<<c.complete_tus<<", warnings "<<c.warning_tus<<", partial "<<c.partial_tus<<", failed "<<c.failed_tus<<")\n"
                <<"Files: "<<model.files.size()<<"\nSymbols: "<<model.symbols.size()<<"\nLogical symbols: "<<model.logical_symbols.size()<<"\nOccurrences: "<<model.occurrences.size()<<"\nTypes: "<<model.types.size()<<"\nRelationships: "<<model.relationships.size()<<"\nMacros: "<<model.macro_definitions.size()<<" definitions, "<<model.macro_expansions.size()<<" expansions\nDiagnostics: "<<model.diagnostics.size()<<"\nBuild targets: "<<model.build_targets.size()<<"; dependencies: "<<model.target_dependencies.size()<<"\nEntry roots: "<<model.entry_roots.size()<<"\nIndirect-call summaries: "<<model.indirect_call_summaries.size()<<'\n';
            std::size_t complete{}, incomplete{}, external{};
            for (const auto& call : summarize_call_targets(model)) {
                switch (call.completeness) {
                case CallTargetCompleteness::Complete: ++complete; break;
                case CallTargetCompleteness::Incomplete: ++incomplete; break;
                case CallTargetCompleteness::ExternalOnly: ++external; break;
                }
            }
            std::cout << "Call target sets: complete=" << complete << ", incomplete=" << incomplete << ", external-only=" << external << '\n';
            return 0;
        }
        if(command=="export"||command=="query"){
            if(view!="observations"&&view!="semantic")throw std::invalid_argument("--view must be observations or semantic");
            if(format!="records"&&format!="jsonl")throw std::invalid_argument("--format must be records or jsonl");
            if(command=="query"&&relationship_kind.empty())throw std::invalid_argument("query requires --relationship KIND");
            const auto model=store.load_read_only(model_path);const bool json_lines=format=="jsonl";
            auto content=view=="observations"?export_observations(model,json_lines):export_semantic_edges(model,json_lines);
            if(command=="query")content=filter_relationship(content,relationship_kind,json_lines);write_output(content,output_path);return 0;
        }
        if(command=="diagnostics"){
            const auto model=store.load_read_only(model_path);std::map<std::tuple<std::uint32_t,std::string,std::string>,std::size_t> grouped;
            for(const auto&[_,diagnostic]:model.diagnostics)++grouped[{diagnostic.severity,diagnostic.option,diagnostic.message}];
            for(const auto&[key,count]:grouped)std::cout<<count<<"|severity:"<<std::get<0>(key)<<"|option:"<<std::get<1>(key)<<'|'<<std::get<2>(key)<<'\n';return 0;
        }
#ifdef CODEINSIGHT_WITH_ANALYSIS
        if(command=="analyze"){
            if(analysis!="dead-code")throw std::invalid_argument("--analysis must be dead-code");if(format!="records"&&format!="jsonl")throw std::invalid_argument("--format must be records or jsonl");const auto model=store.load_read_only(model_path);write_output(export_dead_code_candidates(model,format=="jsonl"),output_path);return 0;
        }
#endif
        usage();return 2;
    }catch(const std::exception& error){std::cerr<<"codeinsight: "<<error.what()<<'\n';return 1;}
}
