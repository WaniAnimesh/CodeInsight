#pragma once

#include "codeinsight/codeinsight.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace semantic_test {

struct SymbolExpectation {
    std::string qualified_name;
    std::optional<std::string> kind;
    std::size_t count{1};
    std::optional<std::size_t> declarations;
    std::optional<std::size_t> definitions;
    std::optional<std::string> type_kind;
    std::optional<std::string> canonical_type;
    std::map<std::string, bool> properties;
};

struct RelationshipExpectation {
    std::string source;
    std::string kind;
    std::optional<std::string> target;
    std::size_t count{1};
    std::optional<std::string> resolution;
    std::optional<std::string> dispatch;
    std::optional<bool> is_virtual;
    std::optional<bool> is_dependent;
    std::optional<std::string> access;
};

struct TypeEquivalenceExpectation { std::string left; std::string right; };

struct SemanticExpectations {
    std::optional<std::size_t> translation_units;
    std::optional<std::size_t> failed_translation_units;
    std::vector<SymbolExpectation> symbols;
    std::vector<SymbolExpectation> absent_symbols;
    std::vector<RelationshipExpectation> relationships;
    std::vector<RelationshipExpectation> absent_relationships;
    std::vector<TypeEquivalenceExpectation> type_equivalences;
};

struct FixtureManifest {
    std::filesystem::path manifest_path;
    std::string name;
    std::string category;
    std::string language{"c++"};
    std::string standard{"c++20"};
    std::vector<std::filesystem::path> sources;
    std::vector<std::filesystem::path> include_paths;
    std::vector<std::string> defines;
    std::vector<std::string> compiler_arguments;
    SemanticExpectations expect;
};

struct NormalizedSymbol {
    std::string key;
    std::string qualified_name;
    std::string kind;
    std::string linkage;
    std::string type_kind;
    std::string canonical_type;
    std::size_t declarations{};
    std::size_t definitions{};
    std::map<std::string, bool> properties;
};

struct NormalizedRelationship {
    std::string kind;
    std::string source;
    std::string target;
    std::string resolution;
    std::string dispatch;
    std::string access;
    bool is_virtual{};
    bool is_dependent{};
};

struct NormalizedSemanticModel {
    std::size_t translation_units{};
    std::size_t complete_translation_units{};
    std::size_t partial_translation_units{};
    std::size_t failed_translation_units{};
    std::vector<NormalizedSymbol> symbols;
    std::vector<NormalizedRelationship> relationships;
    std::vector<std::string> diagnostics;
    [[nodiscard]] std::string serialize() const;
};

struct AssertionReport {
    std::size_t assertion_count{};
    std::vector<std::string> failures;
    [[nodiscard]] bool ok() const noexcept { return failures.empty(); }
};

struct SemanticTestResult {
    std::string fixture;
    std::string category;
    bool passed{};
    std::chrono::milliseconds duration{};
    std::size_t semantic_assertions{};
    std::size_t failed_assertions{};
    std::size_t diagnostic_count{};
    std::vector<std::string> failures;
    std::filesystem::path failure_artifacts;
};

struct RunnerOptions {
    std::filesystem::path fixtures_root;
    std::filesystem::path artifacts_root;
    std::optional<std::string> category;
    std::optional<std::string> fixture;
    std::size_t jobs{1};
    bool inspect{};
    bool update_golden{};
};

class ModelLoader {
public:
    [[nodiscard]] codeinsight::SemanticModel load(const std::filesystem::path& snapshot) const {
        return codeinsight::SQLiteSnapshotStore{}.load_read_only(snapshot);
    }
};

class SemanticTestRunner {
public:
    [[nodiscard]] SemanticTestResult run_one(const FixtureManifest&, const RunnerOptions&) const;
    [[nodiscard]] std::vector<SemanticTestResult> run(const RunnerOptions&) const;
};

[[nodiscard]] FixtureManifest load_fixture_manifest(const std::filesystem::path& path);
[[nodiscard]] std::vector<FixtureManifest> discover_fixtures(const std::filesystem::path& root);
[[nodiscard]] NormalizedSemanticModel normalize_model(const codeinsight::SemanticModel& model,
                                                       const std::filesystem::path& workspace);
[[nodiscard]] std::string semantic_diff(std::string_view expected, std::string_view actual);
[[nodiscard]] std::vector<const NormalizedSymbol*> find_symbols(
    const NormalizedSemanticModel&, std::string_view qualified_name, std::string_view kind = {});
[[nodiscard]] std::vector<const NormalizedRelationship*> find_relationships(
    const NormalizedSemanticModel&, std::string_view source, std::string_view kind,
    std::string_view target = {});
[[nodiscard]] AssertionReport assert_expectations(const NormalizedSemanticModel&,
                                                   const SemanticExpectations&);
void write_json_results(const std::vector<SemanticTestResult>&, const std::filesystem::path&);
void write_junit_results(const std::vector<SemanticTestResult>&, const std::filesystem::path&);

} // namespace semantic_test
