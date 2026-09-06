#include "codeinsight/codeinsight.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <chrono>
#include <stdexcept>

namespace codeinsight {
namespace {

class Database {
public:
    Database(const std::filesystem::path& path, int flags) {
        const auto utf8=path.u8string();
        if(sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()),&db_,flags,nullptr)!=SQLITE_OK) {
            const auto message=db_?sqlite3_errmsg(db_):"cannot allocate SQLite handle"; if(db_) sqlite3_close(db_); db_=nullptr;
            throw std::runtime_error("SQLite open failed: "+std::string(message));
        }
    }
    ~Database(){if(db_) sqlite3_close(db_);}
    Database(const Database&)=delete; Database& operator=(const Database&)=delete;
    sqlite3* get() const{return db_;}
    void exec(const char* sql) const { char* message{}; if(sqlite3_exec(db_,sql,nullptr,nullptr,&message)!=SQLITE_OK){std::string text=message?message:sqlite3_errmsg(db_);sqlite3_free(message);throw std::runtime_error("SQLite execution failed: "+text);} }
private: sqlite3* db_{};
};

class Statement {
public:
    Statement(sqlite3* db,const char* sql):db_(db){if(sqlite3_prepare_v2(db,sql,-1,&stmt_,nullptr)!=SQLITE_OK)throw std::runtime_error("SQLite prepare failed: "+std::string(sqlite3_errmsg(db)));}
    ~Statement(){if(stmt_)sqlite3_finalize(stmt_);}
    void text(int index,std::string_view value){if(sqlite3_bind_text(stmt_,index,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT)!=SQLITE_OK)fail();}
    void integer(int index,std::int64_t value){if(sqlite3_bind_int64(stmt_,index,value)!=SQLITE_OK)fail();}
    void null(int index){if(sqlite3_bind_null(stmt_,index)!=SQLITE_OK)fail();}
    void optional_text(int index,const std::optional<std::string>& value){value?text(index,*value):null(index);}
    void run(){if(sqlite3_step(stmt_)!=SQLITE_DONE)fail();sqlite3_reset(stmt_);sqlite3_clear_bindings(stmt_);}
    bool row(){const auto rc=sqlite3_step(stmt_);if(rc==SQLITE_ROW)return true;if(rc==SQLITE_DONE)return false;fail();}
    std::string string(int index)const{const auto* p=sqlite3_column_text(stmt_,index);return p?reinterpret_cast<const char*>(p):"";}
    std::int64_t number(int index)const{return sqlite3_column_int64(stmt_,index);}
    bool is_null(int index)const{return sqlite3_column_type(stmt_,index)==SQLITE_NULL;}
private:
    [[noreturn]]void fail()const{throw std::runtime_error("SQLite statement failed: "+std::string(sqlite3_errmsg(db_)));}
    sqlite3* db_{};sqlite3_stmt* stmt_{};
};

