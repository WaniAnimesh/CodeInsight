#include "semantic_test/fixture_manifest.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <variant>

namespace semantic_test {
namespace {

struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;
};

class Parser {
public:
    explicit Parser(std::string text) : text_(std::move(text)) {}
    Json parse() {
        auto result = value();
        whitespace();
        if (position_ != text_.size()) fail("trailing content");
        return result;
    }
private:
    std::string text_;
    std::size_t position_{};

    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error("manifest JSON at byte " + std::to_string(position_) + ": " + std::string(message));
    }
    void whitespace() {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
    }
    bool consume(char expected) {
        whitespace();
        if (position_ < text_.size() && text_[position_] == expected) { ++position_; return true; }
        return false;
    }
    Json value() {
        whitespace();
        if (position_ >= text_.size()) fail("expected a value");
        switch (text_[position_]) {
        case '{': return object();
        case '[': return array();
        case '"': return Json{string()};
        case 't': literal("true"); return Json{true};
        case 'f': literal("false"); return Json{false};
        case 'n': literal("null"); return Json{nullptr};
        default: return Json{number()};
        }
    }
    void literal(std::string_view expected) {
        if (!std::string_view(text_).substr(position_).starts_with(expected)) fail("invalid literal");
        position_ += expected.size();
    }
    std::string string() {
        if (!consume('"')) fail("expected string");
        std::string result;
        while (position_ < text_.size()) {
            char c = text_[position_++];
            if (c == '"') return result;
            if (c != '\\') { result += c; continue; }
            if (position_ >= text_.size()) fail("unterminated escape");
            switch (c = text_[position_++]) {
            case '"': case '\\': case '/': result += c; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default: fail("unsupported escape; use UTF-8 directly");
            }
        }
        fail("unterminated string");
    }
    double number() {
        whitespace();
        const auto start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
        if (position_ == start || (position_ == start + 1 && text_[start] == '-')) fail("invalid number");
        return std::stod(text_.substr(start, position_ - start));
    }
    Json array() {
        consume('['); Json::Array result;
        if (consume(']')) return Json{std::move(result)};
        do { result.push_back(value()); } while (consume(','));
        if (!consume(']')) fail("expected ]");
        return Json{std::move(result)};
    }
    Json object() {
        consume('{'); Json::Object result;
        if (consume('}')) return Json{std::move(result)};
        do {
            whitespace();
            if (position_ >= text_.size() || text_[position_] != '"') fail("expected object key");
            auto key = string();
            if (!consume(':')) fail("expected :");
            if (!result.emplace(std::move(key), value()).second) fail("duplicate object key");
        } while (consume(','));
        if (!consume('}')) fail("expected }");
        return Json{std::move(result)};
    }
};

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open fixture manifest: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

const Json::Object& object(const Json& json, std::string_view context) {
    if (const auto* value = std::get_if<Json::Object>(&json.value)) return *value;
    throw std::runtime_error(std::string(context) + " must be an object");
}
const Json::Array& array(const Json& json, std::string_view context) {
    if (const auto* value = std::get_if<Json::Array>(&json.value)) return *value;
    throw std::runtime_error(std::string(context) + " must be an array");
}
const Json* member(const Json::Object& obj, std::string_view key) {
    const auto found = obj.find(std::string(key));
    return found == obj.end() ? nullptr : &found->second;
}
std::string string(const Json& json, std::string_view context) {
    if (const auto* value = std::get_if<std::string>(&json.value)) return *value;
    throw std::runtime_error(std::string(context) + " must be a string");
}
std::string required_string(const Json::Object& obj, std::string_view key) {
    const auto* value = member(obj, key);
    if (!value) throw std::runtime_error("fixture manifest requires '" + std::string(key) + "'");
    return string(*value, key);
}
std::optional<std::string> optional_string(const Json::Object& obj, std::string_view key) {
    if (const auto* value = member(obj, key)) return string(*value, key);
    return std::nullopt;
}
std::size_t integer(const Json& json, std::string_view context) {
    const auto* value = std::get_if<double>(&json.value);
    if (!value || *value < 0 || *value != static_cast<double>(static_cast<std::size_t>(*value)))
        throw std::runtime_error(std::string(context) + " must be a non-negative integer");
    return static_cast<std::size_t>(*value);
}
std::optional<std::size_t> optional_integer(const Json::Object& obj, std::string_view key) {
    if (const auto* value = member(obj, key)) return integer(*value, key);
    return std::nullopt;
}
std::optional<bool> optional_bool(const Json::Object& obj, std::string_view key) {
    if (const auto* value = member(obj, key)) {
        if (const auto* boolean = std::get_if<bool>(&value->value)) return *boolean;
        throw std::runtime_error(std::string(key) + " must be a boolean");
    }
    return std::nullopt;
}
std::vector<std::string> strings(const Json::Object& obj, std::string_view key) {
    std::vector<std::string> result;
    if (const auto* value = member(obj, key))
        for (const auto& item : array(*value, key)) result.push_back(string(item, key));
    return result;
}

