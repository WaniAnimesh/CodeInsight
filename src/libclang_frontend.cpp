#include "codeinsight/codeinsight.hpp"

#include <clang-c/Index.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <syncstream>

namespace codeinsight {
namespace {

struct ClangString {
    CXString value;
    ~ClangString() { clang_disposeString(value); }
    [[nodiscard]] std::string str() const { const char* p=clang_getCString(value); return p ? p : ""; }
};

std::string text(CXString value) { return ClangString{value}.str(); }

bool profile_translation_units() {
#ifdef _WIN32
    char* value{};std::size_t size{};if(_dupenv_s(&value,&size,"CODEINSIGHT_PROFILE_TUS")!=0)return false;const bool enabled=value&&*value;std::free(value);return enabled;
#else
    const auto* value=std::getenv("CODEINSIGHT_PROFILE_TUS");return value&&*value;
#endif
}

std::string clang_file_path(CXFile file) {
    if(!file)return {};
    const auto raw=text(clang_getFileName(file));
    thread_local std::map<std::string,std::string> cache;
    const auto found=cache.find(raw);if(found!=cache.end())return found->second;
    return cache.emplace(raw,normalize_path(raw)).first->second;
}

SourcePoint point(CXSourceLocation location, bool expansion=false) {
    CXFile file{}; unsigned line{}, column{}, offset{};
    if (expansion) clang_getExpansionLocation(location, &file, &line, &column, &offset);
    else clang_getSpellingLocation(location, &file, &line, &column, &offset);
    return {clang_file_path(file), line, column, offset};
}

RawSourceRange range(CXSourceRange value, bool expansion=false) {
    return {point(clang_getRangeStart(value), expansion), point(clang_getRangeEnd(value), expansion)};
}

Linkage linkage(CXLinkageKind kind) {
    switch (kind) {
    case CXLinkage_NoLinkage: return Linkage::None; case CXLinkage_Internal: return Linkage::Internal;
    case CXLinkage_UniqueExternal: return Linkage::UniqueExternal; case CXLinkage_External: return Linkage::External;
    default: return Linkage::Invalid;
    }
}

Visibility visibility(CXVisibilityKind kind) {
    switch (kind) { case CXVisibility_Hidden: return Visibility::Hidden; case CXVisibility_Protected: return Visibility::Protected; case CXVisibility_Default: return Visibility::Default; default: return Visibility::Invalid; }
}

AccessSpecifier access(CX_CXXAccessSpecifier kind) {
    switch (kind) { case CX_CXXPublic: return AccessSpecifier::Public; case CX_CXXProtected: return AccessSpecifier::Protected; case CX_CXXPrivate: return AccessSpecifier::Private; default: return AccessSpecifier::Invalid; }
}

SymbolKind symbol_kind(CXCursor cursor) {
    const auto kind = clang_getCursorKind(cursor);
    switch (kind) {
    case CXCursor_Namespace: return SymbolKind::Namespace;
    case CXCursor_NamespaceAlias: return SymbolKind::NamespaceAlias;
    case CXCursor_ClassDecl: return SymbolKind::Class;
    case CXCursor_StructDecl: return SymbolKind::Struct;
    case CXCursor_UnionDecl: return SymbolKind::Union;
    case CXCursor_FunctionDecl: {
        const auto name=text(clang_getCursorSpelling(cursor)); return name.starts_with("operator") ? SymbolKind::OperatorFunction : SymbolKind::Function;
    }
    case CXCursor_CXXMethod: {
        const auto name=text(clang_getCursorSpelling(cursor)); return name.starts_with("operator") ? SymbolKind::OperatorFunction : SymbolKind::Method;
    }
    case CXCursor_Constructor: return SymbolKind::Constructor;
    case CXCursor_Destructor: return SymbolKind::Destructor;
    case CXCursor_ConversionFunction: return SymbolKind::ConversionFunction;
    case CXCursor_VarDecl: {
        const auto parent=clang_getCursorSemanticParent(cursor);
        return clang_isDeclaration(clang_getCursorKind(parent)) &&
               (clang_getCursorKind(parent)==CXCursor_FunctionDecl || clang_getCursorKind(parent)==CXCursor_CXXMethod || clang_getCursorKind(parent)==CXCursor_Constructor || clang_getCursorKind(parent)==CXCursor_Destructor || clang_getCursorKind(parent)==CXCursor_ConversionFunction || clang_getCursorKind(parent)==CXCursor_FunctionTemplate)
            ? SymbolKind::LocalVariable : SymbolKind::Variable;
    }
    case CXCursor_FieldDecl: return SymbolKind::Field;
    case CXCursor_ParmDecl: return SymbolKind::Parameter;
    case CXCursor_EnumDecl: return SymbolKind::Enum;
    case CXCursor_EnumConstantDecl: return SymbolKind::Enumerator;
    case CXCursor_TypedefDecl: return SymbolKind::Typedef;
    case CXCursor_TypeAliasDecl: return SymbolKind::TypeAlias;
    case CXCursor_ClassTemplate: case CXCursor_ClassTemplatePartialSpecialization: return SymbolKind::ClassTemplate;
    case CXCursor_FunctionTemplate: return SymbolKind::FunctionTemplate;
    case CXCursor_TypeAliasTemplateDecl: return SymbolKind::AliasTemplate;
    case CXCursor_TemplateTypeParameter: return SymbolKind::TemplateTypeParameter;
    case CXCursor_NonTypeTemplateParameter: return SymbolKind::TemplateNonTypeParameter;
    case CXCursor_TemplateTemplateParameter: return SymbolKind::TemplateTemplateParameter;
    case CXCursor_UsingDeclaration: return SymbolKind::UsingDeclaration;
    case CXCursor_UsingDirective: return SymbolKind::UsingDirective;
    case CXCursor_LambdaExpr: return SymbolKind::Lambda;
    default: return SymbolKind::Unknown;
    }
}

bool callable(SymbolKind kind) {
    return kind==SymbolKind::Function || kind==SymbolKind::Method || kind==SymbolKind::Constructor ||
           kind==SymbolKind::Destructor || kind==SymbolKind::ConversionFunction ||
           kind==SymbolKind::OperatorFunction || kind==SymbolKind::FunctionTemplate;
}

bool scope(SymbolKind kind) {
    return callable(kind)||kind==SymbolKind::Namespace||kind==SymbolKind::Class||kind==SymbolKind::Struct||
        kind==SymbolKind::Union||kind==SymbolKind::Enum||kind==SymbolKind::ClassTemplate;
}

std::string qualified_name(CXCursor cursor) {
    std::vector<std::string> names;
    for (auto current=cursor; !clang_Cursor_isNull(current); current=clang_getCursorSemanticParent(current)) {
        const auto kind=clang_getCursorKind(current);
        if (kind==CXCursor_TranslationUnit) break;
        auto name=text(clang_getCursorSpelling(current));
        if (name.empty() && kind==CXCursor_Namespace) name="(anonymous namespace)";
        if (name.empty() && kind==CXCursor_LambdaExpr) name="(lambda@" + std::to_string(point(clang_getCursorLocation(current)).offset) + ")";
        if (!name.empty()) names.push_back(std::move(name));
    }
    std::reverse(names.begin(), names.end());
    std::string result;
    for (const auto& name : names) { if(!result.empty()) result += "::"; result += name; }
    return result;
}

std::string cursor_key(CXCursor cursor) {
    auto usr=text(clang_getCursorUSR(cursor)); if(!usr.empty()) return "usr:"+usr;
    const auto location=point(clang_getCursorLocation(cursor));
    std::string parent_context;const auto parent=clang_getCursorSemanticParent(cursor);
    if(!clang_Cursor_isNull(parent)&&clang_getCursorKind(parent)!=CXCursor_TranslationUnit){
        const auto parent_usr=text(clang_getCursorUSR(parent));
        if(!parent_usr.empty())parent_context="|parent-usr:"+parent_usr;
        else{const auto parent_location=point(clang_getCursorLocation(parent));parent_context="|parent-cursor:"+std::to_string(clang_getCursorKind(parent))+"|"+qualified_name(parent)+"|"+parent_location.path+":"+std::to_string(parent_location.offset)+"|type:"+text(clang_getTypeSpelling(clang_getCursorType(parent)));}
    }
    return "cursor:" + std::to_string(clang_getCursorKind(cursor)) + "|" + qualified_name(cursor) + "|" + location.path + ":" + std::to_string(location.offset) + parent_context;
}

TypeKind type_kind(CXType type) {
    switch (type.kind) {
    case CXType_Invalid: return TypeKind::Invalid;
    case CXType_Pointer: return TypeKind::Pointer;
    case CXType_LValueReference: return TypeKind::LValueReference;
    case CXType_RValueReference: return TypeKind::RValueReference;
    case CXType_ConstantArray: case CXType_IncompleteArray: case CXType_VariableArray: case CXType_DependentSizedArray: return TypeKind::Array;
    case CXType_FunctionNoProto: case CXType_FunctionProto: return TypeKind::Function;
    case CXType_MemberPointer: return TypeKind::MemberPointer;
    case CXType_Auto: return TypeKind::Auto;
    case CXType_Elaborated: case CXType_Record: case CXType_Enum: case CXType_Typedef: return TypeKind::Named;
    case CXType_Unexposed: {
        const auto spelling=text(clang_getTypeSpelling(type));
        if (spelling.starts_with("decltype")) return TypeKind::Decltype;
        if (spelling.find('<') != std::string::npos) return TypeKind::TemplateSpecialization;
        return TypeKind::Dependent;
    }
    case CXType_Dependent: return TypeKind::Dependent;
    default:
        if (type.kind >= CXType_FirstBuiltin && type.kind <= CXType_LastBuiltin) return TypeKind::Builtin;
        return TypeKind::Unknown;
    }
}

struct IndexedVariable {
    std::string usr;
    CXCursor primary{};
    CXIdxEntityCXXTemplateKind template_kind{CXIdxEntity_NonTemplate};
    bool instantiated{};
};

struct Context {
    FactBatch* batch{};
    CXTranslationUnit tu{};
    std::map<std::string, std::size_t> type_index;
    std::set<std::string> emitted_symbols;
    std::set<std::string> known_symbols;
    std::map<std::string, IndexedVariable> indexed_variables;
    std::string source_key;
    std::vector<std::string> project_roots;
};

bool project_owned(const Context& context,std::string_view path){
    if(path.empty())return false;
    if(path==context.source_key)return true;
    return std::any_of(context.project_roots.begin(),context.project_roots.end(),[&](const auto& root){return path==root||(path.size()>root.size()&&path.starts_with(root)&&path[root.size()]=='/');});
}

bool project_owned(const Context& context,CXCursor cursor){
    const auto location=clang_getCursorLocation(cursor);
    return project_owned(context,point(location).path)||project_owned(context,point(location,true).path);
}

bool project_spelling_owned(const Context& context,CXCursor cursor){return project_owned(context,point(clang_getCursorLocation(cursor)).path);}
bool project_expansion_owned(const Context& context,CXCursor cursor){return project_owned(context,point(clang_getCursorLocation(cursor),true).path);}

// The indexing API exposes variable templates that the cursor API reports
// as UnexposedDecl, including the authoritative primary-template USR.
void index_variable(Context& context, const CXIdxEntityInfo* entity) {
    if (!entity || !entity->USR ||
        (entity->kind != CXIdxEntity_Variable && entity->kind != CXIdxEntity_CXXStaticVariable) ||
        entity->templateKind == CXIdxEntity_NonTemplate || !project_owned(context, entity->cursor)) return;
    context.indexed_variables[cursor_key(entity->cursor)] =
        {entity->USR, entity->cursor, entity->templateKind, false};
}

void index_variable_declaration(CXClientData data, const CXIdxDeclInfo* declaration) {
    if (declaration) index_variable(*static_cast<Context*>(data), declaration->entityInfo);
}

void index_variable_reference(CXClientData data, const CXIdxEntityRefInfo* reference) {
    if (!reference) return;
    auto& context = *static_cast<Context*>(data);
    const auto* entity = reference->referencedEntity;
    index_variable(context, entity);
    if (!entity || !context.indexed_variables.contains(cursor_key(entity->cursor))) return;
    const auto target = clang_getCursorReferenced(reference->cursor);
    if (clang_Cursor_isNull(target) || !project_owned(context, target) ||
        clang_getCursorKind(target) != CXCursor_UnexposedDecl) return;
    const auto key = cursor_key(target);
    if (key == cursor_key(entity->cursor)) return;
    const auto usr = text(clang_getCursorUSR(target));
    if (!usr.empty()) context.indexed_variables[key] = {usr, entity->cursor, entity->templateKind, true};
}

std::string add_type(Context& context, CXType cx_type) {
    if (cx_type.kind == CXType_Invalid) return {};
    TypeFact fact;
    fact.kind=type_kind(cx_type);
    fact.qualifiers={clang_isConstQualifiedType(cx_type)!=0,clang_isVolatileQualifiedType(cx_type)!=0,clang_isRestrictQualifiedType(cx_type)!=0};
    fact.spelling=text(clang_getTypeSpelling(cx_type));
    const auto canonical=clang_getCanonicalType(cx_type);
    fact.canonical_spelling=text(clang_getTypeSpelling(canonical));
    const auto declaration=clang_getTypeDeclaration(cx_type);
    fact.named_symbol_usr=text(clang_getCursorUSR(declaration));
    fact.dependent=fact.kind==TypeKind::Dependent;

    auto child = [&](CXType value) { auto key=add_type(context,value); if(!key.empty()) fact.children.push_back(std::move(key)); };
    switch (fact.kind) {
    case TypeKind::Pointer: case TypeKind::LValueReference: case TypeKind::RValueReference: child(clang_getPointeeType(cx_type)); break;
    case TypeKind::Array: {
        child(clang_getArrayElementType(cx_type)); const auto count=clang_getArraySize(cx_type); if(count>=0) fact.array_extent=static_cast<std::uint64_t>(count); break;
    }
    case TypeKind::Function: {
        child(clang_getResultType(cx_type)); const int count=clang_getNumArgTypes(cx_type); for(int i=0;i<count;++i) child(clang_getArgType(cx_type,i)); fact.variadic=clang_isFunctionTypeVariadic(cx_type)!=0; break;
    }
    case TypeKind::MemberPointer:
        // clang_getPointeeType is documented for pointer types, not C++ member
        // pointers, and Clang 22 terminates while decomposing dependent forms
        // such as `E T::*`. The canonical spelling already carries both the
        // class and member components, so retain it as an atomic structural
        // type instead of invoking an unsafe C API combination.
        break;
    default: break;
    }
    const int template_count=clang_Type_getNumTemplateArguments(cx_type);
    for(int i=0;i<template_count;++i) fact.template_arguments.push_back(text(clang_getTypeSpelling(clang_Type_getTemplateArgumentAsType(cx_type,i))));
    std::string identity=std::to_string(static_cast<int>(fact.kind))+"|"+fact.canonical_spelling+"|"+
        (fact.qualifiers.is_const?"c":"")+(fact.qualifiers.is_volatile?"v":"")+(fact.qualifiers.is_restrict?"r":"");
    for(const auto& item:fact.children) identity += "|"+item;
    for(const auto& item:fact.template_arguments) identity += "<"+item+">";
    if(fact.array_extent) identity += "["+std::to_string(*fact.array_extent)+"]";
    fact.local_identity=sha256("type|"+identity);
    if (!context.type_index.contains(fact.local_identity)) {
        context.type_index.emplace(fact.local_identity,context.batch->types.size());
        context.batch->types.push_back(std::move(fact));
    }
    return sha256("type|"+identity);
}

std::string token_text(Context& context, CXCursor cursor) {
    CXToken* tokens{}; unsigned count{}; clang_tokenize(context.tu,clang_getCursorExtent(cursor),&tokens,&count);
    std::string result;
    for(unsigned i=0;i<count;++i) { if(i) result+=' '; result += text(clang_getTokenSpelling(context.tu,tokens[i])); }
    if(tokens) clang_disposeTokens(context.tu,tokens,count);
    return result;
}

std::string declaration_token_text(Context& context,CXCursor cursor) {
    auto extent=clang_getCursorExtent(cursor);const auto start=clang_getRangeStart(extent),finish=clang_getRangeEnd(extent);
    CXFile start_file{},finish_file{};unsigned start_offset{},finish_offset{},ignored_line{},ignored_column{};
    clang_getSpellingLocation(start,&start_file,&ignored_line,&ignored_column,&start_offset);
    clang_getSpellingLocation(finish,&finish_file,&ignored_line,&ignored_column,&finish_offset);
    if(start_file&&finish_file&&clang_File_isEqual(start_file,finish_file)&&finish_offset>start_offset){
        // Function bodies are immediate CompoundStmt/CXXTryStmt children. Clip
        // the token range at that boundary so properties such as constexpr,
        // dllexport, default/delete, and explicit are read from the declaration
        // without retokenizing the complete implementation body.
        struct BodyBoundary{CXSourceLocation location{};bool found{};} boundary;
        clang_visitChildren(cursor,[](CXCursor child,CXCursor,CXClientData data){
            const auto kind=clang_getCursorKind(child);if(kind==CXCursor_CompoundStmt||kind==CXCursor_CXXTryStmt){auto* result=static_cast<BodyBoundary*>(data);result->location=clang_getCursorLocation(child);result->found=true;return CXChildVisit_Break;}return CXChildVisit_Continue;
        },&boundary);
        unsigned boundary_offset{};CXFile boundary_file{};
        if(boundary.found)clang_getSpellingLocation(boundary.location,&boundary_file,&ignored_line,&ignored_column,&boundary_offset);
        if(boundary.found&&boundary_file&&clang_File_isEqual(start_file,boundary_file)&&boundary_offset>start_offset&&boundary_offset<finish_offset){
            extent=clang_getRange(start,clang_getLocationForOffset(context.tu,start_file,boundary_offset));
        }else{
            // Records, namespaces, enums, and aggregate declarations do not
            // expose a CompoundStmt. Their opening brace terminates the
            // declaration prefix. Use libclang's immutable file buffer to find
            // that boundary without tokenizing the full nested scope.
            std::size_t content_size{};const auto* content=clang_getFileContents(context.tu,start_file,&content_size);
            if(content&&start_offset<content_size){
                const auto safe_finish=std::min<std::size_t>(finish_offset,content_size);const std::string_view source(content+start_offset,safe_finish-start_offset);
                const auto brace=source.find('{');if(brace!=std::string_view::npos)extent=clang_getRange(start,clang_getLocationForOffset(context.tu,start_file,static_cast<unsigned>(start_offset+brace+1)));
            }
        }
    }
    CXToken* tokens{};unsigned count{};clang_tokenize(context.tu,extent,&tokens,&count);std::string result;
    for(unsigned i=0;i<count;++i){if(i)result+=' ';result+=text(clang_getTokenSpelling(context.tu,tokens[i]));}
    if(tokens)clang_disposeTokens(context.tu,tokens,count);return result;
}

bool is_explicit_specialization(Context& context, CXCursor cursor) {
    // libclang exposes the specialized primary but does not provide a C API
    // discriminator for explicit specializations.  The empty template-head is
    // the language-level distinction from an implicit instantiation.
    return declaration_token_text(context,cursor).find("template < >")!=std::string::npos;
}

RelationshipFact* add_relationship(Context& context, RelationshipKind kind, const std::string& source, CXCursor target,
                                   CXCursor evidence_cursor, DispatchKind dispatch=DispatchKind::None,
                                   ResolutionStatus resolution=ResolutionStatus::Exact) {
    if(source.empty()) return nullptr;
    RelationshipFact fact; fact.kind=kind; fact.source_local_key=source; fact.evidence=range(clang_getCursorExtent(evidence_cursor),true);
    fact.dispatch=dispatch; fact.resolution=resolution; fact.origin=EvidenceOrigin::CompilerAST;
    if(!clang_Cursor_isNull(target)) {
        fact.target_usr=text(clang_getCursorUSR(target));fact.target_local_key=cursor_key(target);
        if (const auto found=context.indexed_variables.find(fact.target_local_key); found!=context.indexed_variables.end())
            fact.target_usr=found->second.usr;
        fact.observed_target_spelling=text(clang_getCursorSpelling(target));
        fact.target_domain=project_owned(context,target)?TargetDomain::Project:TargetDomain::External;
    }
    if(fact.target_usr.empty() && fact.target_local_key.empty()) {
        fact.unresolved_target=text(clang_getCursorSpelling(evidence_cursor));
        if(fact.unresolved_target.empty()){const auto tokens=token_text(context,evidence_cursor);const auto separator=tokens.find_first_of(" (\t\r\n");fact.unresolved_target=tokens.substr(0,separator);}
        fact.observed_target_spelling=fact.unresolved_target;
        fact.resolution=ResolutionStatus::Unresolved;
        fact.target_domain=dispatch==DispatchKind::Dynamic?TargetDomain::Indirect:TargetDomain::Unknown;
        fact.resolution_failure=dispatch==DispatchKind::Dynamic?ResolutionFailure::IndirectFlow:ResolutionFailure::Unknown;
    }
    context.batch->relationships.push_back(std::move(fact));
    return &context.batch->relationships.back();
}

void visit_cursor(Context& context, CXCursor cursor, CXCursor parent, std::string active_source);

CXChildVisitResult child_visitor(CXCursor cursor, CXCursor parent, CXClientData data) {
    auto* pair=static_cast<std::pair<Context*,std::string>*>(data);
    visit_cursor(*pair->first,cursor,parent,pair->second);
    return CXChildVisit_Continue;
}

bool variable_reference(CXCursor cursor) {
    switch(clang_getCursorKind(cursor)) {
    case CXCursor_VarDecl: case CXCursor_FieldDecl: case CXCursor_ParmDecl: case CXCursor_EnumConstantDecl: return true;
    default: return false;
    }
}

bool callable_reference(CXCursor cursor) {
    switch(clang_getCursorKind(cursor)) {
    case CXCursor_FunctionDecl: case CXCursor_CXXMethod: case CXCursor_Constructor: case CXCursor_Destructor:
    case CXCursor_ConversionFunction: case CXCursor_FunctionTemplate: return true;
    default: return false;
    }
}

enum class ReferenceAccess { Read, Write, ReadWrite, Address };

ReferenceAccess reference_access(Context& context,CXCursor cursor,CXCursor parent) {
    const auto parent_kind=clang_getCursorKind(parent);
    if(parent_kind==CXCursor_CompoundAssignOperator)return ReferenceAccess::ReadWrite;
    if(parent_kind==CXCursor_UnaryOperator) {
        const auto tokens=token_text(context,parent);
        if(tokens.find("++")!=std::string::npos||tokens.find("--")!=std::string::npos)return ReferenceAccess::ReadWrite;
        if(tokens.find('&')!=std::string::npos)return ReferenceAccess::Address;
    }
    if(parent_kind==CXCursor_BinaryOperator) {
        CXToken* tokens{};unsigned count{};clang_tokenize(context.tu,clang_getCursorExtent(parent),&tokens,&count);
        const auto reference_end=point(clang_getRangeEnd(clang_getCursorExtent(cursor)),true).offset;
        for(unsigned i=0;i<count;++i) {
            const auto spelling=text(clang_getTokenSpelling(context.tu,tokens[i]));
            const bool assignment=spelling=="="||spelling=="+="||spelling=="-="||spelling=="*="||spelling=="/="||spelling=="%="||spelling=="&="||spelling=="|="||spelling=="^="||spelling=="<<="||spelling==">>=";
            if(!assignment)continue;
            const auto operator_offset=point(clang_getTokenLocation(context.tu,tokens[i]),true).offset;
            if(tokens)clang_disposeTokens(context.tu,tokens,count);
            if(reference_end<=operator_offset)return spelling=="="?ReferenceAccess::Write:ReferenceAccess::ReadWrite;
            return ReferenceAccess::Read;
        }
        if(tokens)clang_disposeTokens(context.tu,tokens,count);
    }
    return ReferenceAccess::Read;
}

void add_type_relationship(Context& context,RelationshipKind kind,const std::string& source,CXType type,CXCursor evidence) {
    if(source.empty()||type.kind==CXType_Invalid)return;
    const auto key=add_type(context,type);if(key.empty())return;
    RelationshipFact fact;fact.kind=kind;fact.source_local_key=source;fact.target_type_key=key;
    fact.evidence=range(clang_getCursorExtent(evidence),true);fact.resolution=ResolutionStatus::Exact;fact.origin=EvidenceOrigin::CompilerAST;
    context.batch->relationships.push_back(std::move(fact));
}

CXCursor first_child(CXCursor cursor){
    CXCursor result=clang_getNullCursor();clang_visitChildren(cursor,[](CXCursor child,CXCursor,CXClientData data){*static_cast<CXCursor*>(data)=child;return CXChildVisit_Break;},&result);return result;
}

CXCursor first_referenced_descendant(CXCursor cursor){
    CXCursor result=clang_getNullCursor();
    clang_visitChildren(cursor,[](CXCursor child,CXCursor,CXClientData data){
        auto* result=static_cast<CXCursor*>(data);const auto referenced=clang_getCursorReferenced(child);
        if(!clang_Cursor_isNull(referenced)&&(callable_reference(referenced)||variable_reference(referenced))){*result=referenced;return CXChildVisit_Break;}
        CXCursor nested=first_referenced_descendant(child);if(!clang_Cursor_isNull(nested)){*result=nested;return CXChildVisit_Break;}
        return CXChildVisit_Continue;
    },&result);
    return result;
}

bool function_pointer(CXType type) {
    type = clang_getCanonicalType(type);
    if (type.kind != CXType_Pointer) return false;
    const auto pointee = clang_getPointeeType(type);
    return pointee.kind == CXType_FunctionProto || pointee.kind == CXType_FunctionNoProto;
}

bool local_callable_storage(CXCursor cursor) {
    return symbol_kind(cursor) == SymbolKind::LocalVariable && function_pointer(clang_getCursorType(cursor));
}

std::vector<CXCursor> children(CXCursor cursor) {
    std::vector<CXCursor> result;
    clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
        static_cast<std::vector<CXCursor>*>(data)->push_back(child);
        return CXChildVisit_Continue;
    }, &result);
    return result;
}

