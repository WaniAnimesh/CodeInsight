#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace codeinsight {

inline constexpr std::uint32_t schema_version = 4;
inline constexpr std::uint32_t extraction_version = 15;
inline constexpr std::uint32_t canonicalization_version = 8;
inline constexpr std::uint32_t flow_analysis_version = 3;

template<class Tag> struct StrongId {
    std::string value;
    [[nodiscard]] bool valid() const noexcept { return !value.empty(); }
    auto operator<=>(const StrongId&) const = default;
};

struct WorkspaceTag; struct BuildConfigurationTag; struct CompilerInvocationTag;
struct FileTag; struct FileVersionTag; struct TranslationUnitTag; struct TranslationUnitRevisionTag;
struct SymbolTag; struct OccurrenceTag; struct TypeTag; struct RelationshipTag;
struct MacroDefinitionTag; struct MacroExpansionTag; struct DiagnosticTag;
struct BuildTargetTag; struct TargetMembershipTag; struct TargetDependencyTag; struct EntryRootTag;
struct LogicalSymbolTag; struct IndirectCallSummaryTag;
using WorkspaceID = StrongId<WorkspaceTag>;
using BuildConfigurationID = StrongId<BuildConfigurationTag>;
using CompilerInvocationID = StrongId<CompilerInvocationTag>;
using FileID = StrongId<FileTag>;
using FileVersionID = StrongId<FileVersionTag>;
using TranslationUnitID = StrongId<TranslationUnitTag>;
using TranslationUnitRevisionID = StrongId<TranslationUnitRevisionTag>;
using SymbolID = StrongId<SymbolTag>;
using LogicalSymbolID = StrongId<LogicalSymbolTag>;
using OccurrenceID = StrongId<OccurrenceTag>;
using TypeID = StrongId<TypeTag>;
using RelationshipID = StrongId<RelationshipTag>;
using MacroDefinitionID = StrongId<MacroDefinitionTag>;
using MacroExpansionID = StrongId<MacroExpansionTag>;
using DiagnosticID = StrongId<DiagnosticTag>;
using BuildTargetID = StrongId<BuildTargetTag>;
using TargetMembershipID = StrongId<TargetMembershipTag>;
using TargetDependencyID = StrongId<TargetDependencyTag>;
using EntryRootID = StrongId<EntryRootTag>;
using IndirectCallSummaryID = StrongId<IndirectCallSummaryTag>;

[[nodiscard]] std::string sha256(std::string_view bytes);
[[nodiscard]] std::string read_file_hash(const std::filesystem::path& path);
[[nodiscard]] std::string normalize_path(const std::filesystem::path& path,
                                         const std::filesystem::path& base = {});
[[nodiscard]] bool path_is_within(const std::filesystem::path& path,
                                  const std::filesystem::path& root);
[[nodiscard]] std::string read_text_file(const std::filesystem::path& path);

enum class ResolutionStatus { Exact, Conservative, Ambiguous, Unresolved };
enum class EvidenceConfidence { CompilerGuaranteed, Derived, Unknown };
enum class EvidenceOrigin { CompilerAST, CompilerPreprocessor, CompilationDatabase, DerivedCanonicalization, DerivedLogicalEquivalence, DerivedFlowAnalysis };
enum class ExtractionQuality { Complete, CompleteWithWarnings, Partial, Failed };
enum class Language { C, Cxx, ObjectiveC, ObjectiveCxx, Unknown };
enum class Linkage { Invalid, None, Internal, UniqueExternal, External };
enum class Visibility { Invalid, Hidden, Protected, Default };
enum class AccessSpecifier { Invalid, Public, Protected, Private };
enum class OccurrenceRole { Declaration, Definition, Reference, Implicit };
enum class DispatchKind { None, Static, Virtual, Dynamic, Unresolved };
enum class IncludeKind { Quoted, Angled, Forced, Unknown };
enum class BuildTargetKind { Unknown, Executable, StaticLibrary, SharedLibrary, ObjectLibrary, Test };
enum class EntryRootKind { ProcessEntry, Export, Manual, AddressTaken };
enum class TargetDomain { Project, External, Dependent, Indirect, Unknown };
enum class ResolutionFailure {
    None, NoProjectSymbol, AmbiguousProjectSymbol, CrossConfigurationVariant,
    UnsupportedImplicitCallable, DependentExpression, IndirectFlow, ExternalBoundary, Unknown
};
enum class IntegerDomainKind { Unknown, ExactSet, UnsignedInterval, BitMask };