void create_schema(const Database& db){
    db.exec(R"SQL(
PRAGMA foreign_keys=ON;
PRAGMA journal_mode=DELETE;
CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
CREATE TABLE configurations(id TEXT PRIMARY KEY,canonical_key TEXT NOT NULL UNIQUE,fingerprint TEXT NOT NULL);
CREATE TABLE files(id TEXT PRIMARY KEY,workspace TEXT NOT NULL,path TEXT NOT NULL UNIQUE,external INTEGER NOT NULL);
CREATE TABLE file_versions(id TEXT PRIMARY KEY,file_id TEXT NOT NULL REFERENCES files(id),content_hash TEXT NOT NULL,size INTEGER NOT NULL,UNIQUE(file_id,content_hash));
CREATE TABLE translation_units(id TEXT PRIMARY KEY,source_file TEXT NOT NULL REFERENCES files(id),configuration_id TEXT NOT NULL REFERENCES configurations(id),UNIQUE(source_file,configuration_id));
CREATE TABLE revisions(id TEXT PRIMARY KEY,tu_id TEXT NOT NULL REFERENCES translation_units(id),source_hash TEXT NOT NULL,compilation_fingerprint TEXT NOT NULL,extractor_version INTEGER NOT NULL,quality INTEGER NOT NULL);
CREATE TABLE types(id TEXT PRIMARY KEY,canonical_key TEXT NOT NULL UNIQUE,kind INTEGER NOT NULL,cv INTEGER NOT NULL,spelling TEXT NOT NULL,canonical_spelling TEXT NOT NULL,named_symbol TEXT,array_extent INTEGER,variadic INTEGER NOT NULL,dependent INTEGER NOT NULL);
CREATE TABLE type_children(parent_id TEXT NOT NULL REFERENCES types(id),position INTEGER NOT NULL,child_id TEXT NOT NULL REFERENCES types(id),PRIMARY KEY(parent_id,position));
CREATE TABLE type_arguments(parent_id TEXT NOT NULL REFERENCES types(id),position INTEGER NOT NULL,value TEXT NOT NULL,PRIMARY KEY(parent_id,position));
CREATE TABLE logical_symbols(id TEXT PRIMARY KEY,canonical_key TEXT NOT NULL UNIQUE,usr TEXT NOT NULL,linkage INTEGER NOT NULL);
CREATE TABLE symbols(id TEXT PRIMARY KEY,canonical_key TEXT NOT NULL UNIQUE,usr TEXT NOT NULL,logical_id TEXT REFERENCES logical_symbols(id),kind INTEGER NOT NULL,name TEXT NOT NULL,qualified_name TEXT NOT NULL,linkage INTEGER NOT NULL,visibility INTEGER NOT NULL,semantic_parent TEXT,type_id TEXT,function_flags INTEGER NOT NULL,variable_flags INTEGER NOT NULL,template_flags INTEGER NOT NULL,exported INTEGER NOT NULL);
CREATE TABLE occurrences(id TEXT PRIMARY KEY,symbol_id TEXT NOT NULL REFERENCES symbols(id),revision_id TEXT NOT NULL REFERENCES revisions(id),file_version_id TEXT NOT NULL REFERENCES file_versions(id),bl INTEGER,bc INTEGER,bo INTEGER,el INTEGER,ec INTEGER,eo INTEGER,role INTEGER NOT NULL,implicit INTEGER NOT NULL,type_id TEXT REFERENCES types(id),function_flags INTEGER NOT NULL,variable_flags INTEGER NOT NULL,template_flags INTEGER NOT NULL,exported INTEGER NOT NULL);
CREATE TABLE relationships(id TEXT PRIMARY KEY,kind INTEGER NOT NULL,source_id TEXT NOT NULL REFERENCES symbols(id),target_symbol TEXT,target_type TEXT,unresolved TEXT NOT NULL,observed_target_usr TEXT NOT NULL,observed_target_spelling TEXT NOT NULL,revision_id TEXT NOT NULL REFERENCES revisions(id),file_version_id TEXT NOT NULL REFERENCES file_versions(id),bl INTEGER,bc INTEGER,bo INTEGER,el INTEGER,ec INTEGER,eo INTEGER,resolution INTEGER NOT NULL,dispatch INTEGER NOT NULL,origin INTEGER NOT NULL,target_domain INTEGER NOT NULL,resolution_failure INTEGER NOT NULL,access INTEGER NOT NULL,is_virtual INTEGER NOT NULL,is_dependent INTEGER NOT NULL);
CREATE TABLE macro_definitions(id TEXT PRIMARY KEY,name TEXT NOT NULL,replacement TEXT NOT NULL,revision_id TEXT NOT NULL REFERENCES revisions(id),file_version_id TEXT NOT NULL REFERENCES file_versions(id),bl INTEGER,bc INTEGER,bo INTEGER,el INTEGER,ec INTEGER,eo INTEGER);
CREATE TABLE macro_expansions(id TEXT PRIMARY KEY,name TEXT NOT NULL,definition_id TEXT,revision_id TEXT NOT NULL REFERENCES revisions(id),file_version_id TEXT NOT NULL REFERENCES file_versions(id),bl INTEGER,bc INTEGER,bo INTEGER,el INTEGER,ec INTEGER,eo INTEGER);
CREATE TABLE includes(id TEXT PRIMARY KEY,revision_id TEXT NOT NULL REFERENCES revisions(id),including_version TEXT NOT NULL REFERENCES file_versions(id),included_file TEXT NOT NULL REFERENCES files(id),directive_path TEXT NOT NULL,line INTEGER,column_no INTEGER,offset_no INTEGER,kind INTEGER NOT NULL,resolved INTEGER NOT NULL);
CREATE TABLE diagnostics(id TEXT PRIMARY KEY,revision_id TEXT NOT NULL REFERENCES revisions(id),severity INTEGER NOT NULL,message TEXT NOT NULL,option_name TEXT NOT NULL,file_version_id TEXT NOT NULL REFERENCES file_versions(id),bl INTEGER,bc INTEGER,bo INTEGER,el INTEGER,ec INTEGER,eo INTEGER);
CREATE TABLE build_targets(id TEXT PRIMARY KEY,name TEXT NOT NULL,kind INTEGER NOT NULL,source TEXT NOT NULL,topology_authoritative INTEGER NOT NULL);
CREATE TABLE target_memberships(id TEXT PRIMARY KEY,target_id TEXT NOT NULL REFERENCES build_targets(id),tu_id TEXT NOT NULL REFERENCES translation_units(id),UNIQUE(target_id,tu_id));
CREATE TABLE target_dependencies(id TEXT PRIMARY KEY,consumer_id TEXT NOT NULL REFERENCES build_targets(id),dependency_id TEXT NOT NULL REFERENCES build_targets(id),UNIQUE(consumer_id,dependency_id));
CREATE TABLE entry_roots(id TEXT PRIMARY KEY,symbol_id TEXT NOT NULL REFERENCES symbols(id),kind INTEGER NOT NULL,reason TEXT NOT NULL,configuration_id TEXT REFERENCES configurations(id));
CREATE TABLE indirect_call_summaries(id TEXT PRIMARY KEY,call_observation TEXT NOT NULL REFERENCES relationships(id),revision_id TEXT NOT NULL REFERENCES revisions(id),configuration_id TEXT NOT NULL REFERENCES configurations(id),analyzer_version INTEGER NOT NULL,storage_provider TEXT NOT NULL REFERENCES symbols(id),index_provider TEXT NOT NULL REFERENCES symbols(id),domain_kind INTEGER NOT NULL,minimum_value TEXT NOT NULL,maximum_value TEXT NOT NULL,known_zero_mask TEXT NOT NULL,storage_escapes INTEGER NOT NULL,complete INTEGER NOT NULL,reason TEXT NOT NULL);
CREATE TABLE indirect_domain_values(summary_id TEXT NOT NULL REFERENCES indirect_call_summaries(id),value TEXT NOT NULL,PRIMARY KEY(summary_id,value));
CREATE TABLE indirect_stored_targets(summary_id TEXT NOT NULL REFERENCES indirect_call_summaries(id),symbol_id TEXT NOT NULL REFERENCES symbols(id),PRIMARY KEY(summary_id,symbol_id));
CREATE TABLE indirect_selectable_targets(summary_id TEXT NOT NULL REFERENCES indirect_call_summaries(id),symbol_id TEXT NOT NULL REFERENCES symbols(id),PRIMARY KEY(summary_id,symbol_id));
CREATE TABLE indirect_modeled_addresses(summary_id TEXT NOT NULL REFERENCES indirect_call_summaries(id),relationship_id TEXT NOT NULL REFERENCES relationships(id),PRIMARY KEY(summary_id,relationship_id));
CREATE TABLE active_revisions(tu_id TEXT PRIMARY KEY,revision_id TEXT NOT NULL REFERENCES revisions(id));
)SQL");
}

void create_indexes(const Database& db){
    db.exec("CREATE INDEX occurrence_revision ON occurrences(revision_id);CREATE INDEX relationship_revision ON relationships(revision_id);CREATE INDEX include_included ON includes(included_file);");
}

