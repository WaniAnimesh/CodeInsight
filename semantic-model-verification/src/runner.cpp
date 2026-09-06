#include "semantic_test/runner.hpp"
#include "semantic_test/assertions.hpp"
#include "semantic_test/model_loader.hpp"
#include "semantic_test/normalized_model.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace semantic_test {
namespace {

std::string json_escape(std::string_view value) {
    std::string out;
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out;
}
std::string xml_escape(std::string_view value) {
    std::string out;
    for (const char c : value) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}
void write_text(const std::filesystem::path& path, std::string_view text) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + path.string());
    output << text;
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string stamp() {
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}
std::filesystem::path make_working_directory(std::string_view name) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("semantic-test-" + std::string(name) + "-" + stamp());
    std::filesystem::create_directories(path);
    return path;
}
void copy_project(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::filesystem::create_directories(target);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
        const auto relative = std::filesystem::relative(entry.path(), source);
        const auto destination = target / relative;
        if (entry.is_directory()) std::filesystem::create_directories(destination);
        else if (entry.is_regular_file()) {
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::copy_file(entry.path(), destination,
                                       std::filesystem::copy_options::overwrite_existing);
        }
    }
}
void write_compile_commands(const FixtureManifest& fixture, const std::filesystem::path& project) {
    std::ostringstream out;
    out << "[\n";
    for (std::size_t source_index = 0; source_index < fixture.sources.size(); ++source_index) {
        if (source_index) out << ",\n";
        const auto source = std::filesystem::absolute(project / fixture.sources[source_index]).lexically_normal();
        std::vector<std::string> arguments;
        arguments.push_back(fixture.language == "c" ? "clang" : "clang++");
        arguments.push_back("-x");
        arguments.push_back(fixture.language == "c" ? "c" : "c++");
        arguments.push_back("-std=" + fixture.standard);
        for (const auto& include : fixture.include_paths) {
            arguments.push_back("-I");
            arguments.push_back(std::filesystem::absolute(project / include).lexically_normal().generic_string());
        }
        for (const auto& define : fixture.defines) arguments.push_back("-D" + define);
        arguments.insert(arguments.end(), fixture.compiler_arguments.begin(), fixture.compiler_arguments.end());
        arguments.push_back("-c");
        arguments.push_back(source.generic_string());
        out << "  {\"directory\":\"" << json_escape(project.generic_string())
            << "\",\"file\":\"" << json_escape(source.generic_string()) << "\",\"arguments\":[";
        for (std::size_t argument_index = 0; argument_index < arguments.size(); ++argument_index) {
            if (argument_index) out << ',';
            out << '"' << json_escape(arguments[argument_index]) << '"';
        }
        out << "]}";
    }
    out << "\n]\n";
    write_text(project / "compile_commands.json", out.str());
}
std::filesystem::path retain_failure(const std::filesystem::path& working,
                                     const RunnerOptions& options,
                                     std::string_view name) {
    if (options.artifacts_root.empty()) return working;
    const auto destination = options.artifacts_root / (std::string(name) + "-" + stamp());
    copy_project(working, destination);
    std::filesystem::remove_all(working);
    return destination;
}
std::string validation_report(const codeinsight::ValidationResult& validation) {
    std::ostringstream out;
    if (validation.ok()) out << "OK\n";
    else for (const auto& issue : validation.issues) out << issue.code << ": " << issue.message << '\n';
    return out.str();
}

} // namespace