enum class SymbolKind {
    Unknown, Namespace, NamespaceAlias, Class, Struct, Union, Function, Method,
    Constructor, Destructor, ConversionFunction, OperatorFunction, Variable, Field,
    Parameter, Enum, Enumerator, Typedef, TypeAlias, ClassTemplate, FunctionTemplate,
    VariableTemplate, AliasTemplate, TemplateTypeParameter, TemplateNonTypeParameter,
    TemplateTemplateParameter, Macro, UsingDeclaration, UsingDirective, Lambda, LocalVariable
};

enum class TypeKind {
    Invalid, Builtin, Named, Pointer, LValueReference, RValueReference, Array, Function,
    MemberPointer, TemplateSpecialization, Auto, Decltype, Dependent, Unknown
};

enum class RelationshipKind {
    Contains, LexicalParent, SemanticParent, Declares, Defines, References, Reads, Writes,
    Calls, Constructs, Destroys, TakesAddress, Inherits, Overrides, UsesType, ReturnsType,
    ParameterType, FieldType, Instantiates, Specializes, Includes,
    CallableValueFlow, InvokesCallable
};

struct SourcePoint {
    std::string path;
    std::uint32_t line{};
    std::uint32_t column{};
    std::uint32_t offset{};
    auto operator<=>(const SourcePoint&) const = default;
};

struct RawSourceRange {
    SourcePoint begin;
    SourcePoint end;
    auto operator<=>(const RawSourceRange&) const = default;
};

struct SourceRange {
    FileVersionID file;
    std::uint32_t begin_line{}, begin_column{}, begin_offset{};
    std::uint32_t end_line{}, end_column{}, end_offset{};
    auto operator<=>(const SourceRange&) const = default;
};

struct Define { std::string name; std::string value; auto operator<=>(const Define&) const = default; };
struct TargetInfo { std::string triple; std::string architecture; std::string abi; auto operator<=>(const TargetInfo&) const = default; };

struct CompilationCommand {
    FileID source;
    std::filesystem::path source_path;
    std::filesystem::path working_directory;
    std::string compiler;
    std::string compiler_identity;
    Language language{Language::Unknown};
    std::string standard;
    std::vector<std::string> arguments;
    std::vector<std::string> semantic_arguments;
    std::vector<std::filesystem::path> include_paths;
    std::vector<std::filesystem::path> system_include_paths;
    std::vector<Define> defines;
    std::vector<std::string> undefines;
    std::vector<std::filesystem::path> forced_includes;
    TargetInfo target;
    BuildConfigurationID configuration;
    CompilerInvocationID invocation;
    std::string configuration_key;
    std::string fingerprint;
    std::vector<std::filesystem::path> project_roots;
};

class ICompilationDatabaseProvider {
public:
    virtual ~ICompilationDatabaseProvider() = default;
    [[nodiscard]] virtual std::vector<CompilationCommand>
    load(const std::filesystem::path& database, WorkspaceID workspace) const = 0;
};

class CompileCommandsProvider final : public ICompilationDatabaseProvider {
public:
    [[nodiscard]] std::vector<CompilationCommand>
    load(const std::filesystem::path& database, WorkspaceID workspace) const override;
};

