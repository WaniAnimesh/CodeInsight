#include "semantic_test/runner.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void usage() {
    std::cout << "semantic-test: CodeInsight canonical semantic-model conformance\n\n"
              << "  semantic-test run --all [--category NAME | --fixture NAME] [options]\n"
              << "  semantic-test inspect FIXTURE [options]\n"
              << "  semantic-test update-golden FIXTURE [options]\n"
              << "  semantic-test list [--fixtures PATH]\n\n"
              << "Options: --fixtures PATH --artifacts PATH --jobs N --json FILE --junit FILE\n";
}
std::string argument_value(int& index, int argc, char** argv) {
    if (index + 1 >= argc) throw std::invalid_argument(std::string("missing value for ") + argv[index]);
    return argv[++index];
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) { usage(); return 2; }
        const std::string command = argv[1];
        semantic_test::RunnerOptions options;
        options.fixtures_root = std::filesystem::current_path() /
                                "semantic-model-verification" / "fixtures";
        std::filesystem::path json_path;
        std::filesystem::path junit_path;
        int first_option = 2;
        if (command == "inspect" || command == "update-golden") {
            if (argc < 3) throw std::invalid_argument(command + " requires a fixture name");
            options.fixture = argv[2];
            options.inspect = command == "inspect";
            options.update_golden = command == "update-golden";
            first_option = 3;
        } else if (command != "run" && command != "list") {
            usage();
            return 2;
        }
        for (int index = first_option; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--all") {}
            else if (option == "--fixtures") options.fixtures_root = argument_value(index, argc, argv);
            else if (option == "--artifacts") options.artifacts_root = argument_value(index, argc, argv);
            else if (option == "--category") options.category = argument_value(index, argc, argv);
            else if (option == "--fixture") options.fixture = argument_value(index, argc, argv);
            else if (option == "--jobs") options.jobs = std::stoull(argument_value(index, argc, argv));
            else if (option == "--json") json_path = argument_value(index, argc, argv);
            else if (option == "--junit") junit_path = argument_value(index, argc, argv);
            else throw std::invalid_argument("unknown option: " + option);
        }
        if (command == "list") {
            for (const auto& fixture : semantic_test::discover_fixtures(options.fixtures_root))
                std::cout << fixture.category << '/' << fixture.name << '\n';
            return 0;
        }
        const auto results = semantic_test::SemanticTestRunner{}.run(options);
        std::size_t passed{};
        for (const auto& result : results) {
            std::cout << (result.passed ? "PASS" : "FAIL") << " [" << result.category << "] "
                      << result.fixture << " (" << result.duration.count() << " ms, "
                      << result.semantic_assertions << " assertions)\n";
            for (const auto& failure : result.failures) std::cout << "  " << failure << '\n';
            if (!result.failure_artifacts.empty())
                std::cout << "  artifacts: " << result.failure_artifacts.string() << '\n';
            passed += result.passed;
        }
        std::cout << "\nConformance: " << passed << '/' << results.size() << " fixtures passed.\n";
        if (!json_path.empty()) semantic_test::write_json_results(results, json_path);
        if (!junit_path.empty()) semantic_test::write_junit_results(results, junit_path);
        return passed == results.size() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "semantic-test: " << error.what() << '\n';
        return 2;
    }
}