SemanticTestResult SemanticTestRunner::run_one(const FixtureManifest& fixture,
                                                const RunnerOptions& options) const {
    SemanticTestResult result;
    result.fixture = fixture.name;
    result.category = fixture.category;
    const auto started = std::chrono::steady_clock::now();
    const auto working = make_working_directory(fixture.name);
    bool keep_working = true;
    try {
        const auto project = working / "input-project";
        copy_project(fixture.manifest_path.parent_path(), project);
        write_compile_commands(fixture, project);
        const auto snapshot = working / "actual.db";
        codeinsight::IndexOptions index_options;
        index_options.workspace = project;
        index_options.compile_commands = project / "compile_commands.json";
        index_options.output = snapshot;
        index_options.workers = options.jobs;
        index_options.clean = true;
        const auto generated = codeinsight::IncrementalExecutor{}.build(index_options);
        const auto loaded = ModelLoader{}.load(snapshot);
        const auto validation = codeinsight::SemanticModelValidator{}.validate(loaded);
        write_text(working / "validator.txt", validation_report(validation));
        if (!validation.ok()) {
            result.failures.push_back("global model validation failed: " +
                                      validation.issues.front().code + ": " +
                                      validation.issues.front().message);
        } else {
            const auto normalized = normalize_model(loaded, project);
            const auto actual = normalized.serialize();
            write_text(working / "actual.normalized.txt", actual);
            result.diagnostic_count = normalized.diagnostics.size();
            if (options.inspect) std::cout << actual;

            const auto golden = fixture.manifest_path.parent_path() / "expected" /
                                "semantic.snapshot.txt";
            if (options.update_golden) write_text(golden, actual);
            else if (std::filesystem::exists(golden)) {
                const auto expected = read_text(golden);
                if (expected != actual) {
                    const auto difference = semantic_diff(expected, actual);
                    write_text(working / "expected.normalized.txt", expected);
                    write_text(working / "semantic.diff.txt", difference);
                    result.failures.push_back("golden semantic snapshot differs:\n" + difference);
                }
            }
            const auto assertions = assert_expectations(normalized, fixture.expect);
            result.semantic_assertions = assertions.assertion_count;
            result.failures.insert(result.failures.end(), assertions.failures.begin(),
                                   assertions.failures.end());
        }
        result.failed_assertions = result.failures.size();
        result.passed = result.failures.empty();
        write_text(working / "reproducibility.txt",
                   "fixture=" + fixture.name + "\nfixture_manifest=" +
                   fixture.manifest_path.generic_string() + "\nschema_version=" +
                   std::to_string(codeinsight::schema_version) + "\nextraction_version=" +
                   std::to_string(codeinsight::extraction_version) +
                   "\ncanonicalization_version=" +
                   std::to_string(codeinsight::canonicalization_version) +
                   "\ntranslation_units=" + std::to_string(generated.model.completeness.total_tus) + "\n");
        if (result.passed) { std::filesystem::remove_all(working); keep_working = false; }
        else { result.failure_artifacts = retain_failure(working, options, fixture.name); keep_working = false; }
    } catch (const std::exception& error) {
        result.failures.push_back(error.what());
        result.failed_assertions = result.failures.size();
        result.passed = false;
        if (keep_working) {
            write_text(working / "runner-error.txt", error.what());
            result.failure_artifacts = retain_failure(working, options, fixture.name);
            keep_working = false;
        }
    }
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

std::vector<SemanticTestResult> SemanticTestRunner::run(const RunnerOptions& options) const {
    auto fixtures = discover_fixtures(options.fixtures_root);
    std::vector<SemanticTestResult> results;
    for (const auto& fixture : fixtures) {
        if (options.category && fixture.category != *options.category) continue;
        if (options.fixture && fixture.name != *options.fixture) continue;
        results.push_back(run_one(fixture, options));
    }
    if (results.empty()) throw std::runtime_error("no fixtures matched the requested selection");
    return results;
}

void write_json_results(const std::vector<SemanticTestResult>& results,
                        const std::filesystem::path& path) {
    std::ostringstream out;
    out << "{\n  \"fixtures\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        if (index) out << ",\n";
        out << "    {\"fixture\":\"" << json_escape(result.fixture) << "\",\"category\":\""
            << json_escape(result.category) << "\",\"passed\":" << (result.passed ? "true" : "false")
            << ",\"duration_ms\":" << result.duration.count() << ",\"assertions\":"
            << result.semantic_assertions << ",\"failed_assertions\":" << result.failed_assertions
            << ",\"diagnostic_count\":" << result.diagnostic_count << ",\"failures\":[";
        for (std::size_t failure = 0; failure < result.failures.size(); ++failure) {
            if (failure) out << ',';
            out << '"' << json_escape(result.failures[failure]) << '"';
        }
        out << "]}";
    }
    out << "\n  ]\n}\n";
    write_text(path, out.str());
}

void write_junit_results(const std::vector<SemanticTestResult>& results,
                         const std::filesystem::path& path) {
    std::size_t failures{};
    std::chrono::milliseconds duration{};
    for (const auto& result : results) { failures += !result.passed; duration += result.duration; }
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"semantic-model-conformance\" tests=\""
        << results.size() << "\" failures=\"" << failures << "\" time=\""
        << duration.count() / 1000.0 << "\">\n";
    for (const auto& result : results) {
        out << "  <testcase classname=\"" << xml_escape(result.category) << "\" name=\""
            << xml_escape(result.fixture) << "\" time=\"" << result.duration.count() / 1000.0 << "\">\n";
        if (!result.passed) {
            std::ostringstream failures_text;
            for (const auto& failure : result.failures) failures_text << failure << '\n';
            out << "    <failure message=\"semantic conformance failure\">"
                << xml_escape(failures_text.str()) << "</failure>\n";
        }
        out << "  </testcase>\n";
    }
    out << "</testsuite>\n";
    write_text(path, out.str());
}

} // namespace semantic_test