struct TranslationUnitKey { FileID source; BuildConfigurationID configuration; auto operator<=>(const TranslationUnitKey&) const = default; };
struct TranslationUnitRevisionKey {
    TranslationUnitID tu;
    std::string source_hash;
    std::string compilation_fingerprint;
    std::uint32_t extractor_version{extraction_version};
    auto operator<=>(const TranslationUnitRevisionKey&) const = default;
};

struct File { FileID id; WorkspaceID workspace; std::string path; bool external{}; auto operator<=>(const File&) const = default; };
struct FileVersion { FileVersionID id; FileID file; std::string content_hash; std::uint64_t size{}; auto operator<=>(const FileVersion&) const = default; };

struct CVQualifiers { bool is_const{}, is_volatile{}, is_restrict{}; auto operator<=>(const CVQualifiers&) const = default; };

struct TypeFact {
    std::string local_identity;
    TypeKind kind{TypeKind::Unknown};
    CVQualifiers qualifiers;
    std::string spelling;
    std::string canonical_spelling;
    std::string named_symbol_usr;
    std::vector<std::string> children;
    std::vector<std::string> template_arguments;
    std::optional<std::uint64_t> array_extent;
    bool variadic{};
    bool dependent{};
};

struct FunctionProperties {
    bool variadic{}, is_static{}, is_virtual{}, is_pure_virtual{}, is_override{}, is_final{};
    bool is_const{}, is_volatile{}, ref_lvalue{}, ref_rvalue{};
    bool is_constexpr{}, is_consteval{}, is_deleted{}, is_defaulted{}, is_explicit{}, is_noexcept{};
};

struct VariableProperties { bool is_static{}, is_thread_local{}, is_constexpr{}, is_constinit{}, is_mutable{}; };
struct TemplateProperties { bool is_primary{}, is_partial_specialization{}, is_explicit_specialization{}, is_instantiation{}, dependent{}; };

struct SymbolFact {
    std::string local_identity;
    SymbolKind kind{SymbolKind::Unknown};
    std::string clang_usr;
    std::string name;
    std::string qualified_name;
    std::optional<std::string> semantic_parent;
    std::optional<std::string> lexical_parent;
    RawSourceRange spelling_range;
    RawSourceRange expansion_range;
    Linkage linkage{Linkage::Invalid};
    Visibility visibility{Visibility::Invalid};
    AccessSpecifier access{AccessSpecifier::Invalid};
    bool is_declaration{}, is_definition{}, is_implicit{}, is_anonymous{};
    bool exported{};
    std::string type_key;
    std::string result_type_key;
    std::vector<std::string> parameter_type_keys;
    FunctionProperties function;
    VariableProperties variable;
    TemplateProperties templ;
};

struct RelationshipFact {
    RelationshipKind kind{RelationshipKind::References};
    std::string source_local_key;
    std::string target_usr;
    std::string target_local_key;
    std::string target_type_key;
    std::string unresolved_target;
    std::string observed_target_spelling;
    RawSourceRange evidence;
    ResolutionStatus resolution{ResolutionStatus::Unresolved};
    DispatchKind dispatch{DispatchKind::None};
    EvidenceOrigin origin{EvidenceOrigin::CompilerAST};
    TargetDomain target_domain{TargetDomain::Unknown};
    ResolutionFailure resolution_failure{ResolutionFailure::None};
    AccessSpecifier access{AccessSpecifier::Invalid};
    bool is_virtual{}, is_dependent{};
};

struct MacroDefinitionFact { std::string name; std::string replacement; RawSourceRange range; };
struct MacroExpansionFact { std::string name; std::string definition_key; RawSourceRange range; };
struct IncludeFact { std::string including_path; std::string included_path; SourcePoint directive; IncludeKind kind{IncludeKind::Unknown}; bool resolved{}; };
struct DiagnosticFact { std::uint32_t severity{}; std::string message; std::string option; RawSourceRange range; };