void bind_range(Statement& statement,int start,const SourceRange& range){
    statement.text(start,range.file.value);statement.integer(start+1,range.begin_line);statement.integer(start+2,range.begin_column);statement.integer(start+3,range.begin_offset);
    statement.integer(start+4,range.end_line);statement.integer(start+5,range.end_column);statement.integer(start+6,range.end_offset);
}
SourceRange get_range(const Statement& statement,int start){return {FileVersionID{statement.string(start)},static_cast<std::uint32_t>(statement.number(start+1)),static_cast<std::uint32_t>(statement.number(start+2)),static_cast<std::uint32_t>(statement.number(start+3)),static_cast<std::uint32_t>(statement.number(start+4)),static_cast<std::uint32_t>(statement.number(start+5)),static_cast<std::uint32_t>(statement.number(start+6))};}

std::uint32_t function_flags(const FunctionProperties& p){return static_cast<std::uint32_t>(p.variadic)|(static_cast<std::uint32_t>(p.is_static)<<1)|(static_cast<std::uint32_t>(p.is_virtual)<<2)|(static_cast<std::uint32_t>(p.is_pure_virtual)<<3)|(static_cast<std::uint32_t>(p.is_override)<<4)|(static_cast<std::uint32_t>(p.is_final)<<5)|(static_cast<std::uint32_t>(p.is_const)<<6)|(static_cast<std::uint32_t>(p.is_volatile)<<7)|(static_cast<std::uint32_t>(p.ref_lvalue)<<8)|(static_cast<std::uint32_t>(p.ref_rvalue)<<9)|(static_cast<std::uint32_t>(p.is_constexpr)<<10)|(static_cast<std::uint32_t>(p.is_consteval)<<11)|(static_cast<std::uint32_t>(p.is_deleted)<<12)|(static_cast<std::uint32_t>(p.is_defaulted)<<13)|(static_cast<std::uint32_t>(p.is_explicit)<<14)|(static_cast<std::uint32_t>(p.is_noexcept)<<15);}
FunctionProperties function_properties(std::uint32_t f){FunctionProperties p;p.variadic=f&1;p.is_static=f&(1<<1);p.is_virtual=f&(1<<2);p.is_pure_virtual=f&(1<<3);p.is_override=f&(1<<4);p.is_final=f&(1<<5);p.is_const=f&(1<<6);p.is_volatile=f&(1<<7);p.ref_lvalue=f&(1<<8);p.ref_rvalue=f&(1<<9);p.is_constexpr=f&(1<<10);p.is_consteval=f&(1<<11);p.is_deleted=f&(1<<12);p.is_defaulted=f&(1<<13);p.is_explicit=f&(1<<14);p.is_noexcept=f&(1<<15);return p;}
std::uint32_t variable_flags(const VariableProperties& p){return static_cast<std::uint32_t>(p.is_static)|(static_cast<std::uint32_t>(p.is_thread_local)<<1)|(static_cast<std::uint32_t>(p.is_constexpr)<<2)|(static_cast<std::uint32_t>(p.is_constinit)<<3)|(static_cast<std::uint32_t>(p.is_mutable)<<4);}
VariableProperties variable_properties(std::uint32_t f){return {bool(f&1),bool(f&(1<<1)),bool(f&(1<<2)),bool(f&(1<<3)),bool(f&(1<<4))};}
std::uint32_t template_flags(const TemplateProperties& p){return static_cast<std::uint32_t>(p.is_primary)|(static_cast<std::uint32_t>(p.is_partial_specialization)<<1)|(static_cast<std::uint32_t>(p.is_explicit_specialization)<<2)|(static_cast<std::uint32_t>(p.is_instantiation)<<3)|(static_cast<std::uint32_t>(p.dependent)<<4);}
TemplateProperties template_properties(std::uint32_t f){return {bool(f&1),bool(f&(1<<1)),bool(f&(1<<2)),bool(f&(1<<3)),bool(f&(1<<4))};}

