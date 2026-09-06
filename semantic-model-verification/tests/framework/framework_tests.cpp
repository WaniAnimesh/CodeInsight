#include "semantic_test/fixture.hpp"

#include <iostream>
#include <set>
#include <stdexcept>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
}

int main() {
    try {
        const auto fixtures = semantic_test::discover_fixtures(
            std::filesystem::path(SEMANTIC_TEST_SOURCE_DIR) / "fixtures");
        require(fixtures.size() >= 33, "seed suite must contain at least 33 fixtures");
        std::set<std::string> names;
        for (const auto& fixture : fixtures) {
            require(names.insert(fixture.name).second, "fixture names must be unique");
            require(!fixture.sources.empty(), "fixture must name at least one source");
        }
        semantic_test::NormalizedSemanticModel model;
        model.symbols.push_back({"Function|demo::run", "demo::run", "Function", "External",
                                 "Function", "void ()", 1, 1, {}});
        require(semantic_test::find_symbols(model, "demo::run", "Function").size() == 1,
                "symbol query failed");
        const auto difference = semantic_test::semantic_diff("symbol|A\n", "symbol|B\n");
        require(difference.find("Missing: symbol|A") != std::string::npos,
                "semantic diff omitted missing line");
        require(difference.find("Unexpected: symbol|B") != std::string::npos,
                "semantic diff omitted unexpected line");
        std::cout << "Semantic test framework tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Framework test failure: " << error.what() << '\n';
        return 1;
    }
}