// Only transparent value expressions are unwrapped. In particular, a factory
// call is not evidence that the factory itself is the returned callback.
CXCursor callable_value_reference(Context& context, CXCursor expression) {
    const auto kind = clang_getCursorKind(expression);
    if (kind == CXCursor_DeclRefExpr) return clang_getCursorReferenced(expression);
    if (kind == CXCursor_ParenExpr || kind == CXCursor_UnexposedExpr)
        return callable_value_reference(context, first_child(expression));
    if (kind == CXCursor_UnaryOperator) {
        const auto tokens = token_text(context, expression);
        if (!tokens.empty() && (tokens.front() == '&' || tokens.front() == '*'))
            return callable_value_reference(context, first_child(expression));
    }
    return clang_getNullCursor();
}

void observe_callable_value(Context& context, CXCursor storage, CXCursor value,
                            const std::string& owner, CXCursor evidence) {
    if (clang_Cursor_isNull(value) || !local_callable_storage(storage)) return;
    const auto kind = clang_getCursorKind(value);
    if (kind == CXCursor_ConditionalOperator) {
        const auto operands = children(value);
        if (operands.size() == 3) {
            observe_callable_value(context, storage, operands[1], owner, evidence);
            observe_callable_value(context, storage, operands[2], owner, evidence);
        }
        return;
    }
    if (kind == CXCursor_ParenExpr || kind == CXCursor_UnexposedExpr) {
        observe_callable_value(context, storage, first_child(value), owner, evidence);
        return;
    }
    const auto target = callable_value_reference(context, value);
    if (!callable_reference(target) && !local_callable_storage(target)) return;
    add_relationship(context, RelationshipKind::CallableValueFlow, cursor_key(storage), target, evidence);
    // Function-to-pointer decay is address flow too, even without an '&' token.
    // Explicit address-of is already recorded by the normal reference visitor.
    if (callable_reference(target) &&
        !(kind == CXCursor_UnaryOperator && token_text(context, value).starts_with("&")))
        add_relationship(context, RelationshipKind::TakesAddress, owner, target, value);
}

