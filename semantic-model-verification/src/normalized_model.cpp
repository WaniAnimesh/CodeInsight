#include "semantic_test/normalized_model.hpp"
#include "semantic_test/assertions.hpp"
#include "semantic_test/query.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace semantic_test {
namespace {

std::string resolution(codeinsight::ResolutionStatus value) {
    switch (value) {
    case codeinsight::ResolutionStatus::Exact: return "Exact";
    case codeinsight::ResolutionStatus::Conservative: return "Conservative";
    case codeinsight::ResolutionStatus::Ambiguous: return "Ambiguous";
    case codeinsight::ResolutionStatus::Unresolved: return "Unresolved";
    }
    return "Unknown";
}
std::string dispatch(codeinsight::DispatchKind value) {
    switch (value) {
    case codeinsight::DispatchKind::None: return "None";
    case codeinsight::DispatchKind::Static: return "Static";
    case codeinsight::DispatchKind::Virtual: return "Virtual";
    case codeinsight::DispatchKind::Dynamic: return "Dynamic";
    case codeinsight::DispatchKind::Unresolved: return "Unresolved";
    }
    return "Unknown";
}
std::string linkage(codeinsight::Linkage value) {
    switch (value) {
    case codeinsight::Linkage::Invalid: return "Invalid";
    case codeinsight::Linkage::None: return "None";
    case codeinsight::Linkage::Internal: return "Internal";
    case codeinsight::Linkage::UniqueExternal: return "UniqueExternal";
    case codeinsight::Linkage::External: return "External";
    }
    return "Unknown";
}
std::string access(codeinsight::AccessSpecifier value) {
    switch (value) {
    case codeinsight::AccessSpecifier::Invalid: return "Invalid";
    case codeinsight::AccessSpecifier::Public: return "Public";
    case codeinsight::AccessSpecifier::Protected: return "Protected";
    case codeinsight::AccessSpecifier::Private: return "Private";
    }
    return "Unknown";
}
std::string relative(std::string path, const std::filesystem::path& workspace) {
    const auto root = codeinsight::normalize_path(workspace);
    if (path.starts_with(root)) {
        path.erase(0, root.size());
        while (!path.empty() && path.front() == '/') path.erase(path.begin());
    }
    return path;
}
std::string bool_text(bool value) { return value ? "true" : "false"; }
std::string selector(const SymbolExpectation& expectation) {
    return expectation.kind ? *expectation.kind + " " + expectation.qualified_name : expectation.qualified_name;
}
void expect(AssertionReport& report, bool condition, std::string message) {
    ++report.assertion_count;
    if (!condition) report.failures.push_back(std::move(message));
}

} // namespace