void insert_model(const Database& db,const SemanticModel& model){
    Statement metadata(db.get(),"INSERT INTO metadata VALUES(?,?)");
    auto meta=[&](std::string_view key,std::string value){metadata.text(1,key);metadata.text(2,value);metadata.run();};
    meta("schema_version",std::to_string(schema_version));meta("extraction_version",std::to_string(extraction_version));meta("canonicalization_version",std::to_string(canonicalization_version));meta("flow_analysis_version",std::to_string(flow_analysis_version));
    meta("workspace_id",model.workspace.value);meta("workspace_path",model.workspace_path);
    meta("total_tus",std::to_string(model.completeness.total_tus));meta("complete_tus",std::to_string(model.completeness.complete_tus));meta("warning_tus",std::to_string(model.completeness.warning_tus));meta("partial_tus",std::to_string(model.completeness.partial_tus));meta("failed_tus",std::to_string(model.completeness.failed_tus));
    {
        Statement s(db.get(),"INSERT INTO configurations VALUES(?,?,?)");for(const auto&[id,v]:model.configurations){s.text(1,id.value);s.text(2,v.canonical_key);s.text(3,v.fingerprint);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO files VALUES(?,?,?,?)");for(const auto&[id,v]:model.files){s.text(1,id.value);s.text(2,v.workspace.value);s.text(3,v.path);s.integer(4,v.external);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO file_versions VALUES(?,?,?,?)");for(const auto&[id,v]:model.file_versions){s.text(1,id.value);s.text(2,v.file.value);s.text(3,v.content_hash);s.integer(4,v.size);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO translation_units VALUES(?,?,?)");for(const auto&[id,v]:model.translation_units){s.text(1,id.value);s.text(2,v.source.value);s.text(3,v.configuration.value);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO revisions VALUES(?,?,?,?,?,?)");for(const auto&[id,v]:model.revisions){s.text(1,id.value);s.text(2,v.tu.value);s.text(3,v.source_hash);s.text(4,v.compilation_fingerprint);s.integer(5,v.extractor_version);s.integer(6,static_cast<int>(v.quality));s.run();}
    }{
        Statement s(db.get(),"INSERT INTO types VALUES(?,?,?,?,?,?,?,?,?,?)");for(const auto&[id,v]:model.types){s.text(1,id.value);s.text(2,v.canonical_key);s.integer(3,static_cast<int>(v.kind));s.integer(4,static_cast<int>(v.qualifiers.is_const)|(static_cast<int>(v.qualifiers.is_volatile)<<1)|(static_cast<int>(v.qualifiers.is_restrict)<<2));s.text(5,v.spelling);s.text(6,v.canonical_spelling);v.named_symbol?s.text(7,v.named_symbol->value):s.null(7);v.array_extent?s.integer(8,*v.array_extent):s.null(8);s.integer(9,v.variadic);s.integer(10,v.dependent);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO type_children VALUES(?,?,?)");for(const auto&[id,v]:model.types)for(std::size_t i=0;i<v.children.size();++i){s.text(1,id.value);s.integer(2,i);s.text(3,v.children[i].value);s.run();}
        Statement a(db.get(),"INSERT INTO type_arguments VALUES(?,?,?)");for(const auto&[id,v]:model.types)for(std::size_t i=0;i<v.template_arguments.size();++i){a.text(1,id.value);a.integer(2,i);a.text(3,v.template_arguments[i]);a.run();}
    }{
        Statement s(db.get(),"INSERT INTO logical_symbols VALUES(?,?,?,?)");for(const auto&[id,v]:model.logical_symbols){s.text(1,id.value);s.text(2,v.canonical_key);s.text(3,v.clang_usr);s.integer(4,static_cast<int>(v.linkage));s.run();}
    }{
        Statement s(db.get(),"INSERT INTO symbols VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");for(const auto&[id,v]:model.symbols){s.text(1,id.value);s.text(2,v.canonical_key);s.text(3,v.usr);v.logical_symbol?s.text(4,v.logical_symbol->value):s.null(4);s.integer(5,static_cast<int>(v.kind));s.text(6,v.name);s.text(7,v.qualified_name);s.integer(8,static_cast<int>(v.linkage));s.integer(9,static_cast<int>(v.visibility));v.semantic_parent?s.text(10,v.semantic_parent->value):s.null(10);v.type?s.text(11,v.type->value):s.null(11);s.integer(12,function_flags(v.function));s.integer(13,variable_flags(v.variable));s.integer(14,template_flags(v.templ));s.integer(15,v.exported);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO occurrences VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");for(const auto&[id,v]:model.occurrences){s.text(1,id.value);s.text(2,v.symbol.value);s.text(3,v.revision.value);bind_range(s,4,v.range);s.integer(11,static_cast<int>(v.role));s.integer(12,v.implicit);v.type?s.text(13,v.type->value):s.null(13);s.integer(14,function_flags(v.function));s.integer(15,variable_flags(v.variable));s.integer(16,template_flags(v.templ));s.integer(17,v.exported);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO relationships VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");for(const auto&[id,v]:model.relationships){s.text(1,id.value);s.integer(2,static_cast<int>(v.kind));s.text(3,v.source.value);v.target_symbol?s.text(4,v.target_symbol->value):s.null(4);v.target_type?s.text(5,v.target_type->value):s.null(5);s.text(6,v.unresolved_target);s.text(7,v.observed_target_usr);s.text(8,v.observed_target_spelling);s.text(9,v.observed_in.value);bind_range(s,10,v.evidence);s.integer(17,static_cast<int>(v.resolution));s.integer(18,static_cast<int>(v.dispatch));s.integer(19,static_cast<int>(v.origin));s.integer(20,static_cast<int>(v.target_domain));s.integer(21,static_cast<int>(v.resolution_failure));s.integer(22,static_cast<int>(v.access));s.integer(23,v.is_virtual);s.integer(24,v.is_dependent);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO macro_definitions VALUES(?,?,?,?,?,?,?,?,?,?,?)");for(const auto&[id,v]:model.macro_definitions){s.text(1,id.value);s.text(2,v.name);s.text(3,v.replacement);s.text(4,v.revision.value);bind_range(s,5,v.range);s.run();}
        Statement e(db.get(),"INSERT INTO macro_expansions VALUES(?,?,?,?,?,?,?,?,?,?,?)");for(const auto&[id,v]:model.macro_expansions){e.text(1,id.value);e.text(2,v.name);v.definition?e.text(3,v.definition->value):e.null(3);e.text(4,v.revision.value);bind_range(e,5,v.range);e.run();}
    }{
        Statement s(db.get(),"INSERT INTO includes VALUES(?,?,?,?,?,?,?,?,?,?)");for(const auto&[id,v]:model.includes){s.text(1,id.value);s.text(2,v.revision.value);s.text(3,v.including_file.value);s.text(4,v.included_file.value);s.text(5,v.directive.path);s.integer(6,v.directive.line);s.integer(7,v.directive.column);s.integer(8,v.directive.offset);s.integer(9,static_cast<int>(v.kind));s.integer(10,v.resolved);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO diagnostics VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");for(const auto&[id,v]:model.diagnostics){s.text(1,id.value);s.text(2,v.revision.value);s.integer(3,v.severity);s.text(4,v.message);s.text(5,v.option);bind_range(s,6,v.range);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO build_targets VALUES(?,?,?,?,?)");for(const auto&[id,v]:model.build_targets){s.text(1,id.value);s.text(2,v.name);s.integer(3,static_cast<int>(v.kind));s.text(4,v.source);s.integer(5,v.topology_authoritative);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO target_memberships VALUES(?,?,?)");for(const auto&[id,v]:model.target_memberships){s.text(1,id.value);s.text(2,v.target.value);s.text(3,v.tu.value);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO target_dependencies VALUES(?,?,?)");for(const auto&[id,v]:model.target_dependencies){s.text(1,id.value);s.text(2,v.consumer.value);s.text(3,v.dependency.value);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO entry_roots VALUES(?,?,?,?,?)");for(const auto&[id,v]:model.entry_roots){s.text(1,id.value);s.text(2,v.symbol.value);s.integer(3,static_cast<int>(v.kind));s.text(4,v.reason);v.configuration?s.text(5,v.configuration->value):s.null(5);s.run();}
    }{
        Statement s(db.get(),"INSERT INTO indirect_call_summaries VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        Statement values(db.get(),"INSERT INTO indirect_domain_values VALUES(?,?)");
        Statement stored(db.get(),"INSERT INTO indirect_stored_targets VALUES(?,?)");
        Statement selected(db.get(),"INSERT INTO indirect_selectable_targets VALUES(?,?)");
        Statement addresses(db.get(),"INSERT INTO indirect_modeled_addresses VALUES(?,?)");
        for(const auto&[id,v]:model.indirect_call_summaries){s.text(1,id.value);s.text(2,v.call_observation.value);s.text(3,v.revision.value);s.text(4,v.configuration.value);s.integer(5,v.analyzer_version);s.text(6,v.storage_provider.value);s.text(7,v.index_provider.value);s.integer(8,static_cast<int>(v.index_domain.kind));s.text(9,std::to_string(v.index_domain.minimum));s.text(10,std::to_string(v.index_domain.maximum));s.text(11,std::to_string(v.index_domain.known_zero_mask));s.integer(12,v.storage_escapes);s.integer(13,v.complete);s.text(14,v.reason);s.run();
            for(const auto value:v.index_domain.exact_values){values.text(1,id.value);values.text(2,std::to_string(value));values.run();}
            for(const auto& symbol:v.stored_targets){stored.text(1,id.value);stored.text(2,symbol.value);stored.run();}
            for(const auto& symbol:v.selectable_targets){selected.text(1,id.value);selected.text(2,symbol.value);selected.run();}
            for(const auto& relationship:v.modeled_address_observations){addresses.text(1,id.value);addresses.text(2,relationship.value);addresses.run();}
        }
    }{
        Statement s(db.get(),"INSERT INTO active_revisions VALUES(?,?)");for(const auto&[tu,rev]:model.active_revision_by_tu){s.text(1,tu);s.text(2,rev.value);s.run();}
    }
}

std::map<std::string,std::string> load_metadata(sqlite3* db){std::map<std::string,std::string> result;Statement s(db,"SELECT key,value FROM metadata");while(s.row())result.emplace(s.string(0),s.string(1));return result;}

void atomic_replace(const std::filesystem::path& from,const std::filesystem::path& to){
#ifdef _WIN32
    if(!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("atomic snapshot publication failed (Win32 "+std::to_string(GetLastError())+")");
#else
    std::filesystem::rename(from,to);
#endif
}

} // namespace

void SQLiteSnapshotStore::publish(const SemanticModel& model,const std::filesystem::path& path)const{
    const auto validation=SemanticModelValidator{}.validate(model);if(!validation.ok())throw std::runtime_error("refusing to publish invalid model: "+validation.issues.front().code+": "+validation.issues.front().message);
    std::filesystem::create_directories(path.parent_path().empty()?std::filesystem::current_path():path.parent_path());
    const auto suffix=sha256(path.string()+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())).substr(0,12);
    auto temporary=path;temporary+=L".tmp-"+std::wstring(suffix.begin(),suffix.end());
    std::error_code error;std::filesystem::remove(temporary,error);
    try{
        {Database db(temporary,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_EXCLUSIVE);create_schema(db);db.exec("BEGIN IMMEDIATE");
        try{insert_model(db,model);create_indexes(db);db.exec("COMMIT");}catch(...){try{db.exec("ROLLBACK");}catch(...){}throw;}}
        const auto staged_validation=validate_file(temporary);if(!staged_validation.ok())throw std::runtime_error("staged snapshot validation failed: "+staged_validation.issues.front().code+": "+staged_validation.issues.front().message);
        const auto staged=load_read_only(temporary);if(normalized_snapshot(staged)!=normalized_snapshot(model))throw std::runtime_error("staged snapshot is not semantically equivalent to the authoritative in-memory model");
        atomic_replace(temporary,path);
    }catch(...){std::filesystem::remove(temporary,error);throw;}
}

void SQLiteSnapshotStore::publish_checkpoint(const SemanticModel& model,const std::filesystem::path& path)const{
    std::filesystem::create_directories(path.parent_path().empty()?std::filesystem::current_path():path.parent_path());
    const auto suffix=sha256(path.string()+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())).substr(0,12);
    auto temporary=path;temporary+=L".tmp-"+std::wstring(suffix.begin(),suffix.end());
    std::error_code error;std::filesystem::remove(temporary,error);
    try{
        {Database db(temporary,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_EXCLUSIVE);create_schema(db);db.exec("BEGIN IMMEDIATE");
        try{insert_model(db,model);create_indexes(db);db.exec("COMMIT");}catch(...){try{db.exec("ROLLBACK");}catch(...){}throw;}
        Statement integrity(db.get(),"PRAGMA quick_check");if(!integrity.row()||integrity.string(0)!="ok")throw std::runtime_error("checkpoint SQLite quick_check failed");
        Statement foreign_keys(db.get(),"PRAGMA foreign_key_check");if(foreign_keys.row())throw std::runtime_error("checkpoint foreign-key violation in table "+foreign_keys.string(0));}
        atomic_replace(temporary,path);
    }catch(...){std::filesystem::remove(temporary,error);throw;}
}

SemanticModel SQLiteSnapshotStore::load_read_only(const std::filesystem::path& path)const{
    Database db(path,SQLITE_OPEN_READONLY);db.exec("PRAGMA query_only=ON;PRAGMA foreign_keys=ON;");const auto metadata=load_metadata(db.get());
    if(!metadata.contains("schema_version"))throw SnapshotVersionMismatch("semantic schema version is absent");const auto stored_schema=std::stoul(metadata.at("schema_version"));
    if(stored_schema!=schema_version)throw SnapshotVersionMismatch("unsupported semantic schema version");
    if(!metadata.contains("extraction_version")||!metadata.contains("canonicalization_version"))throw SnapshotVersionMismatch("semantic extractor or canonicalizer version is absent");
    if(stored_schema==schema_version&&(std::stoul(metadata.at("extraction_version"))!=extraction_version||std::stoul(metadata.at("canonicalization_version"))!=canonicalization_version))throw SnapshotVersionMismatch("unsupported semantic extractor or canonicalizer version");
    SemanticModel model;model.workspace=WorkspaceID{metadata.at("workspace_id")};model.workspace_path=metadata.at("workspace_path");
    {Statement s(db.get(),"SELECT id,canonical_key,fingerprint FROM configurations");while(s.row()){BuildConfiguration v{BuildConfigurationID{s.string(0)},s.string(1),s.string(2)};model.configurations.emplace(v.id,v);}}
    {Statement s(db.get(),"SELECT id,workspace,path,external FROM files");while(s.row()){File v{FileID{s.string(0)},WorkspaceID{s.string(1)},s.string(2),bool(s.number(3))};model.files.emplace(v.id,v);}}
    {Statement s(db.get(),"SELECT id,file_id,content_hash,size FROM file_versions");while(s.row()){FileVersion v{FileVersionID{s.string(0)},FileID{s.string(1)},s.string(2),static_cast<std::uint64_t>(s.number(3))};model.file_versions.emplace(v.id,v);}}
    {Statement s(db.get(),"SELECT id,source_file,configuration_id FROM translation_units");while(s.row()){TranslationUnit v{TranslationUnitID{s.string(0)},FileID{s.string(1)},BuildConfigurationID{s.string(2)}};model.translation_units.emplace(v.id,v);}}
    {Statement s(db.get(),"SELECT id,tu_id,source_hash,compilation_fingerprint,extractor_version,quality FROM revisions");while(s.row()){TranslationUnitRevision v{TranslationUnitRevisionID{s.string(0)},TranslationUnitID{s.string(1)},s.string(2),s.string(3),static_cast<std::uint32_t>(s.number(4)),static_cast<ExtractionQuality>(s.number(5))};model.revisions.emplace(v.id,v);}}
    {Statement s(db.get(),"SELECT id,canonical_key,kind,cv,spelling,canonical_spelling,named_symbol,array_extent,variadic,dependent FROM types");while(s.row()){const auto flags=s.number(3);Type v;v.id=TypeID{s.string(0)};v.canonical_key=s.string(1);v.kind=static_cast<TypeKind>(s.number(2));v.qualifiers={bool(flags&1),bool(flags&2),bool(flags&4)};v.spelling=s.string(4);v.canonical_spelling=s.string(5);if(!s.is_null(6))v.named_symbol=SymbolID{s.string(6)};if(!s.is_null(7))v.array_extent=static_cast<std::uint64_t>(s.number(7));v.variadic=s.number(8);v.dependent=s.number(9);model.types.emplace(v.id,std::move(v));}
        Statement c(db.get(),"SELECT parent_id,child_id FROM type_children ORDER BY parent_id,position");while(c.row())model.types.at(TypeID{c.string(0)}).children.push_back(TypeID{c.string(1)});
        Statement a(db.get(),"SELECT parent_id,value FROM type_arguments ORDER BY parent_id,position");while(a.row())model.types.at(TypeID{a.string(0)}).template_arguments.push_back(a.string(1));}
    if(stored_schema>=3){
        Statement s(db.get(),"SELECT id,canonical_key,usr,linkage FROM logical_symbols");while(s.row()){LogicalSymbol v;v.id=LogicalSymbolID{s.string(0)};v.canonical_key=s.string(1);v.clang_usr=s.string(2);v.linkage=static_cast<Linkage>(s.number(3));model.logical_symbols.emplace(v.id,std::move(v));}
        Statement symbols(db.get(),"SELECT id,canonical_key,usr,logical_id,kind,name,qualified_name,linkage,visibility,semantic_parent,type_id,function_flags,variable_flags,template_flags,exported FROM symbols");while(symbols.row()){Symbol v;v.id=SymbolID{symbols.string(0)};v.canonical_key=symbols.string(1);v.usr=symbols.string(2);if(!symbols.is_null(3))v.logical_symbol=LogicalSymbolID{symbols.string(3)};v.kind=static_cast<SymbolKind>(symbols.number(4));v.name=symbols.string(5);v.qualified_name=symbols.string(6);v.linkage=static_cast<Linkage>(symbols.number(7));v.visibility=static_cast<Visibility>(symbols.number(8));if(!symbols.is_null(9))v.semantic_parent=SymbolID{symbols.string(9)};if(!symbols.is_null(10))v.type=TypeID{symbols.string(10)};v.function=function_properties(static_cast<std::uint32_t>(symbols.number(11)));v.variable=variable_properties(static_cast<std::uint32_t>(symbols.number(12)));v.templ=template_properties(static_cast<std::uint32_t>(symbols.number(13)));v.exported=symbols.number(14);if(v.logical_symbol)model.logical_symbols.at(*v.logical_symbol).variants.insert(v.id);model.symbols.emplace(v.id,std::move(v));}
    }else{
        Statement s(db.get(),"SELECT id,canonical_key,usr,kind,name,qualified_name,linkage,semantic_parent,type_id,function_flags,variable_flags,template_flags,exported FROM symbols");while(s.row()){Symbol v;v.id=SymbolID{s.string(0)};v.canonical_key=s.string(1);v.usr=s.string(2);v.kind=static_cast<SymbolKind>(s.number(3));v.name=s.string(4);v.qualified_name=s.string(5);v.linkage=static_cast<Linkage>(s.number(6));if(!s.is_null(7))v.semantic_parent=SymbolID{s.string(7)};if(!s.is_null(8))v.type=TypeID{s.string(8)};v.function=function_properties(static_cast<std::uint32_t>(s.number(9)));v.variable=variable_properties(static_cast<std::uint32_t>(s.number(10)));v.templ=template_properties(static_cast<std::uint32_t>(s.number(11)));v.exported=s.number(12);model.symbols.emplace(v.id,std::move(v));}
        for(auto&[id,symbol]:model.symbols)if(!symbol.usr.empty()&&(symbol.linkage==Linkage::External||symbol.linkage==Linkage::UniqueExternal)){
            const auto key="logical-symbol|"+model.workspace.value+"|usr|"+symbol.usr+"|linkage|"+std::to_string(static_cast<int>(symbol.linkage));const LogicalSymbolID logical_id{sha256(key)};symbol.logical_symbol=logical_id;auto [entry,_]=model.logical_symbols.try_emplace(logical_id,LogicalSymbol{logical_id,key,symbol.usr,symbol.linkage,{}});entry->second.variants.insert(id);
        }
    }
    {Statement s(db.get(),"SELECT id,symbol_id,revision_id,file_version_id,bl,bc,bo,el,ec,eo,role,implicit,type_id,function_flags,variable_flags,template_flags,exported FROM occurrences");while(s.row()){SymbolOccurrence v{OccurrenceID{s.string(0)},SymbolID{s.string(1)},TranslationUnitRevisionID{s.string(2)},get_range(s,3),static_cast<OccurrenceRole>(s.number(10)),bool(s.number(11))};if(!s.is_null(12))v.type=TypeID{s.string(12)};v.function=function_properties(static_cast<std::uint32_t>(s.number(13)));v.variable=variable_properties(static_cast<std::uint32_t>(s.number(14)));v.templ=template_properties(static_cast<std::uint32_t>(s.number(15)));v.exported=bool(s.number(16));model.occurrences.emplace(v.id,v);}}
    if(stored_schema>=3){Statement s(db.get(),"SELECT id,kind,source_id,target_symbol,target_type,unresolved,observed_target_usr,observed_target_spelling,revision_id,file_version_id,bl,bc,bo,el,ec,eo,resolution,dispatch,origin,target_domain,resolution_failure,access,is_virtual,is_dependent FROM relationships");while(s.row()){Relationship v;v.id=RelationshipID{s.string(0)};v.kind=static_cast<RelationshipKind>(s.number(1));v.source=SymbolID{s.string(2)};if(!s.is_null(3))v.target_symbol=SymbolID{s.string(3)};if(!s.is_null(4))v.target_type=TypeID{s.string(4)};v.unresolved_target=s.string(5);v.observed_target_usr=s.string(6);v.observed_target_spelling=s.string(7);v.observed_in=TranslationUnitRevisionID{s.string(8)};v.evidence=get_range(s,9);v.resolution=static_cast<ResolutionStatus>(s.number(16));v.dispatch=static_cast<DispatchKind>(s.number(17));v.origin=static_cast<EvidenceOrigin>(s.number(18));v.target_domain=static_cast<TargetDomain>(s.number(19));v.resolution_failure=static_cast<ResolutionFailure>(s.number(20));v.access=static_cast<AccessSpecifier>(s.number(21));v.is_virtual=s.number(22);v.is_dependent=s.number(23);model.relationships.emplace(v.id,std::move(v));}}
    else{Statement s(db.get(),"SELECT id,kind,source_id,target_symbol,target_type,unresolved,revision_id,file_version_id,bl,bc,bo,el,ec,eo,resolution,dispatch,origin,access,is_virtual,is_dependent FROM relationships");while(s.row()){Relationship v;v.id=RelationshipID{s.string(0)};v.kind=static_cast<RelationshipKind>(s.number(1));v.source=SymbolID{s.string(2)};if(!s.is_null(3))v.target_symbol=SymbolID{s.string(3)};if(!s.is_null(4))v.target_type=TypeID{s.string(4)};v.unresolved_target=s.string(5);v.observed_target_spelling=v.unresolved_target;v.observed_in=TranslationUnitRevisionID{s.string(6)};v.evidence=get_range(s,7);v.resolution=static_cast<ResolutionStatus>(s.number(14));v.dispatch=static_cast<DispatchKind>(s.number(15));v.origin=static_cast<EvidenceOrigin>(s.number(16));v.target_domain=v.target_symbol?TargetDomain::Project:(v.dispatch==DispatchKind::Dynamic?TargetDomain::Indirect:TargetDomain::Unknown);v.resolution_failure=v.resolution==ResolutionStatus::Unresolved?(v.dispatch==DispatchKind::Dynamic?ResolutionFailure::IndirectFlow:ResolutionFailure::Unknown):ResolutionFailure::None;v.access=static_cast<AccessSpecifier>(s.number(17));v.is_virtual=s.number(18);v.is_dependent=s.number(19);model.relationships.emplace(v.id,std::move(v));}}
    {Statement s(db.get(),"SELECT id,name,replacement,revision_id,file_version_id,bl,bc,bo,el,ec,eo FROM macro_definitions");while(s.row()){MacroDefinition v{MacroDefinitionID{s.string(0)},s.string(1),s.string(2),TranslationUnitRevisionID{s.string(3)},get_range(s,4)};model.macro_definitions.emplace(v.id,v);}
        Statement e(db.get(),"SELECT id,name,definition_id,revision_id,file_version_id,bl,bc,bo,el,ec,eo FROM macro_expansions");while(e.row()){MacroExpansion v;v.id=MacroExpansionID{e.string(0)};v.name=e.string(1);if(!e.is_null(2))v.definition=MacroDefinitionID{e.string(2)};v.revision=TranslationUnitRevisionID{e.string(3)};v.range=get_range(e,4);model.macro_expansions.emplace(v.id,v);}}
    {Statement s(db.get(),"SELECT id,revision_id,including_version,included_file,directive_path,line,column_no,offset_no,kind,resolved FROM includes");while(s.row()){IncludeRelationship v{RelationshipID{s.string(0)},TranslationUnitRevisionID{s.string(1)},FileVersionID{s.string(2)},FileID{s.string(3)},{s.string(4),static_cast<std::uint32_t>(s.number(5)),static_cast<std::uint32_t>(s.number(6)),static_cast<std::uint32_t>(s.number(7))},static_cast<IncludeKind>(s.number(8)),bool(s.number(9))};model.includes.emplace(v.id,v);}}
    {Statement s(db.get(),"SELECT id,revision_id,severity,message,option_name,file_version_id,bl,bc,bo,el,ec,eo FROM diagnostics");while(s.row()){Diagnostic v{DiagnosticID{s.string(0)},TranslationUnitRevisionID{s.string(1)},static_cast<std::uint32_t>(s.number(2)),s.string(3),s.string(4),get_range(s,5)};model.diagnostics.emplace(v.id,v);}}
    if(stored_schema>=3){Statement s(db.get(),"SELECT id,name,kind,source,topology_authoritative FROM build_targets");while(s.row()){BuildTarget v{BuildTargetID{s.string(0)},s.string(1),static_cast<BuildTargetKind>(s.number(2)),s.string(3),bool(s.number(4))};model.build_targets.emplace(v.id,std::move(v));}}
    else{Statement s(db.get(),"SELECT id,name,kind,source FROM build_targets");while(s.row()){BuildTarget v{BuildTargetID{s.string(0)},s.string(1),static_cast<BuildTargetKind>(s.number(2)),s.string(3),false};model.build_targets.emplace(v.id,std::move(v));}}
    {Statement s(db.get(),"SELECT id,target_id,tu_id FROM target_memberships");while(s.row()){TargetMembership v{TargetMembershipID{s.string(0)},BuildTargetID{s.string(1)},TranslationUnitID{s.string(2)}};model.target_memberships.emplace(v.id,std::move(v));}}
    if(stored_schema>=3){Statement s(db.get(),"SELECT id,consumer_id,dependency_id FROM target_dependencies");while(s.row()){TargetDependency v{TargetDependencyID{s.string(0)},BuildTargetID{s.string(1)},BuildTargetID{s.string(2)}};model.target_dependencies.emplace(v.id,std::move(v));}}
    {Statement s(db.get(),"SELECT id,symbol_id,kind,reason,configuration_id FROM entry_roots");while(s.row()){EntryRoot v;v.id=EntryRootID{s.string(0)};v.symbol=SymbolID{s.string(1)};v.kind=static_cast<EntryRootKind>(s.number(2));v.reason=s.string(3);if(!s.is_null(4))v.configuration=BuildConfigurationID{s.string(4)};model.entry_roots.emplace(v.id,std::move(v));}}
    if(stored_schema>=3){
        Statement s(db.get(),"SELECT id,call_observation,revision_id,configuration_id,analyzer_version,storage_provider,index_provider,domain_kind,minimum_value,maximum_value,known_zero_mask,storage_escapes,complete,reason FROM indirect_call_summaries");while(s.row()){IndirectCallSummary v;v.id=IndirectCallSummaryID{s.string(0)};v.call_observation=RelationshipID{s.string(1)};v.revision=TranslationUnitRevisionID{s.string(2)};v.configuration=BuildConfigurationID{s.string(3)};v.analyzer_version=static_cast<std::uint32_t>(s.number(4));v.storage_provider=SymbolID{s.string(5)};v.index_provider=SymbolID{s.string(6)};v.index_domain.kind=static_cast<IntegerDomainKind>(s.number(7));v.index_domain.minimum=std::stoull(s.string(8));v.index_domain.maximum=std::stoull(s.string(9));v.index_domain.known_zero_mask=std::stoull(s.string(10));v.storage_escapes=s.number(11);v.complete=s.number(12);v.reason=s.string(13);model.indirect_call_summaries.emplace(v.id,std::move(v));}
        Statement values(db.get(),"SELECT summary_id,value FROM indirect_domain_values");while(values.row())model.indirect_call_summaries.at(IndirectCallSummaryID{values.string(0)}).index_domain.exact_values.insert(std::stoull(values.string(1)));
        Statement stored(db.get(),"SELECT summary_id,symbol_id FROM indirect_stored_targets");while(stored.row())model.indirect_call_summaries.at(IndirectCallSummaryID{stored.string(0)}).stored_targets.insert(SymbolID{stored.string(1)});
        Statement selected(db.get(),"SELECT summary_id,symbol_id FROM indirect_selectable_targets");while(selected.row())model.indirect_call_summaries.at(IndirectCallSummaryID{selected.string(0)}).selectable_targets.insert(SymbolID{selected.string(1)});
        Statement addresses(db.get(),"SELECT summary_id,relationship_id FROM indirect_modeled_addresses");while(addresses.row())model.indirect_call_summaries.at(IndirectCallSummaryID{addresses.string(0)}).modeled_address_observations.insert(RelationshipID{addresses.string(1)});
    }
    {Statement s(db.get(),"SELECT tu_id,revision_id FROM active_revisions");while(s.row())model.active_revision_by_tu.emplace(s.string(0),TranslationUnitRevisionID{s.string(1)});}
    model.completeness={std::stoull(metadata.at("total_tus")),std::stoull(metadata.at("complete_tus")),std::stoull(metadata.at("warning_tus")),std::stoull(metadata.at("partial_tus")),std::stoull(metadata.at("failed_tus"))};
    return model;
}

ValidationResult SQLiteSnapshotStore::validate_file(const std::filesystem::path& path)const{
    ValidationResult result;
    try{
        Database db(path,SQLITE_OPEN_READONLY);db.exec("PRAGMA query_only=ON;PRAGMA foreign_keys=ON;");
        Statement integrity(db.get(),"PRAGMA integrity_check");if(!integrity.row()||integrity.string(0)!="ok")result.issues.push_back({"storage.integrity","SQLite integrity_check failed"});
        Statement foreign_keys(db.get(),"PRAGMA foreign_key_check");while(foreign_keys.row())result.issues.push_back({"storage.foreign-key","foreign-key violation in table "+foreign_keys.string(0)+" row "+std::to_string(foreign_keys.number(1))});
    }catch(const std::exception& error){result.issues.push_back({"storage.open",error.what()});return result;}
    try{auto semantic=SemanticModelValidator{}.validate(load_read_only(path));result.issues.insert(result.issues.end(),semantic.issues.begin(),semantic.issues.end());}
    catch(const std::exception& error){result.issues.push_back({"storage.deserialize",error.what()});}
    return result;
}

} // namespace codeinsight