struct FactBatch {
    CompilationCommand command;
    TranslationUnitRevisionKey revision_key;
    ExtractionQuality quality{ExtractionQuality::Failed};
    std::vector<SymbolFact> symbols;
    std::vector<TypeFact> types;
    std::vector<RelationshipFact> relationships;
    std::vector<MacroDefinitionFact> macro_definitions;
    std::vector<MacroExpansionFact> macro_expansions;
    std::vector<IncludeFact> includes;
    std::vector<DiagnosticFact> diagnostics;
};

struct CompilationRequest { CompilationCommand command; TranslationUnitID tu; TranslationUnitRevisionKey revision; };

class ITranslationUnitFrontend {
public:
    virtual ~ITranslationUnitFrontend() = default;
    [[nodiscard]] virtual FactBatch extract(const CompilationRequest& request) = 0;
};

class LibClangFrontend final : public ITranslationUnitFrontend {
public:
    LibClangFrontend();
    ~LibClangFrontend() override;
    LibClangFrontend(const LibClangFrontend&) = delete;
    LibClangFrontend& operator=(const LibClangFrontend&) = delete;
    [[nodiscard]] FactBatch extract(const CompilationRequest& request) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class CompilationScheduler {
public:
    explicit CompilationScheduler(std::size_t workers);
    [[nodiscard]] std::vector<FactBatch> extract(
        const std::vector<CompilationRequest>& requests,
        const std::function<std::unique_ptr<ITranslationUnitFrontend>()>& factory) const;
private:
    std::size_t workers_;
};

struct BuildConfiguration { BuildConfigurationID id; std::string canonical_key; std::string fingerprint; };
struct TranslationUnit { TranslationUnitID id; FileID source; BuildConfigurationID configuration; };
struct TranslationUnitRevision { TranslationUnitRevisionID id; TranslationUnitID tu; std::string source_hash; std::string compilation_fingerprint; std::uint32_t extractor_version{}; ExtractionQuality quality{ExtractionQuality::Failed}; };

struct Type {
    TypeID id;
    std::string canonical_key;
    TypeKind kind{TypeKind::Unknown};
    CVQualifiers qualifiers;
    std::string spelling;
    std::string canonical_spelling;
    std::optional<SymbolID> named_symbol;
    std::vector<TypeID> children;
    std::vector<std::string> template_arguments;
    std::optional<std::uint64_t> array_extent;
    bool variadic{}, dependent{};
};

struct Symbol {
    SymbolID id;
    std::string canonical_key;
    std::string usr;
    std::optional<LogicalSymbolID> logical_symbol;
    SymbolKind kind{SymbolKind::Unknown};
    std::string name;
    std::string qualified_name;
    Linkage linkage{Linkage::Invalid};
    Visibility visibility{Visibility::Invalid};
    std::optional<SymbolID> semantic_parent;
    std::optional<TypeID> type;
    FunctionProperties function;
    VariableProperties variable;
    TemplateProperties templ;
    bool exported{};
};

struct LogicalSymbol {
    LogicalSymbolID id;
    std::string canonical_key;
    std::string clang_usr;
    Linkage linkage{Linkage::Invalid};
    std::set<SymbolID> variants;
};

struct SymbolOccurrence {
    OccurrenceID id;
    SymbolID symbol;
    TranslationUnitRevisionID revision;
    SourceRange range;
    OccurrenceRole role{OccurrenceRole::Declaration};
    bool implicit{};
    // Properties belong to observations so removing a TU removes its claims.
    std::optional<TypeID> type;
    FunctionProperties function;
    VariableProperties variable;
    TemplateProperties templ;
    bool exported{};
};

struct Relationship {
    RelationshipID id;
    RelationshipKind kind{RelationshipKind::References};
    SymbolID source;
    std::optional<SymbolID> target_symbol;
    std::optional<TypeID> target_type;
    std::string unresolved_target;
    std::string observed_target_usr;
    std::string observed_target_spelling;
    TranslationUnitRevisionID observed_in;
    SourceRange evidence;
    ResolutionStatus resolution{ResolutionStatus::Unresolved};
    DispatchKind dispatch{DispatchKind::None};
    EvidenceOrigin origin{EvidenceOrigin::CompilerAST};
    TargetDomain target_domain{TargetDomain::Unknown};
    ResolutionFailure resolution_failure{ResolutionFailure::None};
    AccessSpecifier access{AccessSpecifier::Invalid};
    bool is_virtual{}, is_dependent{};
};

struct MacroDefinition { MacroDefinitionID id; std::string name; std::string replacement; TranslationUnitRevisionID revision; SourceRange range; };
struct MacroExpansion { MacroExpansionID id; std::string name; std::optional<MacroDefinitionID> definition; TranslationUnitRevisionID revision; SourceRange range; };
struct IncludeRelationship { RelationshipID id; TranslationUnitRevisionID revision; FileVersionID including_file; FileID included_file; SourcePoint directive; IncludeKind kind{IncludeKind::Unknown}; bool resolved{}; };
struct Diagnostic { DiagnosticID id; TranslationUnitRevisionID revision; std::uint32_t severity{}; std::string message; std::string option; SourceRange range; };

struct BuildTarget { BuildTargetID id; std::string name; BuildTargetKind kind{BuildTargetKind::Unknown}; std::string source; bool topology_authoritative{}; };
struct TargetMembership { TargetMembershipID id; BuildTargetID target; TranslationUnitID tu; };
struct TargetDependency { TargetDependencyID id; BuildTargetID consumer; BuildTargetID dependency; };
struct EntryRoot { EntryRootID id; SymbolID symbol; EntryRootKind kind{EntryRootKind::Manual}; std::string reason; std::optional<BuildConfigurationID> configuration; };

struct IntegerValueDomain {
    IntegerDomainKind kind{IntegerDomainKind::Unknown};
    std::set<std::uint64_t> exact_values;
    std::uint64_t minimum{};
    std::uint64_t maximum{};
    std::uint64_t known_zero_mask{};
};

// A derived, versioned proof for one indirect call observation.  Raw AST
// relationships remain untouched; this artifact records why a smaller target
// set is safe and which address observations were completely accounted for.
struct IndirectCallSummary {
    IndirectCallSummaryID id;
    RelationshipID call_observation;
    TranslationUnitRevisionID revision;
    BuildConfigurationID configuration;
    std::uint32_t analyzer_version{flow_analysis_version};
    SymbolID storage_provider;
    SymbolID index_provider;
    IntegerValueDomain index_domain;
    std::set<SymbolID> stored_targets;
    std::set<SymbolID> selectable_targets;
    std::set<RelationshipID> modeled_address_observations;
    bool storage_escapes{true};
    bool complete{};
    std::string reason;
};

struct ModelCompleteness {
    std::size_t total_tus{}, complete_tus{}, warning_tus{}, partial_tus{}, failed_tus{};
};

struct FrontendVersion {
    std::string configured;
    std::string runtime;
    std::uint32_t major{};
    [[nodiscard]] bool supported() const noexcept { return major >= 20; }
};

struct SemanticModel {
    WorkspaceID workspace;
    std::string workspace_path;
    std::map<BuildConfigurationID, BuildConfiguration> configurations;
    std::map<FileID, File> files;
    std::map<FileVersionID, FileVersion> file_versions;
    std::map<TranslationUnitID, TranslationUnit> translation_units;
    std::map<TranslationUnitRevisionID, TranslationUnitRevision> revisions;
    std::map<TypeID, Type> types;
    std::map<SymbolID, Symbol> symbols;
    std::map<LogicalSymbolID, LogicalSymbol> logical_symbols;
    std::map<OccurrenceID, SymbolOccurrence> occurrences;
    std::map<RelationshipID, Relationship> relationships;
    std::map<MacroDefinitionID, MacroDefinition> macro_definitions;
    std::map<MacroExpansionID, MacroExpansion> macro_expansions;
    std::map<RelationshipID, IncludeRelationship> includes;
    std::map<DiagnosticID, Diagnostic> diagnostics;
    std::map<BuildTargetID, BuildTarget> build_targets;
    std::map<TargetMembershipID, TargetMembership> target_memberships;
    std::map<TargetDependencyID, TargetDependency> target_dependencies;
    std::map<EntryRootID, EntryRoot> entry_roots;
    std::map<IndirectCallSummaryID, IndirectCallSummary> indirect_call_summaries;
    std::map<std::string, TranslationUnitRevisionID> active_revision_by_tu;
    ModelCompleteness completeness;
};

class Canonicalizer {
public:
    explicit Canonicalizer(SemanticModel& model);
    void apply(const FactBatch& batch);
    void detach(TranslationUnitID tu);
    void garbage_collect();
private:
    SemanticModel& model_;
    std::map<std::string, std::pair<FileID, FileVersionID>> file_cache_;
    std::map<BuildConfigurationID, std::map<std::string, std::set<SymbolID>>> usr_index_;
};

struct ValidationIssue { std::string code; std::string message; };
struct ValidationResult { std::vector<ValidationIssue> issues; [[nodiscard]] bool ok() const noexcept { return issues.empty(); } };

class SemanticModelValidator {
public:
    [[nodiscard]] ValidationResult validate(const SemanticModel& model) const;
};

class SnapshotVersionMismatch : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SQLiteSnapshotStore {
public:
    void publish(const SemanticModel& model, const std::filesystem::path& path) const;
    void publish_checkpoint(const SemanticModel& model, const std::filesystem::path& path) const;
    [[nodiscard]] SemanticModel load_read_only(const std::filesystem::path& path) const;
    [[nodiscard]] ValidationResult validate_file(const std::filesystem::path& path) const;
};

struct IndexOptions {
    std::filesystem::path workspace;
    std::filesystem::path compile_commands;
    std::filesystem::path output;
    std::size_t workers{};
    bool clean{};
    std::vector<std::filesystem::path> project_roots;
    std::vector<std::filesystem::path> additional_compile_commands;
    bool require_complete{};
    std::filesystem::path target_manifest;
    std::string target_name;
    BuildTargetKind target_kind{BuildTargetKind::Unknown};
    bool target_topology_authoritative{};
    std::vector<std::string> manual_entry_roots;
};

struct IndexResult { SemanticModel model; std::size_t extracted_tus{}; std::size_t reused_tus{}; };

class IncrementalExecutor {
public:
    [[nodiscard]] IndexResult build(const IndexOptions& options) const;
};

[[nodiscard]] std::string normalized_snapshot(const SemanticModel& model);
[[nodiscard]] FrontendVersion frontend_version();
[[nodiscard]] std::string export_observations(const SemanticModel& model, bool json_lines = false);
[[nodiscard]] std::string export_semantic_edges(const SemanticModel& model, bool json_lines = false);
[[nodiscard]] std::string_view to_string(SymbolKind value);
[[nodiscard]] std::string_view to_string(TypeKind value);
[[nodiscard]] std::string_view to_string(RelationshipKind value);
[[nodiscard]] std::string_view to_string(ExtractionQuality value);
[[nodiscard]] std::string_view to_string(ResolutionStatus value);
[[nodiscard]] std::string_view to_string(DispatchKind value);
[[nodiscard]] std::string_view to_string(EvidenceOrigin value);
[[nodiscard]] std::string_view to_string(TargetDomain value);
[[nodiscard]] std::string_view to_string(ResolutionFailure value);
[[nodiscard]] std::string_view to_string(BuildTargetKind value);
[[nodiscard]] std::string_view to_string(EntryRootKind value);


} // namespace codeinsight