void ensure_implicit_symbol(Context& context, CXCursor cursor) {
    if (clang_Cursor_isNull(cursor) || !project_owned(context, cursor)) return;
    const auto kind = symbol_kind(cursor);
    if (!callable(kind) && kind!=SymbolKind::Namespace && kind!=SymbolKind::Class &&
        kind!=SymbolKind::Struct && kind!=SymbolKind::Union && kind!=SymbolKind::ClassTemplate) return;
    const auto key=cursor_key(cursor);
    if (!context.known_symbols.insert(key).second) return;
    const auto parent=clang_getCursorSemanticParent(cursor);
    ensure_implicit_symbol(context, parent);
    SymbolFact fact;fact.local_identity=key;fact.kind=kind;fact.clang_usr=text(clang_getCursorUSR(cursor));fact.name=text(clang_getCursorSpelling(cursor));fact.qualified_name=qualified_name(cursor);
    if(!clang_Cursor_isNull(parent)&&clang_getCursorKind(parent)!=CXCursor_TranslationUnit&&symbol_kind(parent)!=SymbolKind::Unknown)fact.semantic_parent=cursor_key(parent);
    fact.spelling_range=range(clang_getCursorExtent(cursor));fact.expansion_range=range(clang_getCursorExtent(cursor),true);fact.linkage=linkage(clang_getCursorLinkage(cursor));fact.visibility=visibility(clang_getCursorVisibility(cursor));fact.access=access(clang_getCXXAccessSpecifier(cursor));
    fact.is_declaration=true;fact.is_definition=clang_isCursorDefinition(cursor)!=0;fact.is_implicit=true;fact.type_key=add_type(context,clang_getCursorType(cursor));
    if (callable(kind)) {
        fact.result_type_key=add_type(context,clang_getCursorResultType(cursor));
        const int count=clang_Cursor_getNumArguments(cursor);for(int i=0;i<count;++i)fact.parameter_type_keys.push_back(add_type(context,clang_getCursorType(clang_Cursor_getArgument(cursor,i))));
        fact.function.variadic=clang_Cursor_isVariadic(cursor)!=0;fact.function.is_virtual=clang_CXXMethod_isVirtual(cursor)!=0;fact.function.is_pure_virtual=clang_CXXMethod_isPureVirtual(cursor)!=0;fact.function.is_const=clang_CXXMethod_isConst(cursor)!=0;fact.function.is_defaulted=clang_CXXMethod_isDefaulted(cursor)!=0;fact.function.is_deleted=clang_CXXMethod_isDeleted(cursor)!=0;
        fact.function.is_static=clang_getCursorKind(cursor)==CXCursor_CXXMethod&&clang_CXXMethod_isStatic(cursor)!=0;
    }
    const auto primary=clang_getSpecializedCursorTemplate(cursor);
    if (!clang_Cursor_isNull(primary)) {
        ensure_implicit_symbol(context, primary);
        const bool specialization=is_explicit_specialization(context,cursor);
        fact.templ.is_instantiation=!specialization;fact.templ.is_explicit_specialization=specialization;
        add_relationship(context,specialization?RelationshipKind::Specializes:RelationshipKind::Instantiates,key,primary,cursor);
    }
    context.batch->symbols.push_back(std::move(fact));
    const auto cursor_kind=clang_getCursorKind(cursor);
    if (cursor_kind==CXCursor_CXXMethod || cursor_kind==CXCursor_Destructor || cursor_kind==CXCursor_ConversionFunction) {
        CXCursor* overridden{}; unsigned count{};
        clang_getOverriddenCursors(cursor,&overridden,&count);
        for (unsigned i=0;i<count;++i) {
            ensure_implicit_symbol(context,overridden[i]);
            add_relationship(context,RelationshipKind::Overrides,key,overridden[i],cursor);
        }
        if (overridden) clang_disposeOverriddenCursors(overridden);
    }
}

