#include "codeinsight/codeinsight.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace codeinsight {
namespace {

using namespace std::string_view_literals;

constexpr std::array<std::uint32_t, 64> k{
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

template<class E> std::string_view enum_at(E value, std::span<const std::string_view> names) {
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "Invalid";
}

} // namespace

std::string sha256(std::string_view input) {
    std::vector<std::uint8_t> bytes(input.begin(), input.end());
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8;
    bytes.push_back(0x80);
    while ((bytes.size() % 64) != 56) bytes.push_back(0);
    for (int i = 7; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>(bit_length >> (i * 8)));

    std::array<std::uint32_t, 8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto j = offset + i * 4;
            w[i] = (static_cast<std::uint32_t>(bytes[j]) << 24) |
                   (static_cast<std::uint32_t>(bytes[j + 1]) << 16) |
                   (static_cast<std::uint32_t>(bytes[j + 2]) << 8) | bytes[j + 3];
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            const auto s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        auto [a,b,c,d,e,f,g,hh] = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const auto s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto t1 = hh + s1 + ch + k[i] + w[i];
            const auto s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto value : h) out << std::setw(8) << value;
    return out.str();
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open file: " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string read_file_hash(const std::filesystem::path& path) { return sha256(read_text_file(path)); }

std::string normalize_path(const std::filesystem::path& input, const std::filesystem::path& base) {
    if (input.empty()) return {};
    std::error_code error;
    auto path = input;
    if (path.is_relative()) path = (base.empty() ? std::filesystem::current_path() : base) / path;
    path = std::filesystem::absolute(path, error).lexically_normal();
    if (std::filesystem::exists(path, error)) {
        const auto canonical = std::filesystem::weakly_canonical(path, error);
        if (!error) path = canonical;
    }
    auto value = path.generic_string();
#ifdef _WIN32
    value = lower_ascii(value);
#endif
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

bool path_is_within(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto candidate=normalize_path(path);const auto boundary=normalize_path(root);
    if(candidate.empty()||boundary.empty())return false;
    if(candidate==boundary)return true;
    return candidate.size()>boundary.size()&&candidate.starts_with(boundary)&&candidate[boundary.size()]=='/';
}

CompilationScheduler::CompilationScheduler(std::size_t workers)
    : workers_(std::max<std::size_t>(1, workers)) {}

std::vector<FactBatch> CompilationScheduler::extract(
    const std::vector<CompilationRequest>& requests,
    const std::function<std::unique_ptr<ITranslationUnitFrontend>()>& factory) const {
    std::vector<FactBatch> results(requests.size());
    std::atomic_size_t next{};
    const auto count = std::min(workers_, std::max<std::size_t>(1, requests.size()));
    std::vector<std::jthread> threads;
    threads.reserve(count);
    for (std::size_t worker = 0; worker < count; ++worker) {
        threads.emplace_back([&] {
            std::unique_ptr<ITranslationUnitFrontend> frontend;
            std::string initialization_error;
            try {
                frontend = factory();
                if (!frontend) initialization_error = "frontend factory returned null";
            } catch (const std::exception& error) {
                initialization_error = error.what();
            } catch (...) {
                initialization_error = "frontend initialization failed with a non-standard exception";
            }
            for (;;) {
                const auto index = next.fetch_add(1);
                if (index >= requests.size()) break;
                try {
                    if (!frontend) throw std::runtime_error(initialization_error.empty()?"frontend initialization failed":initialization_error);
                    results[index] = frontend->extract(requests[index]);
                } catch (const std::exception& error) {
                    auto& batch = results[index];
                    batch.command = requests[index].command;
                    batch.revision_key = requests[index].revision;
                    batch.quality = ExtractionQuality::Failed;
                    batch.diagnostics.push_back({4, error.what(), "codeinsight-extractor", {}});
                } catch (...) {
                    auto& batch = results[index];
                    batch.command = requests[index].command;
                    batch.revision_key = requests[index].revision;
                    batch.quality = ExtractionQuality::Failed;
                    batch.diagnostics.push_back({4, "frontend extraction failed with a non-standard exception", "codeinsight-extractor", {}});
                }
            }
        });
    }
    // Join before the return value can move results. Relying on NRVO here
    // would let workers write through a moved-from vector when elision is off.
    threads.clear();
    return results;
}

std::string_view to_string(SymbolKind value) {
    static constexpr std::array names{"Unknown"sv,"Namespace"sv,"NamespaceAlias"sv,"Class"sv,"Struct"sv,"Union"sv,"Function"sv,"Method"sv,
        "Constructor"sv,"Destructor"sv,"ConversionFunction"sv,"OperatorFunction"sv,"Variable"sv,"Field"sv,"Parameter"sv,"Enum"sv,"Enumerator"sv,
        "Typedef"sv,"TypeAlias"sv,"ClassTemplate"sv,"FunctionTemplate"sv,"VariableTemplate"sv,"AliasTemplate"sv,"TemplateTypeParameter"sv,
        "TemplateNonTypeParameter"sv,"TemplateTemplateParameter"sv,"Macro"sv,"UsingDeclaration"sv,"UsingDirective"sv,"Lambda"sv,"LocalVariable"sv};
    return enum_at(value, names);
}

std::string_view to_string(TypeKind value) {
    static constexpr std::array names{"Invalid"sv,"Builtin"sv,"Named"sv,"Pointer"sv,"LValueReference"sv,"RValueReference"sv,"Array"sv,"Function"sv,
        "MemberPointer"sv,"TemplateSpecialization"sv,"Auto"sv,"Decltype"sv,"Dependent"sv,"Unknown"sv};
    return enum_at(value, names);
}

std::string_view to_string(RelationshipKind value) {
    static constexpr std::array names{"Contains"sv,"LexicalParent"sv,"SemanticParent"sv,"Declares"sv,"Defines"sv,"References"sv,"Reads"sv,"Writes"sv,
        "Calls"sv,"Constructs"sv,"Destroys"sv,"TakesAddress"sv,"Inherits"sv,"Overrides"sv,"UsesType"sv,"ReturnsType"sv,"ParameterType"sv,
        "FieldType"sv,"Instantiates"sv,"Specializes"sv,"Includes"sv,"CallableValueFlow"sv,"InvokesCallable"sv};
    return enum_at(value, names);
}

std::string_view to_string(ExtractionQuality value) {
    static constexpr std::array names{"Complete"sv,"CompleteWithWarnings"sv,"Partial"sv,"Failed"sv};
    return enum_at(value, names);
}

std::string_view to_string(ResolutionStatus value) {
    static constexpr std::array names{"Exact"sv,"Conservative"sv,"Ambiguous"sv,"Unresolved"sv};
    return enum_at(value, names);
}

std::string_view to_string(DispatchKind value) {
    static constexpr std::array names{"None"sv,"Static"sv,"Virtual"sv,"Dynamic"sv,"Unresolved"sv};
    return enum_at(value, names);
}

std::string_view to_string(EvidenceOrigin value) {
    static constexpr std::array names{"CompilerAST"sv,"CompilerPreprocessor"sv,"CompilationDatabase"sv,"DerivedCanonicalization"sv,
        "DerivedLogicalEquivalence"sv,"DerivedFlowAnalysis"sv};
    return enum_at(value, names);
}

std::string_view to_string(TargetDomain value) {
    static constexpr std::array names{"Project"sv,"External"sv,"Dependent"sv,"Indirect"sv,"Unknown"sv};
    return enum_at(value,names);
}

std::string_view to_string(ResolutionFailure value) {
    static constexpr std::array names{"None"sv,"NoProjectSymbol"sv,"AmbiguousProjectSymbol"sv,"CrossConfigurationVariant"sv,
        "UnsupportedImplicitCallable"sv,"DependentExpression"sv,"IndirectFlow"sv,"ExternalBoundary"sv,"Unknown"sv};
    return enum_at(value,names);
}

std::string_view to_string(BuildTargetKind value) {
    static constexpr std::array names{"Unknown"sv,"Executable"sv,"StaticLibrary"sv,"SharedLibrary"sv,"ObjectLibrary"sv,"Test"sv};
    return enum_at(value,names);
}

std::string_view to_string(EntryRootKind value) {
    static constexpr std::array names{"ProcessEntry"sv,"Export"sv,"Manual"sv,"AddressTaken"sv};
    return enum_at(value,names);
}

} // namespace codeinsight
