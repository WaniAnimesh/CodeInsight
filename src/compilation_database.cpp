#include "codeinsight/codeinsight.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <variant>

namespace codeinsight {
namespace {

struct Json {
    using array = std::vector<Json>;
    using object = std::map<std::string, Json>;
    std::variant<std::nullptr_t, bool, double, std::string, array, object> value;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}
    Json parse() { auto value = parse_value(); whitespace(); if (pos_ != text_.size()) fail("trailing input"); return value; }
private:
    std::string_view text_;
    std::size_t pos_{};
    [[noreturn]] void fail(std::string_view message) const { throw std::runtime_error("invalid compile_commands JSON at byte " + std::to_string(pos_) + ": " + std::string(message)); }
    void whitespace() { while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
    char take() { if (pos_ >= text_.size()) fail("unexpected end"); return text_[pos_++]; }
    bool consume(char c) { whitespace(); if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; } return false; }
    Json parse_value() {
        whitespace(); if (pos_ >= text_.size()) fail("missing value");
        if (text_[pos_] == '"') return Json{parse_string()};
        if (text_[pos_] == '[') return Json{parse_array()};
        if (text_[pos_] == '{') return Json{parse_object()};
        if (text_.substr(pos_, 4) == "null") { pos_ += 4; return Json{nullptr}; }
        if (text_.substr(pos_, 4) == "true") { pos_ += 4; return Json{true}; }
        if (text_.substr(pos_, 5) == "false") { pos_ += 5; return Json{false}; }
        const auto start = pos_;
        while (pos_ < text_.size() && std::string_view("-+0123456789.eE").find(text_[pos_]) != std::string_view::npos) ++pos_;
        double number{}; const auto token = text_.substr(start, pos_ - start);
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), number);
        if (ec != std::errc{} || ptr != token.data() + token.size()) fail("invalid value");
        return Json{number};
    }
    std::string parse_string() {
        if (take() != '"') fail("expected string"); std::string out;
        while (pos_ < text_.size()) {
            const char c = take(); if (c == '"') return out;
            if (c != '\\') { out.push_back(c); continue; }
            const char escape = take();
            switch (escape) {
            case '"': out.push_back('"'); break; case '\\': out.push_back('\\'); break; case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break; case 'f': out.push_back('\f'); break; case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break; case 't': out.push_back('\t'); break;
            case 'u': {
                if (pos_ + 4 > text_.size()) fail("short unicode escape");
                unsigned value{}; for (int i=0;i<4;++i) { const char h=take(); value <<= 4; if(h>='0'&&h<='9') value+=h-'0'; else if(h>='a'&&h<='f') value+=h-'a'+10; else if(h>='A'&&h<='F') value+=h-'A'+10; else fail("bad unicode escape"); }
                if (value < 0x80) out.push_back(static_cast<char>(value));
                else if (value < 0x800) { out.push_back(static_cast<char>(0xc0 | value >> 6)); out.push_back(static_cast<char>(0x80 | value & 0x3f)); }
                else { out.push_back(static_cast<char>(0xe0 | value >> 12)); out.push_back(static_cast<char>(0x80 | (value >> 6) & 0x3f)); out.push_back(static_cast<char>(0x80 | value & 0x3f)); }
                break;
            }
            default: fail("invalid escape");
            }
        }
        fail("unterminated string");
    }
    Json::array parse_array() {
        take(); Json::array values; if (consume(']')) return values;
        do { values.push_back(parse_value()); } while (consume(','));
        if (!consume(']')) fail("expected ]"); return values;
    }
    Json::object parse_object() {
        take(); Json::object values; if (consume('}')) return values;
        do { whitespace(); if (pos_ >= text_.size() || text_[pos_] != '"') fail("expected object key"); auto key=parse_string(); if(!consume(':')) fail("expected :"); values.emplace(std::move(key), parse_value()); } while (consume(','));
        if (!consume('}')) fail("expected }"); return values;
    }
};

const Json* field(const Json::object& object, std::string_view name) {
    const auto it = object.find(std::string(name)); return it == object.end() ? nullptr : &it->second;
}

