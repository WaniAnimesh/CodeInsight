#include "codeinsight/semantic/call_resolution.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace codeinsight;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    if (!out) throw std::runtime_error("cannot write fixture");
}

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("codeinsight-call-resolution-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { std::filesystem::create_directories(root); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};

std::set<std::string> targets(const SemanticModel& model, const CallTargetSet& call) {
    std::set<std::string> names;
    for (const auto& id : call.targets) names.insert(model.symbols.at(id).qualified_name);
    return names;
}

void test_call_outcomes() {
    SemanticModel model;
    Relationship external;
    external.id = RelationshipID{"external-call"};
    external.kind = RelationshipKind::Calls;
    external.origin = EvidenceOrigin::CompilerAST;
    external.dispatch = DispatchKind::Static;
    external.resolution = ResolutionStatus::Unresolved;
    external.target_domain = TargetDomain::External;
    external.resolution_failure = ResolutionFailure::ExternalBoundary;
    model.relationships.emplace(external.id, external);
    require(summarize_call_targets(model).front().completeness == CallTargetCompleteness::ExternalOnly,
        "authoritative external call was classified as unresolved project flow");
    model.relationships.at(external.id).dispatch = DispatchKind::Virtual;
    require(summarize_call_targets(model).front().completeness == CallTargetCompleteness::Incomplete,
        "external virtual declaration ruled out unknown project overrides");
    model.relationships.at(external.id).dispatch = DispatchKind::Dynamic;
    require(summarize_call_targets(model).front().completeness == CallTargetCompleteness::Incomplete,
        "indirect boundary incorrectly claimed external-only targets");
}
void test_scheduler_failures() {
    std::vector<CompilationRequest> requests(8);
    for(std::size_t i=0;i<requests.size();++i) requests[i].command.source_path="request-"+std::to_string(i)+".cpp";
    const auto check=[&](auto factory) {
        const auto results=CompilationScheduler{4}.extract(requests,factory);
        require(results.size()==requests.size(),"scheduler lost failed requests");
        for(std::size_t i=0;i<results.size();++i) {
            require(results[i].command.source_path==requests[i].command.source_path,"scheduler returned before every request completed");
            require(results[i].quality==ExtractionQuality::Failed&&!results[i].diagnostics.empty(),"frontend failure did not retain a diagnostic");
        }
    };
    check([]()->std::unique_ptr<ITranslationUnitFrontend>{throw std::runtime_error("initialization failure");});
    check([]()->std::unique_ptr<ITranslationUnitFrontend>{throw 7;});
    check([]()->std::unique_ptr<ITranslationUnitFrontend>{throw std::runtime_error("");});
    check([]()->std::unique_ptr<ITranslationUnitFrontend>{return nullptr;});
    struct BrokenFrontend final : ITranslationUnitFrontend {
        FactBatch extract(const CompilationRequest&) override {throw 7;}
    };
    check([]()->std::unique_ptr<ITranslationUnitFrontend>{return std::make_unique<BrokenFrontend>();});
}

void test_property_lifetime() {
    Fixture fixture;
    write_text(fixture.root / "shared.hpp", "struct Guard { ~Guard(); };\n");
    write_text(fixture.root / "a.cpp", "#include \"shared.hpp\"\nGuard::~Guard() = default;\n");
    write_text(fixture.root / "b.cpp", "#include \"shared.hpp\"\nvoid use() { Guard value; }\n");
    const auto directory=fixture.root.generic_string();
    const auto entry=[&](std::string name){return "{\"directory\":\""+directory+"\",\"file\":\""+name+"\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\""+name+"\"]}";};
    write_text(fixture.root / "compile_commands.json", "["+entry("a.cpp")+","+entry("b.cpp")+"]");
    IndexOptions options{fixture.root,fixture.root/"compile_commands.json",fixture.root/"model.db",2,true,{}};
    options.require_complete=true;
    const auto defaulted=[](const SemanticModel& model){
        for(const auto& [_,symbol]:model.symbols) if(symbol.qualified_name=="Guard::~Guard") return symbol.function.is_defaulted;
        throw std::runtime_error("destructor missing from property lifetime fixture");
    };
    const auto initial=IncrementalExecutor{}.build(options);
    require(defaulted(initial.model),"definition property lost behind header declaration");
    require(defaulted(SQLiteSnapshotStore{}.load_read_only(options.output)),"observation properties lost on reload");
    options.clean=false;
    write_text(fixture.root / "a.cpp", "#include \"shared.hpp\"\nGuard::~Guard() {}\n");
    const auto replaced=IncrementalExecutor{}.build(options);
    require(replaced.extracted_tus==1 && replaced.reused_tus==1,"property edit invalidated the wrong TUs");
    require(!defaulted(replaced.model),"defaulted property survived definition replacement");
    write_text(fixture.root / "a.cpp", "#include \"shared.hpp\"\nGuard::~Guard() = default;\n");
    require(defaulted(IncrementalExecutor{}.build(options).model),"restored definition property missing");
    write_text(fixture.root / "compile_commands.json", "["+entry("b.cpp")+"]");
    const auto removed=IncrementalExecutor{}.build(options);
    require(removed.extracted_tus==0 && removed.reused_tus==1,"TU removal unnecessarily re-extracted the surviving declaration");
    require(!defaulted(removed.model),"removed TU left its defaulted property on a surviving declaration");
    const auto incremental=normalized_snapshot(removed.model);
    options.clean=true;
    require(normalized_snapshot(IncrementalExecutor{}.build(options).model)==incremental,"property cleanup differs from a clean build");
    write_text(fixture.root / "b.cpp", "int value = 1;\n");
    options.clean=false;
    (void)IncrementalExecutor{}.build(options);
    write_text(fixture.root / "b.cpp", "double value = 1.0;\n");
    const auto changed_type=IncrementalExecutor{}.build(options);
    for(const auto& [_,symbol]:changed_type.model.symbols) if(symbol.name=="value")
        require(symbol.type&&changed_type.model.types.at(*symbol.type).canonical_spelling=="double","type survived same-USR revision replacement");
    options.clean=true;
    require(normalized_snapshot(IncrementalExecutor{}.build(options).model)==normalized_snapshot(changed_type.model),"incremental type change differs from clean extraction");
}

void run() {
    test_call_outcomes();
    test_property_lifetime();
    test_scheduler_failures();
    Fixture fixture;
    const auto source = fixture.root / "main.cpp";
    // No SDK dependencies: all regressions exercise compiler observations.
    const std::string code = R"CPP(using Callback = int (*)();
static int first() { return 1; }
static int second() { return 2; }
static int unrelated() { return 3; }
extern Callback unknown();
static int flows(bool choose) {
    Callback value = first;
    Callback copy(value);
    value = &second;
    Callback selected = choose ? first : second;
    Callback result = unknown();
    int total = copy() + selected() + result();
    { Callback value = &unrelated; total += value(); }
    return total + value();
}
static int cyclic(bool choose) {
    Callback left = first;
    Callback right = second;
    if (choose) left = right;
    else right = left;
    return left() + right();
}
static int argument(Callback passed) {
    Callback isolated = &first;
    return passed() + isolated();
}
struct Base { virtual int run() { return 1; } };
struct Derived : Base { int run() override { return 2; } };
static int dispatch(Base& value) { return value.run(); }
static int qualified(Derived& value) { return value.Base::run(); }
int main() { Derived value; return flows(false) + cyclic(false) + argument(first) + dispatch(value) + qualified(value); }
)CPP";
    write_text(source, code);
    const auto directory = fixture.root.generic_string();
    write_text(fixture.root / "compile_commands.json",
        "[{\"directory\":\"" + directory + "\",\"file\":\"main.cpp\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\"main.cpp\"]}]");
    IndexOptions options{fixture.root, fixture.root / "compile_commands.json", fixture.root / "model.db", 1, true, {}};
    options.require_complete = true;
    auto result = IncrementalExecutor{}.build(options);
    require(result.model.completeness.complete_tus == 1, "fixture was not complete");
    const auto calls = summarize_call_targets(result.model);
    const auto call_at = [&](std::string_view expression) -> const CallTargetSet& {
        const auto offset = code.find(expression);
        require(offset != std::string::npos, "expression missing in fixture");
        for (const auto& call : calls)
            if (result.model.relationships.at(call.observation).evidence.begin_offset == offset) return call;
        throw std::runtime_error("call observation missing: " + std::string(expression));
    };
    require(targets(result.model, call_at("copy()")) == std::set<std::string>{"first", "second"}, "copy/assignment flow did not propagate");
    require(targets(result.model, call_at("selected()")) == std::set<std::string>{"first", "second"}, "conditional flow did not propagate");
    require(call_at("copy()").completeness == CallTargetCompleteness::Incomplete, "local flow overstated completeness");
    require(call_at("result()").targets.empty(), "factory declaration was mistaken for its returned callback");
    require(targets(result.model, call_at("value(); }")) == std::set<std::string>{"unrelated"}, "shadowed callback mixed with outer storage");
    require(targets(result.model, call_at("value();\n}")) == std::set<std::string>{"first", "second"}, "outer callback mixed with shadowed storage");
    require(targets(result.model, call_at("left()")) == std::set<std::string>{"first", "second"}, "cyclic flow lost targets");
    require(targets(result.model, call_at("right()")) == std::set<std::string>{"first", "second"}, "cyclic flow did not converge");
    require(call_at("passed()").targets.empty(), "unsupported argument flow borrowed a local target");
    require(targets(result.model, call_at("isolated()")) == std::set<std::string>{"first"}, "local initializer target missing");
    require(targets(result.model, call_at("value.run()")) == std::set<std::string>{"Base::run", "Derived::run"}, "virtual override target missing");
    require(call_at("value.run()").completeness == CallTargetCompleteness::Incomplete, "open virtual hierarchy claimed complete");
    require(targets(result.model, call_at("value.Base::run()")) == std::set<std::string>{"Base::run"}, "qualified base call acquired override targets");
    require(call_at("value.Base::run()").completeness == CallTargetCompleteness::Complete, "qualified base call was not exact");
    require(call_at("unknown();\n    int total").completeness == CallTargetCompleteness::Complete, "project declaration call was not exact");

    const auto before = normalized_snapshot(result.model);
    resolve_call_targets(result.model);
    require(normalized_snapshot(result.model) == before, "resolution pass was not idempotent");
    require(normalized_snapshot(SQLiteSnapshotStore{}.load_read_only(options.output)) == before, "new flow relationships did not round trip");
    options.clean = false;
    const auto reused = IncrementalExecutor{}.build(options);
    require(reused.extracted_tus == 0 && reused.reused_tus == 1, "unchanged model was re-extracted");
    require(normalized_snapshot(reused.model) == before, "incremental resolution drifted");

    // Removing one initializer must remove its candidate after revision replacement.
    auto changed = code;
    const auto assignment = changed.find("value = &second;");
    changed.replace(assignment, std::string("value = &second;").size(), "value = &first;");
    write_text(source, changed);
    const auto updated = IncrementalExecutor{}.build(options);
    require(updated.extracted_tus == 1, "changed flow was not re-extracted");
    for (const auto& call : summarize_call_targets(updated.model)) {
        const auto& observation = updated.model.relationships.at(call.observation);
        if (observation.evidence.begin_offset == changed.find("copy()"))
            require(targets(updated.model, call) == std::set<std::string>{"first"}, "stale candidate survived incremental replacement");
    }
    require(SemanticModelValidator{}.validate(updated.model).ok(), "updated model is invalid");
}

} // namespace

int main() {
    try { run(); std::cout << "Call-resolution tests passed.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