NormalizedSemanticModel normalize_model(const codeinsight::SemanticModel& model,
                                         const std::filesystem::path& workspace) {
    NormalizedSemanticModel result;
    result.translation_units = model.completeness.total_tus;
    result.complete_translation_units = model.completeness.complete_tus + model.completeness.warning_tus;
    result.partial_translation_units = model.completeness.partial_tus;
    result.failed_translation_units = model.completeness.failed_tus;

    std::map<codeinsight::SymbolID, std::string> symbol_names;
    for (const auto& [id, symbol] : model.symbols) symbol_names.emplace(id, symbol.qualified_name);

    for (const auto& [id, symbol] : model.symbols) {
        NormalizedSymbol normalized;
        normalized.qualified_name = symbol.qualified_name;
        normalized.kind = std::string(codeinsight::to_string(symbol.kind));
        normalized.linkage = linkage(symbol.linkage);
        if (symbol.type) {
            if (const auto found = model.types.find(*symbol.type); found != model.types.end()) {
                normalized.type_kind = std::string(codeinsight::to_string(found->second.kind));
                normalized.canonical_type = found->second.canonical_spelling;
            }
        }
        std::set<std::string> support_units;
        for (const auto& [_, occurrence] : model.occurrences) if (occurrence.symbol == id) {
            normalized.declarations += occurrence.role == codeinsight::OccurrenceRole::Declaration;
            normalized.definitions += occurrence.role == codeinsight::OccurrenceRole::Definition;
            const auto revision = model.revisions.find(occurrence.revision);
            if (revision != model.revisions.end()) {
                const auto unit = model.translation_units.find(revision->second.tu);
                if (unit != model.translation_units.end()) {
                    const auto file = model.files.find(unit->second.source);
                    if (file != model.files.end()) support_units.insert(relative(file->second.path, workspace));
                }
            }
        }
        normalized.key = normalized.kind + "|" + normalized.qualified_name + "|" +
                         normalized.linkage + "|" + normalized.canonical_type;
        if (symbol.linkage == codeinsight::Linkage::Internal ||
            symbol.linkage == codeinsight::Linkage::None || symbol.kind == codeinsight::SymbolKind::Lambda ||
            symbol.kind == codeinsight::SymbolKind::LocalVariable) {
            for (const auto& unit : support_units) normalized.key += "|tu:" + unit;
        }
        const auto& function = symbol.function;
        normalized.properties = {
            {"variadic", function.variadic}, {"static", function.is_static},
            {"virtual", function.is_virtual}, {"pure_virtual", function.is_pure_virtual},
            {"override", function.is_override}, {"final", function.is_final},
            {"const", function.is_const}, {"volatile", function.is_volatile},
            {"ref_lvalue", function.ref_lvalue}, {"ref_rvalue", function.ref_rvalue},
            {"constexpr", function.is_constexpr}, {"consteval", function.is_consteval},
            {"deleted", function.is_deleted}, {"defaulted", function.is_defaulted},
            {"explicit", function.is_explicit}, {"noexcept", function.is_noexcept},
            {"variable_static", symbol.variable.is_static},
            {"thread_local", symbol.variable.is_thread_local},
            {"mutable", symbol.variable.is_mutable},
            {"template_primary", symbol.templ.is_primary},
            {"partial_specialization", symbol.templ.is_partial_specialization},
            {"explicit_specialization", symbol.templ.is_explicit_specialization},
            {"instantiation", symbol.templ.is_instantiation},
            {"dependent", symbol.templ.dependent}
        };
        result.symbols.push_back(std::move(normalized));
    }

    for (const auto& [_, relationship] : model.relationships) {
        NormalizedRelationship normalized;
        normalized.kind = std::string(codeinsight::to_string(relationship.kind));
        if (const auto found = symbol_names.find(relationship.source); found != symbol_names.end())
            normalized.source = found->second;
        if (relationship.target_symbol) {
            if (const auto found = symbol_names.find(*relationship.target_symbol); found != symbol_names.end())
                normalized.target = found->second;
        } else if (relationship.target_type) {
            if (const auto found = model.types.find(*relationship.target_type); found != model.types.end())
                normalized.target = "type:" + found->second.canonical_spelling;
        } else normalized.target = "unresolved:" + relationship.unresolved_target;
        normalized.resolution = resolution(relationship.resolution);
        normalized.dispatch = dispatch(relationship.dispatch);
        normalized.access = access(relationship.access);
        normalized.is_virtual = relationship.is_virtual;
        normalized.is_dependent = relationship.is_dependent;
        result.relationships.push_back(std::move(normalized));
    }
    for (const auto& [_, diagnostic] : model.diagnostics)
        result.diagnostics.push_back(std::to_string(diagnostic.severity) + "|" + diagnostic.message);

    std::ranges::sort(result.symbols, {}, &NormalizedSymbol::key);
    std::ranges::sort(result.relationships, [](const auto& left, const auto& right) {
        return std::tie(left.kind, left.source, left.target, left.resolution, left.dispatch,
                        left.access, left.is_virtual, left.is_dependent) <
               std::tie(right.kind, right.source, right.target, right.resolution, right.dispatch,
                        right.access, right.is_virtual, right.is_dependent);
    });
    std::ranges::sort(result.diagnostics);
    return result;
}

std::string NormalizedSemanticModel::serialize() const {
    std::ostringstream out;
    out << "translation_units|" << translation_units << "|complete:" << complete_translation_units
        << "|partial:" << partial_translation_units << "|failed:" << failed_translation_units << '\n';
    for (const auto& symbol : symbols) {
        out << "symbol|" << symbol.key << "|declarations:" << symbol.declarations
            << "|definitions:" << symbol.definitions << "|type-kind:" << symbol.type_kind << '\n';
    }
    for (const auto& relationship : relationships) {
        out << "relationship|" << relationship.kind << '|' << relationship.source << '|'
            << relationship.target << '|' << relationship.resolution << '|' << relationship.dispatch
            << '|' << relationship.access << "|virtual:" << bool_text(relationship.is_virtual)
            << "|dependent:" << bool_text(relationship.is_dependent) << '\n';
    }
    for (const auto& diagnostic : diagnostics) out << "diagnostic|" << diagnostic << '\n';
    return out.str();
}

std::vector<const NormalizedSymbol*> find_symbols(const NormalizedSemanticModel& model,
                                                   std::string_view qualified_name,
                                                   std::string_view kind) {
    std::vector<const NormalizedSymbol*> result;
    for (const auto& symbol : model.symbols)
        if (symbol.qualified_name == qualified_name && (kind.empty() || symbol.kind == kind))
            result.push_back(&symbol);
    return result;
}

std::vector<const NormalizedRelationship*> find_relationships(
    const NormalizedSemanticModel& model, std::string_view source, std::string_view kind,
    std::string_view target) {
    std::vector<const NormalizedRelationship*> result;
    for (const auto& relationship : model.relationships)
        if (relationship.source == source && relationship.kind == kind &&
            (target.empty() || relationship.target == target)) result.push_back(&relationship);
    return result;
}