// A lexical may-destroy dependency, not a claim about a particular exit path.
// Pointers, references and static/thread storage are not automatic objects.
void observe_automatic_destruction(Context& context, CXCursor variable, const std::string& owner) {
    if (owner.empty() || symbol_kind(variable)!=SymbolKind::LocalVariable ||
        clang_Cursor_getStorageClass(variable)==CX_SC_Static ||
        clang_Cursor_getStorageClass(variable)==CX_SC_Extern ||
        clang_getCursorTLSKind(variable)!=CXTLS_None) return;
    auto type=clang_getCanonicalType(clang_getCursorType(variable));
    while (type.kind==CXType_ConstantArray || type.kind==CXType_IncompleteArray || type.kind==CXType_VariableArray)
        type=clang_getCanonicalType(clang_getArrayElementType(type));
    if (type.kind!=CXType_Record || clang_isPODType(type)) return;
    auto record=clang_getTypeDeclaration(type);
    const auto definition=clang_getCursorDefinition(record);
    if (!clang_Cursor_isNull(definition)) record=definition;
    CXCursor destructor=clang_getNullCursor();
    for (const auto child:children(record)) if (clang_getCursorKind(child)==CXCursor_Destructor) {destructor=child;break;}
    const auto target=clang_Cursor_isNull(destructor)?record:destructor;
    ensure_implicit_symbol(context,target);
    if(auto* edge=add_relationship(context,RelationshipKind::Destroys,owner,target,variable,DispatchKind::Static,ResolutionStatus::Conservative))
        edge->origin=EvidenceOrigin::DerivedCanonicalization;
}