SymbolExpectation symbol_expectation(const Json& json) {
    const auto& obj = object(json, "symbol expectation");
    SymbolExpectation result;
    result.qualified_name = required_string(obj, "qualified_name");
    result.kind = optional_string(obj, "kind");
    if (const auto count = optional_integer(obj, "count")) result.count = *count;
    result.declarations = optional_integer(obj, "declarations");
    result.definitions = optional_integer(obj, "definitions");
    result.type_kind = optional_string(obj, "type_kind");
    result.canonical_type = optional_string(obj, "canonical_type");
    if (const auto* properties = member(obj, "properties")) {
        for (const auto& [name, value] : object(*properties, "properties")) {
            const auto* flag = std::get_if<bool>(&value.value);
            if (!flag) throw std::runtime_error("symbol property must be boolean: " + name);
            result.properties.emplace(name, *flag);
        }
    }
    return result;
}

RelationshipExpectation relationship_expectation(const Json& json) {
    const auto& obj = object(json, "relationship expectation");
    RelationshipExpectation result;
    result.source = required_string(obj, "source");
    result.kind = required_string(obj, "kind");
    result.target = optional_string(obj, "target");
    if (const auto count = optional_integer(obj, "count")) result.count = *count;
    result.resolution = optional_string(obj, "resolution");
    result.dispatch = optional_string(obj, "dispatch");
    result.is_virtual = optional_bool(obj, "is_virtual");
    result.is_dependent = optional_bool(obj, "is_dependent");
    result.access = optional_string(obj, "access");
    return result;
}

} // namespace

FixtureManifest load_fixture_manifest(const std::filesystem::path& path) {
    const auto document = Parser(read(path)).parse();
    const auto& root = object(document, "fixture manifest");
    FixtureManifest result;
    result.manifest_path = std::filesystem::absolute(path).lexically_normal();
    result.name = required_string(root, "name");
    result.category = required_string(root, "category");
    if (const auto value = optional_string(root, "language")) result.language = *value;
    if (const auto value = optional_string(root, "standard")) result.standard = *value;
    for (const auto& source : strings(root, "sources")) result.sources.emplace_back(source);
    for (const auto& include : strings(root, "include_paths")) result.include_paths.emplace_back(include);
    result.defines = strings(root, "defines");
    result.compiler_arguments = strings(root, "compiler_arguments");
    if (result.sources.empty()) throw std::runtime_error("fixture has no sources: " + result.name);

    if (const auto* expected = member(root, "expect")) {
        const auto& obj = object(*expected, "expect");
        result.expect.translation_units = optional_integer(obj, "translation_units");
        result.expect.failed_translation_units = optional_integer(obj, "failed_translation_units");
        auto load_symbols = [&](std::string_view key, auto& destination) {
            if (const auto* list = member(obj, key))
                for (const auto& item : array(*list, key)) destination.push_back(symbol_expectation(item));
        };
        auto load_relationships = [&](std::string_view key, auto& destination) {
            if (const auto* list = member(obj, key))
                for (const auto& item : array(*list, key)) destination.push_back(relationship_expectation(item));
        };
        load_symbols("symbols", result.expect.symbols);
        load_symbols("absent_symbols", result.expect.absent_symbols);
        load_relationships("relationships", result.expect.relationships);
        load_relationships("absent_relationships", result.expect.absent_relationships);
        if (const auto* list = member(obj, "type_equivalences")) {
            for (const auto& item : array(*list, "type_equivalences")) {
                const auto& equivalence = object(item, "type equivalence");
                result.expect.type_equivalences.push_back(
                    {required_string(equivalence, "left"), required_string(equivalence, "right")});
            }
        }
    }
    return result;
}

std::vector<FixtureManifest> discover_fixtures(const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root))
        throw std::runtime_error("fixtures root is not a directory: " + root.string());
    std::vector<FixtureManifest> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.is_regular_file() && entry.path().filename() == "fixture.json")
            result.push_back(load_fixture_manifest(entry.path()));
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tie(left.category, left.name) < std::tie(right.category, right.name);
    });
    return result;
}

} // namespace semantic_test