AssertionReport assert_expectations(const NormalizedSemanticModel& model,
                                     const SemanticExpectations& expectations) {
    AssertionReport report;
    if (expectations.translation_units)
        expect(report, model.translation_units == *expectations.translation_units,
               "expected " + std::to_string(*expectations.translation_units) +
               " translation units, actual " + std::to_string(model.translation_units));
    if (expectations.failed_translation_units)
        expect(report, model.failed_translation_units == *expectations.failed_translation_units,
               "expected " + std::to_string(*expectations.failed_translation_units) +
               " failed translation units, actual " + std::to_string(model.failed_translation_units));

    for (const auto& expected : expectations.symbols) {
        const auto found = find_symbols(model, expected.qualified_name, expected.kind.value_or(""));
        expect(report, found.size() == expected.count,
               "expected " + std::to_string(expected.count) + " symbol(s) for " + selector(expected) +
               ", actual " + std::to_string(found.size()));
        const auto sum = [&](auto member) { std::size_t value{}; for (const auto* symbol : found) value += symbol->*member; return value; };
        if (expected.declarations)
            expect(report, sum(&NormalizedSymbol::declarations) == *expected.declarations,
                   selector(expected) + " declaration count mismatch: expected " +
                   std::to_string(*expected.declarations) + ", actual " +
                   std::to_string(sum(&NormalizedSymbol::declarations)));
        if (expected.definitions)
            expect(report, sum(&NormalizedSymbol::definitions) == *expected.definitions,
                   selector(expected) + " definition count mismatch: expected " +
                   std::to_string(*expected.definitions) + ", actual " +
                   std::to_string(sum(&NormalizedSymbol::definitions)));
        if (expected.type_kind)
            expect(report, found.size() == 1 && found.front()->type_kind == *expected.type_kind,
                   selector(expected) + " type kind mismatch: expected " + *expected.type_kind +
                   (found.size() == 1 ? ", actual " + found.front()->type_kind : ", symbol is not unique"));
        if (expected.canonical_type)
            expect(report, found.size() == 1 && found.front()->canonical_type == *expected.canonical_type,
                   selector(expected) + " canonical type mismatch: expected " + *expected.canonical_type +
                   (found.size() == 1 ? ", actual " + found.front()->canonical_type : ", symbol is not unique"));
        for (const auto& [name, value] : expected.properties) {
            const bool matches = found.size() == 1 && found.front()->properties.contains(name) &&
                                 found.front()->properties.at(name) == value;
            expect(report, matches, selector(expected) + " property " + name +
                   " mismatch: expected " + bool_text(value));
        }
    }
    for (const auto& absent : expectations.absent_symbols) {
        const auto found = find_symbols(model, absent.qualified_name, absent.kind.value_or(""));
        expect(report, found.empty(), "unexpected symbol present: " + selector(absent));
    }

    auto check_relationship = [&](const RelationshipExpectation& expected, bool absent) {
        const auto found = find_relationships(model, expected.source, expected.kind,
                                               expected.target.value_or(""));
        std::size_t matching{};
        for (const auto* relationship : found) {
            if (expected.resolution && relationship->resolution != *expected.resolution) continue;
            if (expected.dispatch && relationship->dispatch != *expected.dispatch) continue;
            if (expected.is_virtual && relationship->is_virtual != *expected.is_virtual) continue;
            if (expected.is_dependent && relationship->is_dependent != *expected.is_dependent) continue;
            if (expected.access && relationship->access != *expected.access) continue;
            ++matching;
        }
        const auto description = expected.source + " --" + expected.kind + "--> " +
                                 expected.target.value_or("*");
        expect(report, absent ? matching == 0 : matching == expected.count,
               (absent ? "unexpected relationship present: " : "relationship count mismatch: ") +
               description + (absent ? "" : "; expected " + std::to_string(expected.count) +
                                      ", actual " + std::to_string(matching)));
    };
    for (const auto& relationship : expectations.relationships) check_relationship(relationship, false);
    for (const auto& relationship : expectations.absent_relationships) check_relationship(relationship, true);

    for (const auto& equivalence : expectations.type_equivalences) {
        const auto left = find_symbols(model, equivalence.left);
        const auto right = find_symbols(model, equivalence.right);
        expect(report, left.size() == 1 && right.size() == 1 &&
                       left.front()->canonical_type == right.front()->canonical_type,
               "canonical types differ: " + equivalence.left + " vs " + equivalence.right);
    }
    return report;
}

std::string semantic_diff(std::string_view expected, std::string_view actual) {
    auto lines = [](std::string_view text) {
        std::map<std::string, std::size_t> result;
        std::istringstream input{std::string(text)};
        for (std::string line; std::getline(input, line);) if (!line.empty()) ++result[line];
        return result;
    };
    const auto expected_lines = lines(expected);
    const auto actual_lines = lines(actual);
    std::ostringstream out;
    for (const auto& [line, count] : expected_lines) {
        const auto found = actual_lines.find(line);
        const auto actual_count = found == actual_lines.end() ? 0 : found->second;
        for (std::size_t index = actual_count; index < count; ++index) out << "Missing: " << line << '\n';
    }
    for (const auto& [line, count] : actual_lines) {
        const auto found = expected_lines.find(line);
        const auto expected_count = found == expected_lines.end() ? 0 : found->second;
        for (std::size_t index = expected_count; index < count; ++index) out << "Unexpected: " << line << '\n';
    }
    return out.str();
}

} // namespace semantic_test