void visit_cursor(Context& context, CXCursor cursor, CXCursor parent, std::string active_source) {
    const auto cursor_kind=clang_getCursorKind(cursor);
    // libclang represents a C++ alias template as a TypeAliasTemplateDecl
    // containing a TypeAliasDecl payload. Both cursors can carry the same USR,
    // but they are two views of one source declaration, not conflicting raw
    // symbols. Emit the source-level template and still traverse the payload's
    // children for reference evidence.
    const bool alias_template_payload=cursor_kind==CXCursor_TypeAliasDecl&&clang_getCursorKind(parent)==CXCursor_TypeAliasTemplateDecl&&text(clang_getCursorUSR(cursor))==text(clang_getCursorUSR(parent));
    // External declarations can contain enormous SDK/STL subtrees. Their
    // symbols are intentionally outside the authoritative project model, so
    // do not recurse through those subtrees. Project macro expansions and
    // project-owned instantiations remain visible because ownership considers
    // both spelling and expansion locations.
    if(cursor_kind!=CXCursor_TranslationUnit&&clang_isDeclaration(cursor_kind)&&!project_owned(context,cursor))return;
    // DetailedPreprocessingRecord also exposes every SDK/STL macro as a
    // translation-unit child. External preprocessing cursors cannot
    // contribute project facts and usually have no useful descendants, so
    // prune them at the same ownership boundary as external declarations.
    if(cursor_kind==CXCursor_MacroDefinition&&!project_spelling_owned(context,cursor))return;
    if(cursor_kind==CXCursor_MacroExpansion&&!project_expansion_owned(context,cursor))return;
    if(cursor_kind==CXCursor_InclusionDirective)return; // clang_getInclusions supplies canonical include facts.
    if(cursor_kind!=CXCursor_TranslationUnit&&clang_isPreprocessing(cursor_kind)&&!project_owned(context,cursor))return;
    const auto indexed=context.indexed_variables.find(cursor_key(cursor));
    const auto kind=indexed==context.indexed_variables.end()?symbol_kind(cursor):
        (indexed->second.instantiated?SymbolKind::Variable:SymbolKind::VariableTemplate);
    std::string own_key;

    if (cursor_kind==CXCursor_MacroDefinition && project_spelling_owned(context,cursor)) {
        auto full=token_text(context,cursor); auto name=text(clang_getCursorSpelling(cursor));
        auto replacement=full.size()>name.size() ? full.substr(name.size()) : std::string{};
        context.batch->macro_definitions.push_back({name,replacement,range(clang_getCursorExtent(cursor),true)});
    } else if (cursor_kind==CXCursor_MacroExpansion && project_expansion_owned(context,cursor)) {
        context.batch->macro_expansions.push_back({text(clang_getCursorSpelling(cursor)),cursor_key(clang_getCursorReferenced(cursor)),range(clang_getCursorExtent(cursor),true)});
    } else if (kind!=SymbolKind::Unknown && clang_isDeclaration(cursor_kind) && project_owned(context,cursor) && !alias_template_payload) {
        own_key=cursor_key(cursor);
        context.known_symbols.insert(own_key);
        const auto observation_key=own_key+"|"+std::to_string(point(clang_getCursorLocation(cursor),true).offset)+"|"+std::to_string(clang_isCursorDefinition(cursor));
        if(context.emitted_symbols.insert(observation_key).second) {
            SymbolFact fact; fact.local_identity=own_key; fact.kind=kind; fact.clang_usr=text(clang_getCursorUSR(cursor));
            fact.name=text(clang_getCursorSpelling(cursor)); fact.qualified_name=qualified_name(cursor);
            if (indexed!=context.indexed_variables.end()) fact.clang_usr=indexed->second.usr;
            const auto semantic=clang_getCursorSemanticParent(cursor); const auto lexical=clang_getCursorLexicalParent(cursor);
            if(!clang_Cursor_isNull(semantic) && clang_getCursorKind(semantic)!=CXCursor_TranslationUnit && symbol_kind(semantic)!=SymbolKind::Unknown) fact.semantic_parent=cursor_key(semantic);
            if(!clang_Cursor_isNull(lexical) && clang_getCursorKind(lexical)!=CXCursor_TranslationUnit && symbol_kind(lexical)!=SymbolKind::Unknown) fact.lexical_parent=cursor_key(lexical);
            fact.spelling_range=range(clang_getCursorExtent(cursor)); fact.expansion_range=range(clang_getCursorExtent(cursor),true);
            fact.linkage=linkage(clang_getCursorLinkage(cursor)); fact.visibility=visibility(clang_getCursorVisibility(cursor)); fact.access=access(clang_getCXXAccessSpecifier(cursor));
            fact.is_definition=clang_isCursorDefinition(cursor)!=0; fact.is_declaration=true;
            fact.is_anonymous=fact.name.empty();fact.type_key=add_type(context,clang_getCursorType(cursor));
            const auto declaration_tokens=declaration_token_text(context,cursor);const auto body=declaration_tokens.find('{');const auto declaration_prefix=declaration_tokens.substr(0,body);fact.exported=declaration_prefix.find("dllexport")!=std::string::npos;
            fact.is_implicit=fact.expansion_range.begin.path.empty()||fact.expansion_range.begin.line==0;
            if(callable(kind)) {
                fact.result_type_key=add_type(context,clang_getCursorResultType(cursor));
                const int count=clang_Cursor_getNumArguments(cursor);for(int i=0;i<count;++i)fact.parameter_type_keys.push_back(add_type(context,clang_getCursorType(clang_Cursor_getArgument(cursor,i))));
                fact.function.variadic=clang_Cursor_isVariadic(cursor)!=0;
                if(cursor_kind==CXCursor_CXXMethod||cursor_kind==CXCursor_Constructor||cursor_kind==CXCursor_Destructor||cursor_kind==CXCursor_ConversionFunction) {
                    fact.function.is_static=cursor_kind==CXCursor_CXXMethod&&clang_CXXMethod_isStatic(cursor)!=0; fact.function.is_virtual=clang_CXXMethod_isVirtual(cursor)!=0;
                    fact.function.is_pure_virtual=clang_CXXMethod_isPureVirtual(cursor)!=0; fact.function.is_const=clang_CXXMethod_isConst(cursor)!=0;
                    fact.function.is_defaulted=clang_CXXMethod_isDefaulted(cursor)!=0; fact.function.is_deleted=clang_CXXMethod_isDeleted(cursor)!=0;
                }
                const auto exception=clang_getCursorExceptionSpecificationType(cursor);
                fact.function.is_noexcept=exception==CXCursor_ExceptionSpecificationKind_BasicNoexcept || exception==CXCursor_ExceptionSpecificationKind_ComputedNoexcept || exception==CXCursor_ExceptionSpecificationKind_NoThrow;
                fact.function.is_constexpr=declaration_prefix.find("constexpr")!=std::string::npos;fact.function.is_consteval=declaration_prefix.find("consteval")!=std::string::npos;fact.function.is_explicit=declaration_prefix.find("explicit")!=std::string::npos;
                fact.function.is_defaulted|=declaration_tokens.find("= default")!=std::string::npos;fact.function.is_deleted|=declaration_tokens.find("= delete")!=std::string::npos;
            }
            const auto storage=clang_Cursor_getStorageClass(cursor);fact.variable.is_static=storage==CX_SC_Static;fact.variable.is_thread_local=clang_getCursorTLSKind(cursor)!=CXTLS_None;
            if(cursor_kind==CXCursor_FieldDecl)fact.variable.is_mutable=clang_CXXField_isMutable(cursor)!=0;
            if(kind==SymbolKind::ClassTemplate || kind==SymbolKind::FunctionTemplate || kind==SymbolKind::VariableTemplate || kind==SymbolKind::AliasTemplate) fact.templ.is_primary=true;
            fact.templ.dependent=clang_getCursorType(cursor).kind==CXType_Dependent||clang_getCursorType(cursor).kind==CXType_Unexposed;
            if(cursor_kind==CXCursor_ClassTemplatePartialSpecialization) { fact.templ.is_primary=false; fact.templ.is_partial_specialization=true; }
            if (indexed!=context.indexed_variables.end()) {
                fact.templ={};
                fact.templ.is_instantiation=indexed->second.instantiated;
                fact.templ.is_primary=!fact.templ.is_instantiation&&indexed->second.template_kind==CXIdxEntity_Template;
                fact.templ.is_partial_specialization=!fact.templ.is_instantiation&&indexed->second.template_kind==CXIdxEntity_TemplatePartialSpecialization;
                fact.templ.is_explicit_specialization=!fact.templ.is_instantiation&&indexed->second.template_kind==CXIdxEntity_TemplateSpecialization;
                fact.templ.dependent=fact.templ.is_primary||fact.templ.is_partial_specialization;
                if (fact.templ.is_instantiation) add_relationship(context,RelationshipKind::Instantiates,own_key,indexed->second.primary,cursor);
            }
            context.batch->symbols.push_back(std::move(fact));
            const auto& emitted=context.batch->symbols.back();
            auto type_edge=[&](RelationshipKind relationship,const std::string& type_key){if(type_key.empty())return;RelationshipFact edge;edge.kind=relationship;edge.source_local_key=own_key;edge.target_type_key=type_key;edge.evidence=emitted.expansion_range;edge.resolution=ResolutionStatus::Exact;edge.origin=EvidenceOrigin::CompilerAST;context.batch->relationships.push_back(std::move(edge));};
            if(kind==SymbolKind::Field)type_edge(RelationshipKind::FieldType,emitted.type_key);else if(kind==SymbolKind::Parameter)type_edge(RelationshipKind::ParameterType,emitted.type_key);else if(!callable(kind))type_edge(RelationshipKind::UsesType,emitted.type_key);
            if(callable(kind)){type_edge(RelationshipKind::ReturnsType,emitted.result_type_key);for(const auto& parameter:emitted.parameter_type_keys)type_edge(RelationshipKind::ParameterType,parameter);}
        }
        if(!active_source.empty() && own_key!=active_source) add_relationship(context,RelationshipKind::Contains,active_source,cursor,cursor);
        if(kind==SymbolKind::LocalVariable&&!active_source.empty()) {
            const auto tokens=declaration_token_text(context,cursor);
            if(tokens.find('=')!=std::string::npos||tokens.find('{')!=std::string::npos)add_relationship(context,RelationshipKind::Writes,active_source,cursor,cursor);
        }
        if (local_callable_storage(cursor))
            observe_callable_value(context, cursor, clang_Cursor_getVarDeclInitializer(cursor), active_source, cursor);
        observe_automatic_destruction(context,cursor,active_source);
        if(scope(kind)) active_source=own_key;

        if(cursor_kind==CXCursor_CXXMethod||cursor_kind==CXCursor_Destructor||cursor_kind==CXCursor_ConversionFunction) {
            CXCursor* overridden{}; unsigned count{}; clang_getOverriddenCursors(cursor,&overridden,&count);
            for(unsigned i=0;i<count;++i) {ensure_implicit_symbol(context,overridden[i]);add_relationship(context,RelationshipKind::Overrides,own_key,overridden[i],cursor);}
            if(overridden) clang_disposeOverriddenCursors(overridden);
        }
        const auto primary=clang_getSpecializedCursorTemplate(cursor);
        if(!clang_Cursor_isNull(primary)) {
            const auto specialization=cursor_kind==CXCursor_ClassTemplatePartialSpecialization || is_explicit_specialization(context,cursor);
            for(auto it=context.batch->symbols.rbegin();it!=context.batch->symbols.rend();++it)if(it->local_identity==own_key){it->templ.is_primary=false;it->templ.is_partial_specialization=cursor_kind==CXCursor_ClassTemplatePartialSpecialization;it->templ.is_explicit_specialization=specialization&&!it->templ.is_partial_specialization;it->templ.is_instantiation=!specialization;break;}
            add_relationship(context,specialization?RelationshipKind::Specializes:RelationshipKind::Instantiates,own_key,primary,cursor);
        }
    } else if(cursor_kind==CXCursor_CXXOverrideAttr||cursor_kind==CXCursor_CXXFinalAttr){
        for(auto it=context.batch->symbols.rbegin();it!=context.batch->symbols.rend();++it)if(it->local_identity==active_source){if(cursor_kind==CXCursor_CXXOverrideAttr)it->function.is_override=true;else it->function.is_final=true;break;}
    } else if (cursor_kind==CXCursor_CXXBaseSpecifier) {
        const auto target=clang_getTypeDeclaration(clang_getCursorType(cursor));
        if(auto* edge=add_relationship(context,RelationshipKind::Inherits,active_source,target,cursor,DispatchKind::None,clang_Cursor_isNull(target)?ResolutionStatus::Unresolved:ResolutionStatus::Exact)) {
            edge->access=access(clang_getCXXAccessSpecifier(cursor)); edge->is_virtual=clang_isVirtualBase(cursor)!=0; edge->is_dependent=clang_getCursorType(cursor).kind==CXType_Unexposed;
        }
    } else if (cursor_kind == CXCursor_BinaryOperator && clang_getCursorBinaryOperatorKind(cursor) == CXBinaryOperator_Assign) {
        const auto operands = children(cursor);
        if (operands.size() == 2) {
            const auto storage = callable_value_reference(context, operands[0]);
            observe_callable_value(context, storage, operands[1], active_source, cursor);
        }
    } else if(cursor_kind==CXCursor_CXXNewExpr) {
        auto allocated=clang_getCursorType(cursor);if(allocated.kind==CXType_Pointer)allocated=clang_getPointeeType(allocated);
        const auto target=clang_getTypeDeclaration(allocated);if(!clang_Cursor_isNull(target))add_relationship(context,RelationshipKind::Constructs,active_source,target,cursor);
    } else if (cursor_kind==CXCursor_CallExpr) {
        const auto referenced=clang_getCursorReferenced(cursor);
        auto target=callable_reference(referenced)?referenced:clang_getNullCursor();
        if (!clang_Cursor_isNull(target) && clang_getCursorKind(target)!=CXCursor_Constructor) {
            // libclang can forward a factory/operator[] declaration through
            // the call of its returned function pointer. Bind that inner call
            // separately; it is not the target of the outer invocation.
            const auto callee=first_child(cursor);
            const auto callee_type=clang_getCanonicalType(clang_getCursorType(callee));
            if (function_pointer(callee_type) || callee_type.kind==CXType_FunctionProto || callee_type.kind==CXType_FunctionNoProto)
                if (!callable_reference(callable_value_reference(context,callee))) target=clang_getNullCursor();
        }
        ensure_implicit_symbol(context,target);
        auto dispatch=clang_Cursor_isNull(target)?DispatchKind::Dynamic:DispatchKind::Static;
        if(!clang_Cursor_isNull(target) && clang_Cursor_isDynamicCall(cursor)) dispatch=DispatchKind::Virtual;
        add_relationship(context,RelationshipKind::Calls,active_source,target,cursor,dispatch,clang_Cursor_isNull(target)?ResolutionStatus::Unresolved:ResolutionStatus::Exact);
        if (clang_Cursor_isNull(target)) {
            const auto storage = callable_value_reference(context, first_child(cursor));
            if (local_callable_storage(storage))
                add_relationship(context, RelationshipKind::InvokesCallable, active_source, storage, cursor, DispatchKind::Dynamic);
        }
        if(!clang_Cursor_isNull(target)&&clang_getCursorKind(target)==CXCursor_Constructor)add_relationship(context,RelationshipKind::Constructs,active_source,target,cursor);
        if(!clang_Cursor_isNull(target)&&clang_getCursorKind(target)==CXCursor_Destructor)add_relationship(context,RelationshipKind::Destroys,active_source,target,cursor);
    } else if(cursor_kind==CXCursor_CXXDeleteExpr) {
        const auto operand=first_child(cursor);auto destroyed=clang_getCursorType(operand);if(destroyed.kind==CXType_Pointer)destroyed=clang_getPointeeType(destroyed);
        const auto target=destroyed.kind==CXType_Invalid?clang_getNullCursor():clang_getTypeDeclaration(destroyed);
        if(!clang_Cursor_isNull(target))add_relationship(context,RelationshipKind::Destroys,active_source,target,cursor);else add_type_relationship(context,RelationshipKind::Destroys,active_source,destroyed,cursor);
    } else if(cursor_kind==CXCursor_UnaryOperator) {
        const auto tokens=token_text(context,cursor);const auto first=tokens.find_first_not_of(" \t\r\n");
        if(first!=std::string::npos&&tokens[first]=='&'){
            // A direct reference child is handled by reference_access below.
            // Add a fallback only when libclang inserted an unexposed wrapper
            // between the unary operator and the referenced declaration.
            const auto direct=first_child(cursor);const auto direct_target=clang_getCursorReferenced(direct);
            if(clang_Cursor_isNull(direct_target)||(!callable_reference(direct_target)&&!variable_reference(direct_target))){
                const auto target=first_referenced_descendant(cursor);
                if(!clang_Cursor_isNull(target))add_relationship(context,RelationshipKind::TakesAddress,active_source,target,cursor);
            }
        }
    } else if (clang_isReference(cursor_kind)||cursor_kind==CXCursor_DeclRefExpr||cursor_kind==CXCursor_MemberRefExpr||cursor_kind==CXCursor_VariableRef) {
        const auto target=clang_getCursorReferenced(cursor);
        add_relationship(context,RelationshipKind::References,active_source,target,cursor);
        const auto mode=reference_access(context,cursor,parent);
        if(callable_reference(target)&&mode==ReferenceAccess::Address)add_relationship(context,RelationshipKind::TakesAddress,active_source,target,cursor);
        if(variable_reference(target)) {
            if(mode==ReferenceAccess::Address)add_relationship(context,RelationshipKind::TakesAddress,active_source,target,cursor);
            else {
                if(mode==ReferenceAccess::Read||mode==ReferenceAccess::ReadWrite)add_relationship(context,RelationshipKind::Reads,active_source,target,cursor);
                if(mode==ReferenceAccess::Write||mode==ReferenceAccess::ReadWrite)add_relationship(context,RelationshipKind::Writes,active_source,target,cursor);
            }
        }
    }

    std::pair<Context*,std::string> child_context{&context,std::move(active_source)};
    clang_visitChildren(cursor,child_visitor,&child_context);
}