std::string string_field(const Json::object& object, std::string_view name, bool required=true) {
    const auto* value = field(object, name);
    if (!value) { if (required) throw std::runtime_error("compile command missing field: " + std::string(name)); return {}; }
    const auto* string = std::get_if<std::string>(&value->value);
    if (!string) throw std::runtime_error("compile command field is not a string: " + std::string(name));
    return *string;
}

std::vector<std::string> split_command(std::string_view command) {
    std::vector<std::string> result;std::string current;char quote{};
    for(std::size_t i=0;i<command.size();++i){const char c=command[i];
        if(c=='\\'&&quote!='\''){
            std::size_t count=1;while(i+count<command.size()&&command[i+count]=='\\')++count;
            if(i+count<command.size()&&command[i+count]=='"'){current.append(count/2,'\\');if(count%2)current.push_back('"');else quote=quote=='"'?0:'"';i+=count;continue;}
            current.append(count,'\\');i+=count-1;continue;
        }
        if((c=='"'||c=='\'')&&(!quote||quote==c)){quote=quote?0:c;continue;}
        if(std::isspace(static_cast<unsigned char>(c))&&!quote){if(!current.empty()){result.push_back(std::move(current));current.clear();}continue;}
        current.push_back(c);
    }
    if (quote) throw std::runtime_error("unterminated quote in compile command");
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

bool starts(std::string_view value, std::string_view prefix) { return value.starts_with(prefix); }

void append_key(std::string& key,std::string_view value){key+=std::to_string(value.size());key+=':';key.append(value);key+=';';}

bool wrapper(std::string_view executable){
    auto name=std::filesystem::path(executable).filename().string();
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return name=="ccache"||name=="ccache.exe"||name=="sccache"||name=="sccache.exe"||name=="distcc"||name=="distcc.exe";
}

std::string resolve_compiler(std::string_view token,const std::filesystem::path& working_directory){
    std::filesystem::path input(token);std::error_code error;
    if(input.is_absolute()||input.has_parent_path())return normalize_path(input,working_directory);
    auto try_path=[&](const std::filesystem::path& directory)->std::string{
        auto candidate=directory/input;if(std::filesystem::is_regular_file(candidate,error))return normalize_path(candidate);
#ifdef _WIN32
        candidate+=L".exe";if(std::filesystem::is_regular_file(candidate,error))return normalize_path(candidate);
#endif
        return {};
    };
    if(auto found=try_path(working_directory);!found.empty())return found;
    std::string path_environment;
#ifdef _WIN32
    char* environment{};std::size_t environment_size{};if(_dupenv_s(&environment,&environment_size,"PATH")==0&&environment){path_environment=environment;std::free(environment);}
#else
    if(const char* environment=std::getenv("PATH"))path_environment=environment;
#endif
    if(!path_environment.empty()){
#ifdef _WIN32
        constexpr char separator=';';
#else
        constexpr char separator=':';
#endif
        std::string_view paths(path_environment);std::size_t begin{};
        while(begin<=paths.size()){
            const auto end=paths.find(separator,begin);const auto item=paths.substr(begin,end==std::string_view::npos?paths.size()-begin:end-begin);
            if(!item.empty())if(auto found=try_path(std::filesystem::path(item));!found.empty())return found;
            if(end==std::string_view::npos)break;begin=end+1;
        }
    }
    auto unresolved=std::string(token);
#ifdef _WIN32
    std::transform(unresolved.begin(),unresolved.end(),unresolved.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
#endif
    return "unresolved:"+unresolved;
}

std::string compiler_identity(std::string_view resolved){
    std::string material="compiler|";append_key(material,resolved);std::error_code error;const auto path=std::filesystem::path(resolved);
    if(std::filesystem::is_regular_file(path,error)){
        error.clear();const auto size=std::filesystem::file_size(path,error);if(!error)append_key(material,std::to_string(size));
        error.clear();const auto modified=std::filesystem::last_write_time(path,error);if(!error)append_key(material,std::to_string(modified.time_since_epoch().count()));
    }
    return material;
}

std::vector<std::string> expand_response_files(const std::vector<std::string>& arguments,const std::filesystem::path& working_directory,unsigned depth=0,std::set<std::string>* active=nullptr){
    if(depth>16)throw std::runtime_error("response-file nesting exceeds 16 levels");
    std::set<std::string> owned_active; if(!active)active=&owned_active;
    std::vector<std::string> result;
    for(const auto& argument:arguments){
        if(argument.size()>1&&argument[0]=='@'){
            const auto path=normalize_path(std::filesystem::path(argument.substr(1)),working_directory);
            if(!active->insert(path).second)throw std::runtime_error("response-file cycle: "+path);
            const auto expanded=expand_response_files(split_command(read_text_file(path)),working_directory,depth+1,active);
            result.insert(result.end(),expanded.begin(),expanded.end());active->erase(path);
        }else result.push_back(argument);
    }
    return result;
}

bool output_only_flag(std::string_view argument){
    return argument=="-c"||argument=="/c"||argument=="-S"||argument=="-E"||argument=="-M"||argument=="-MM"||
        argument=="-MD"||argument=="-MMD"||argument=="-MP"||argument=="-MG"||argument=="-save-temps"||argument=="-fsyntax-only"||
        argument=="/showIncludes"||argument=="/P"||argument=="/EP"||argument=="/E";
}

bool output_with_operand(std::string_view argument){
    return argument=="-o"||argument=="--output"||argument=="-MF"||argument=="-MT"||argument=="-MQ"||argument=="-MJ"||
        argument=="--serialize-diagnostics"||argument=="/sourceDependencies";
}

bool joined_output(std::string_view argument){
    return argument.starts_with("--output=")||argument.starts_with("--serialize-diagnostics=")||argument.starts_with("/sourceDependencies:")||
        (argument.size()>3&&(argument.starts_with("/Fo")||argument.starts_with("/Fd")||argument.starts_with("/Fe")||argument.starts_with("/Fa")||argument.starts_with("/Fi")));
}

bool path_with_operand(std::string_view argument){
    return argument=="-I"||argument=="-isystem"||argument=="-iquote"||argument=="-idirafter"||argument=="-iframework"||argument=="-F"||
        argument=="-include"||argument=="-imacros"||argument=="-include-pch"||argument=="-isysroot"||argument=="--sysroot"||
        argument=="-resource-dir"||argument=="-fmodule-map-file"||argument=="-fmodules-cache-path"||argument=="/I"||argument=="/FI"||
        argument=="/imsvc"||argument=="/external:I";
}

std::optional<std::string> normalize_joined_path(std::string_view argument,const std::filesystem::path& working_directory){
    const std::array<std::string_view,8> prefixes{"--sysroot=","-fmodule-map-file=","-fmodules-cache-path=","/external:I","/imsvc","/FI","/I","-I"};
    for(const auto prefix:prefixes)if(argument.starts_with(prefix)&&argument.size()>prefix.size())return std::string(prefix)+normalize_path(std::filesystem::path(argument.substr(prefix.size())),working_directory);
    return std::nullopt;
}

std::vector<std::string> semantic_arguments(const CompilationCommand& command,std::size_t driver_index){
    std::vector<std::string> tail(command.arguments.begin()+static_cast<std::ptrdiff_t>(driver_index+1),command.arguments.end());
    tail=expand_response_files(tail,command.working_directory);
    std::vector<std::string> result;const auto source=normalize_path(command.source_path);
    auto require_next=[&](std::size_t& i,std::string_view flag)->std::string{
        if(i+1>=tail.size())throw std::runtime_error("missing operand for compilation flag "+std::string(flag));return tail[++i];
    };
    for(std::size_t i=0;i<tail.size();++i){
        const auto& argument=tail[i];
        const std::filesystem::path possible_source(argument);if((possible_source.is_absolute()||(!argument.empty()&&argument[0]!='-'&&argument[0]!='/'))&&normalize_path(possible_source,command.working_directory)==source)continue;
        if(output_only_flag(argument)||joined_output(argument))continue;
        if(output_with_operand(argument)){(void)require_next(i,argument);continue;}
        if(path_with_operand(argument)){result.push_back(argument);result.push_back(normalize_path(std::filesystem::path(require_next(i,argument)),command.working_directory));continue;}
        if(auto normalized=normalize_joined_path(argument,command.working_directory)){result.push_back(std::move(*normalized));continue;}
        if((argument.starts_with("/Tc")||argument.starts_with("/Tp"))&&argument.size()>3){
            const auto candidate=normalize_path(std::filesystem::path(argument.substr(3)),command.working_directory);
            if(candidate==source){result.push_back(argument.starts_with("/Tc")?"/TC":"/TP");continue;}
        }
        result.push_back(argument);
    }
    auto compiler_name=std::filesystem::path(command.compiler).filename().string();
    std::transform(compiler_name.begin(),compiler_name.end(),compiler_name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(compiler_name=="cl"||compiler_name=="cl.exe"||compiler_name=="clang-cl"||compiler_name=="clang-cl.exe"){
        result.insert(result.begin(),"--driver-mode=cl");
        // libclang injects editor-oriented cc1 flags which clang-cl diagnoses as
        // unknown driver arguments. They do not affect semantic extraction.
        result.push_back("-Wno-unknown-argument");
    }
    return result;
}

void interpret(CompilationCommand& command) {
    const auto& args = command.semantic_arguments;
    auto path_arg = [&](std::string_view value) { return std::filesystem::path(normalize_path(std::filesystem::path(value), command.working_directory)); };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto next = [&]() -> std::string_view { if(i+1>=args.size())throw std::runtime_error("missing operand for compilation flag "+arg);return args[++i]; };
        if (arg == "-x") { const auto value=next(); command.language = value.find("++") != std::string_view::npos ? Language::Cxx : Language::C; }
        else if (starts(arg, "-std=")) command.standard = arg.substr(5);
        else if (arg=="-std") command.standard=std::string(next());
        else if (starts(arg, "/std:")) command.standard = arg.substr(5);
        else if(arg=="/TC")command.language=Language::C;
        else if(arg=="/TP")command.language=Language::Cxx;
        else if (arg == "-I" || arg == "/I") command.include_paths.push_back(path_arg(next()));
        else if (starts(arg, "-I") && arg.size() > 2) command.include_paths.push_back(path_arg(arg.substr(2)));
        else if (starts(arg, "/I") && arg.size() > 2) command.include_paths.push_back(path_arg(arg.substr(2)));
        else if (arg == "-isystem"||arg=="/imsvc"||arg=="/external:I") command.system_include_paths.push_back(path_arg(next()));
        else if (starts(arg, "-D") || starts(arg, "/D")) { auto value=arg.substr(2); if(value.empty()) value=std::string(next()); const auto equals=value.find('='); command.defines.push_back({value.substr(0,equals), equals==std::string::npos ? "1" : value.substr(equals+1)}); }
        else if (arg=="-D"||arg=="/D") {auto value=std::string(next());const auto equals=value.find('=');command.defines.push_back({value.substr(0,equals),equals==std::string::npos?"1":value.substr(equals+1)});}
        else if (arg=="-U"||arg=="/U") command.undefines.push_back(std::string(next()));
        else if (starts(arg, "-U") || starts(arg, "/U")) command.undefines.push_back(arg.substr(2));
        else if (arg == "-include" || arg == "/FI") command.forced_includes.push_back(path_arg(next()));
        else if (starts(arg, "/FI") && arg.size() > 3) command.forced_includes.push_back(path_arg(arg.substr(3)));
        else if (starts(arg, "--target=")) command.target.triple=arg.substr(9);
        else if (arg == "-target") command.target.triple=std::string(next());
    }
    if (command.language == Language::Unknown) {
        const auto extension = command.source_path.extension().string();
        command.language = extension == ".c" ? Language::C : Language::Cxx;
    }
}

std::string invocation_material(const CompilationCommand& command){
    std::string key="invocation|";append_key(key,normalize_path(command.working_directory));append_key(key,command.compiler_identity);for(const auto& argument:command.arguments)append_key(key,argument);return key;
}

std::string configuration_material(const CompilationCommand& command){
    std::string key="configuration|";append_key(key,command.compiler_identity);append_key(key,std::to_string(static_cast<int>(command.language)));for(const auto& argument:command.semantic_arguments)append_key(key,argument);return key;
}

} // namespace

std::vector<CompilationCommand> CompileCommandsProvider::load(
    const std::filesystem::path& database, WorkspaceID workspace) const {
    const auto root = JsonParser(read_text_file(database)).parse();
    const auto* entries = std::get_if<Json::array>(&root.value);
    if (!entries) throw std::runtime_error("compile_commands root must be an array");
    std::vector<CompilationCommand> result;
    result.reserve(entries->size());
    for (const auto& entry : *entries) {
        const auto* object = std::get_if<Json::object>(&entry.value);
        if (!object) throw std::runtime_error("compile_commands entry must be an object");
        CompilationCommand command;
        command.working_directory = std::filesystem::path(normalize_path(string_field(*object,"directory"),database.parent_path()));
        command.source_path = std::filesystem::path(normalize_path(string_field(*object, "file"), command.working_directory));
        if (const auto* arguments = field(*object, "arguments")) {
            const auto* array = std::get_if<Json::array>(&arguments->value);
            if (!array) throw std::runtime_error("arguments must be an array");
            for (const auto& arg : *array) {
                const auto* text = std::get_if<std::string>(&arg.value);
                if (!text) throw std::runtime_error("argument must be a string");
                command.arguments.push_back(*text);
            }
        } else command.arguments = split_command(string_field(*object, "command"));
        if (command.arguments.empty()) throw std::runtime_error("empty compilation command for " + command.source_path.string());
        std::size_t driver_index{};while(driver_index+1<command.arguments.size()&&wrapper(command.arguments[driver_index]))++driver_index;
        command.compiler = command.arguments[driver_index];
        const auto resolved_compiler=resolve_compiler(command.compiler,command.working_directory);
        command.compiler_identity=compiler_identity(resolved_compiler);
        command.source = FileID{sha256("file|" + workspace.value + "|" + normalize_path(command.source_path))};
        command.semantic_arguments=semantic_arguments(command,driver_index);
        interpret(command);
        command.configuration_key=configuration_material(command);
        command.fingerprint=sha256("compilation-fingerprint|"+command.configuration_key);
        command.invocation=CompilerInvocationID{sha256(invocation_material(command))};
        command.configuration=BuildConfigurationID{sha256(command.configuration_key)};
        result.push_back(std::move(command));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return std::tie(a.source.value,a.configuration.value,a.fingerprint,a.invocation.value)<std::tie(b.source.value,b.configuration.value,b.fingerprint,b.invocation.value);
    });
    std::map<BuildConfigurationID,std::string> configuration_keys;std::map<CompilerInvocationID,std::string> invocation_materials;
    std::vector<CompilationCommand> unique;unique.reserve(result.size());
    for(auto& command:result){
        const auto [configuration,inserted]=configuration_keys.emplace(command.configuration,command.configuration_key);
        if(!inserted&&configuration->second!=command.configuration_key)throw std::runtime_error("build-configuration hash collision: "+command.configuration.value);
        const auto material=invocation_material(command);const auto [invocation,invocation_inserted]=invocation_materials.emplace(command.invocation,material);
        if(!invocation_inserted&&invocation->second!=material)throw std::runtime_error("compiler-invocation hash collision: "+command.invocation.value);
        if(!unique.empty()&&unique.back().source==command.source&&unique.back().configuration==command.configuration){
            if(unique.back().fingerprint!=command.fingerprint||unique.back().semantic_arguments!=command.semantic_arguments)throw std::runtime_error("conflicting semantic commands share a translation-unit identity for "+command.source_path.string());
            continue;
        }
        unique.push_back(std::move(command));
    }
    return unique;
}

} // namespace codeinsight