void inclusion_visitor(CXFile included, CXSourceLocation* stack, unsigned length, CXClientData data) {
    auto& context=*static_cast<Context*>(data);
    IncludeFact fact; fact.included_path=normalize_path(text(clang_getFileName(included))); fact.resolved=!fact.included_path.empty();
    if(length) { fact.directive=point(stack[0],true); fact.including_path=fact.directive.path; }
    if(fact.including_path.empty()) fact.including_path=normalize_path(context.batch->command.source_path);
    if(!project_owned(context,fact.including_path))return;
    context.batch->includes.push_back(std::move(fact));
}

std::vector<std::string> libclang_arguments(const CompilationCommand& command) {
    return command.semantic_arguments;
}

} // namespace

struct LibClangFrontend::Impl {
    CXIndex index{clang_createIndex(0,0)};
    ~Impl() { if(index) clang_disposeIndex(index); }
};

LibClangFrontend::LibClangFrontend() : impl_(std::make_unique<Impl>()) {
    if(!impl_->index) throw std::runtime_error("clang_createIndex failed");
}
LibClangFrontend::~LibClangFrontend() = default;

FrontendVersion frontend_version() {
    FrontendVersion result;
#ifdef CODEINSIGHT_CONFIGURED_CLANG_VERSION
    result.configured=CODEINSIGHT_CONFIGURED_CLANG_VERSION;
#endif
    result.runtime=text(clang_getClangVersion());
    const auto marker=result.runtime.find("version ");
    if(marker!=std::string::npos) {
        const auto begin=result.runtime.data()+marker+8;
        const auto end=result.runtime.data()+result.runtime.size();
        (void)std::from_chars(begin,end,result.major);
    }
    return result;
}

FactBatch LibClangFrontend::extract(const CompilationRequest& request) {
    using Clock=std::chrono::steady_clock;const auto started=Clock::now();
    const bool profile_tu=profile_translation_units();
    if(profile_tu)std::osyncstream(std::cerr)<<"profile-tu-start|"<<normalize_path(request.command.source_path)<<'\n';
    FactBatch batch; batch.command=request.command; batch.revision_key=request.revision;
    const auto owned_args=libclang_arguments(request.command);
    std::vector<const char*> args; args.reserve(owned_args.size()); for(const auto& arg:owned_args) args.push_back(arg.c_str());
    CXTranslationUnit tu{};
    const auto options=CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing | CXTranslationUnit_IncludeAttributedTypes;
    const auto error=clang_parseTranslationUnit2(impl_->index,request.command.source_path.string().c_str(),args.data(),static_cast<int>(args.size()),nullptr,0,options,&tu);
    const auto parsed=Clock::now();
    if(profile_tu)std::osyncstream(std::cerr)<<"profile-tu-parsed|"<<normalize_path(request.command.source_path)<<"|error:"<<static_cast<int>(error)<<"|parse-ms:"<<std::chrono::duration_cast<std::chrono::milliseconds>(parsed-started).count()<<'\n';
    if(error!=CXError_Success || !tu) {
        batch.quality=ExtractionQuality::Failed;
        batch.diagnostics.push_back({4,"libclang failed to parse translation unit (error "+std::to_string(error)+")","libclang",{}});
        return batch;
    }
    struct Dispose { CXTranslationUnit tu; ~Dispose(){ clang_disposeTranslationUnit(tu); } } dispose{tu};
    Context context{&batch,tu};context.source_key=normalize_path(request.command.source_path);for(const auto& root:request.command.project_roots)context.project_roots.push_back(normalize_path(root));
    const auto action=clang_IndexAction_create(impl_->index);
    struct DisposeAction { CXIndexAction value; ~DisposeAction(){clang_IndexAction_dispose(value);} } dispose_action{action};
    IndexerCallbacks callbacks{};
    callbacks.indexDeclaration=index_variable_declaration; callbacks.indexEntityReference=index_variable_reference;
    const int index_error=clang_indexTranslationUnit(action,&context,&callbacks,sizeof(callbacks),CXIndexOpt_IndexFunctionLocalSymbols,tu);
    if (index_error) batch.diagnostics.push_back({3,"libclang declaration indexing failed (error "+std::to_string(index_error)+")","libclang-index",{}});
    visit_cursor(context,clang_getTranslationUnitCursor(tu),clang_getNullCursor(),{});
    // Indexed variable-template declarations have a VarDecl payload cursor
    // absent from the ordinary TU child list. Visit that compiler cursor too.
    for (const auto& [_, variable] : context.indexed_variables)
        if (!variable.instantiated)
            visit_cursor(context,variable.primary,clang_getCursorSemanticParent(variable.primary),{});
    clang_getInclusions(tu,inclusion_visitor,&context);
    const auto visited=Clock::now();

    bool errors=index_error!=0; bool warnings{};
    const auto diagnostic_count=clang_getNumDiagnostics(tu);
    for(unsigned i=0;i<diagnostic_count;++i) {
        const auto diagnostic=clang_getDiagnostic(tu,i);
        DiagnosticFact fact; fact.severity=clang_getDiagnosticSeverity(diagnostic); fact.message=text(clang_getDiagnosticSpelling(diagnostic));
        CXString disable{}; fact.option=text(clang_getDiagnosticOption(diagnostic,&disable)); clang_disposeString(disable);
        const auto count=clang_getDiagnosticNumRanges(diagnostic); fact.range=count?range(clang_getDiagnosticRange(diagnostic,0),true):RawSourceRange{point(clang_getDiagnosticLocation(diagnostic),true),point(clang_getDiagnosticLocation(diagnostic),true)};
        errors |= fact.severity>=CXDiagnostic_Error; warnings |= fact.severity==CXDiagnostic_Warning;
        batch.diagnostics.push_back(std::move(fact)); clang_disposeDiagnostic(diagnostic);
    }
    batch.quality=errors?ExtractionQuality::Partial:(warnings?ExtractionQuality::CompleteWithWarnings:ExtractionQuality::Complete);
    if(profile_tu){
        const auto finished=Clock::now();const auto milliseconds=[](auto begin,auto end){return std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();};
        std::osyncstream(std::cerr)<<"profile-tu|"<<normalize_path(request.command.source_path)<<"|parse-ms:"<<milliseconds(started,parsed)<<"|visit-ms:"<<milliseconds(parsed,visited)<<"|diagnostics-ms:"<<milliseconds(visited,finished)<<"|total-ms:"<<milliseconds(started,finished)<<'\n';
    }
    return batch;
}

} // namespace codeinsight
