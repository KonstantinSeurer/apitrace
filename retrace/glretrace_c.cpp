
#include "glproc.h"
#include "glretrace.hpp"
#include "retrace.hpp"
#include "retrace_swizzle.hpp"

#include "eglimports_h.h"
#include "glimports_h.h"
#include "glproc_cpp.h"
#include "glproc_h.h"
#include "os_backtrace_cpp.h"
#include "os_backtrace_hpp.h"
#include "os_hpp.h"
#include "os_posix_cpp.h"
#include "os_string_hpp.h"
#include "retrace_library_hpp.h"
#include "retrace_swizzle_cpp.h"
#include "retrace_swizzle_hpp.h"
#include "trace_model_cpp.h"
#include "trace_model_hpp.h"

#include "md5.h"

#include <snappy.h>

#include <filesystem>
#include <unordered_map>
#include <unordered_set>

static std::unordered_set<std::string> ignore_calls = {
    "wglDescribePixelFormat",
    "glDebugMessageCallback",
};

static std::unordered_set<std::string> handle_maps;

static uint32_t sequence_index = 0;
static bool is_wsi_sequence = true;

static std::filesystem::path target_directory;

static std::vector<std::string> generated_filenames = {
    "main.cpp",
    "state.cpp",
    "glproc.cpp",
    "os_posix.cpp",
    "os_backtrace.cpp",
    "trace_model.cpp",
    "retrace_swizzle.cpp",
};

static uint32_t out_param_index = 0;

static FILE *sequence_h_file = nullptr;
static FILE *sequence_file = nullptr;
static FILE *main_file = nullptr;
static FILE *values_file = nullptr;
static FILE *state_file = nullptr;
static FILE *data_file = nullptr;

#define DATA_CHUNK_SIZE (1 * 1024 * 1024)
static long long unsigned data_offset = 0;
static uint8_t *data_buffer = nullptr;
static void *compressed_data_buffer = nullptr;
static std::unordered_map<std::string, long long unsigned> data_map;

static std::vector<char> data;

static std::unordered_map<std::string, std::string> value_variables;
static uint32_t value_index = 0;

static uint32_t call_index = 0;

static GLint active_program = 0;
static GLint active_pipeline = 0;
static std::unordered_map<GLint, GLint> pipeline_active_programs;

static std::vector<trace::Call *> calls;

#define MAX_SEQUENCE_LENGTH 10000

static long long
get_int_arg(const trace::Call &call, const char *name, long long default_value) {
    for (uint32_t i = 0; i < call.args.size(); i++) {
        if (name && !strcmp(name, call.sig->arg_names[i]))
            return call.args[i].value->toSInt();
    }
    return default_value;
}

static void
register_handle_map(const retrace::HandleType *handle_type) {
    std::string map_type = std::string("retrace::map<") + handle_type->c_decl + std::string(">");
    if (handle_type->key_type) {
      map_type = "std::unordered_map<" + handle_type->key_type->c_decl + ", " +
                 map_type + ">";
    }

    std::string decl = map_type + std::string(" ") +
                       std::string(handle_type->name) + std::string("_map");

    if (handle_maps.find(decl) == handle_maps.end()) {
        if (handle_type->key_type) {
            fprintf(sequence_h_file,
                    "void set_%s(%s key, %s trace, uint32_t range, %s actual);\n",
                    handle_type->name, handle_type->key_type->c_decl.c_str(),
                    handle_type->c_decl.c_str(), handle_type->c_decl.c_str());
            fprintf(
                state_file,
                "void\nset_%s(%s key, %s trace, uint32_t range, %s actual) {\n    "
                "for (uint32_t i = 0; i < range; i++) "
                "%s_map[key][(%s)((uintptr_t)trace + i)] = "
                "(%s)((uintptr_t)actual + i);\n}\n",
                handle_type->name, handle_type->key_type->c_decl.c_str(),
                handle_type->c_decl.c_str(), handle_type->c_decl.c_str(),
                handle_type->name, handle_type->c_decl.c_str(),
                handle_type->c_decl.c_str());

            fprintf(sequence_h_file, "%s get_%s(%s key, %s trace);\n",
                    handle_type->c_decl.c_str(), handle_type->name,
                    handle_type->key_type->c_decl.c_str(),
                    handle_type->c_decl.c_str());
            fprintf(
                state_file,
                "%s\nget_%s(%s key, %s trace) {\n    return %s_map[key]",
                handle_type->c_decl.c_str(), handle_type->name,
                handle_type->key_type->c_decl.c_str(), handle_type->c_decl.c_str(),
                handle_type->name);
            if (!strcmp("location", handle_type->name)) {
                fprintf(state_file, ".lookupUniformLocation(trace);\n}\n");
            } else {
                fprintf(state_file, "[trace];\n}\n");
            }

            if (!strcmp("uniformBlock", handle_type->name)) {
                fprintf(state_file, "%s", R"(
void
mapUniformBlockName(GLuint program, GLint index, const char *name) {
    program = get_program(program);
    GLint num_blocks = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &num_blocks);
    for (int i = 0; i < num_blocks; i++) {
        GLint buf_len;
        glGetActiveUniformBlockiv(program, i, GL_UNIFORM_BLOCK_NAME_LENGTH, &buf_len);
        std::vector<char> name_buf(buf_len + 1);
        GLint length;
        glGetActiveUniformBlockName(program, i, name_buf.size(), &length, name_buf.data());
        name_buf[length] = '\0';
        if (!strcmp(name, name_buf.data())) {
            uniformBlock_map[program][index] = i;
            return;
        }
    }
}
)");
            }
        } else {
            fprintf(sequence_h_file,
                    "void set_%s(%s trace, uint32_t range, %s actual);\n",
                    handle_type->name, handle_type->c_decl.c_str(),
                    handle_type->c_decl.c_str());
            fprintf(state_file,
                    "void\nset_%s(%s trace, uint32_t range, %s actual) {\n    for "
                    "(uint32_t i = 0; i < range; i++) %s_map[(%s)((uintptr_t)trace + "
                    "i)] = "
                    "(%s)((uintptr_t)actual + i);\n}\n",
                    handle_type->name, handle_type->c_decl.c_str(),
                    handle_type->c_decl.c_str(), handle_type->name,
                    handle_type->c_decl.c_str(), handle_type->c_decl.c_str());

            fprintf(sequence_h_file, "%s get_%s(%s trace);\n",
                    handle_type->c_decl.c_str(), handle_type->name,
                    handle_type->c_decl.c_str());
            fprintf(state_file,
                    "%s\nget_%s(%s trace) {\n    return %s_map[trace];\n}\n",
                    handle_type->c_decl.c_str(), handle_type->name,
                    handle_type->c_decl.c_str(), handle_type->name);
        }
    }

    handle_maps.insert(decl);
}

static uint64_t
get_handle_default_value(const char *name) {
    if (strcmp("program", name))
        return 0;

    uint64_t program = active_program;

    if (active_pipeline && pipeline_active_programs.find(active_pipeline) != pipeline_active_programs.end())
      program = pipeline_active_programs.at(active_pipeline);

    return program;
}

static void
print_get_handle(FILE *out, const trace::Call &call, const retrace::HandleType *handle_type,
                 unsigned long long handle) {
    register_handle_map(handle_type);
    if (handle_type->key_name) {
        long long key = get_int_arg(call, handle_type->key_name, get_handle_default_value(handle_type->key_name));
        fprintf(out, "get_%s((%s)%lli, (%s)%lli)", handle_type->name,
                handle_type->key_type->c_decl.c_str(), key, handle_type->c_decl.c_str(), handle);
    } else {
        fprintf(out, "get_%s((%s)%lli)", handle_type->name, handle_type->c_decl.c_str(), handle);
    }
}

static void
print_set_handle(FILE *out, const trace::Call &call, const retrace::HandleType *handle_type,
                 long long handle) {
    register_handle_map(handle_type);
    if (handle_type->key_name) {
        long long key = get_int_arg(call, handle_type->key_name, get_handle_default_value(handle_type->key_name));
        fprintf(out, "set_%s((%s)%lli, (%s)%lli, %lli, ", handle_type->name, handle_type->key_type->c_decl.c_str(), key,
                handle_type->c_decl.c_str(), handle, get_int_arg(call, handle_type->range, 1));
    } else {
        fprintf(out, "set_%s((%s)%lli, %lli, ", handle_type->name,
                handle_type->c_decl.c_str(), handle, get_int_arg(call, handle_type->range, 1));
    }
}

static void
flush_data_buffer(long long unsigned size)
{
    size_t compressed_size = 0;
    snappy::RawCompress((const char *)data_buffer, size, (char *)compressed_data_buffer, &compressed_size);
    fwrite(&compressed_size, sizeof(size_t), 1, data_file);
    fwrite(compressed_data_buffer, compressed_size, 1, data_file);
}

static long long unsigned
get_blob_offset(void *data, long long unsigned size)
{
    struct MD5Context md5c;
    MD5Init(&md5c);
    MD5Update(&md5c, (unsigned char *)data, size);
    unsigned char signature[16];
    MD5Final(signature, &md5c);

    const char hex[] = "0123456789ABCDEF";
    char csig[33];
    for(int i = 0; i < sizeof(signature); i++){
        csig[2*i    ] = hex[signature[i] >> 4];
        csig[2*i + 1] = hex[signature[i] & 0xf];
    }
    csig[32] = '\0';

    std::string hash = csig;
    if (data_map.find(hash) != data_map.end())
        return data_map.at(hash);

    long long unsigned written_size = 0;
    while (written_size < size) {
        long long unsigned dst_offset = (data_offset + written_size) % DATA_CHUNK_SIZE;
        long long unsigned write_size = std::min(DATA_CHUNK_SIZE - dst_offset, size - written_size);
        memcpy(data_buffer + dst_offset, (uint8_t *)data + written_size, write_size);
        written_size += write_size;

        if (dst_offset + write_size == DATA_CHUNK_SIZE)
            flush_data_buffer(DATA_CHUNK_SIZE);
    }

    long long unsigned result_offset = data_offset;
    data_map[hash] = result_offset;
    data_offset += size;
    return result_offset;
}

static bool
print_value_expression(FILE *out, const retrace::ValueType *type, const trace::Call &call,
                       trace::Value *value) {
    if (type->kind == retrace::ValueTypeKind::_const) {
        const retrace::ConstType *const_type = (const retrace::ConstType *)type;
        print_value_expression(out, const_type->type, call, value);
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::literal) {
        const retrace::LiteralType *literal_type =
            (const retrace::LiteralType *)type;
        if (!strcmp("Bool", literal_type->encodedKind)) {
          if (value->toBool())
            fprintf(out, "true");
          else
            fprintf(out, "false");
        } else if (!strcmp("SInt", literal_type->encodedKind)) {
          fprintf(out, "%lli", value->toSInt());
        } else if (!strcmp("UInt", literal_type->encodedKind)) {
          fprintf(out, "%llu", value->toUInt());
        } else if (!strcmp("Float", literal_type->encodedKind)) {
          fprintf(out, "%f", value->toFloat());
        } else if (!strcmp("Double", literal_type->encodedKind)) {
          fprintf(out, "%f", value->toDouble());
        } else {
          abort();
        }
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::pointer ||
          type->kind == retrace::ValueTypeKind::int_pointer) {
        fprintf(out, "(%s)0x%llx", type->c_decl.c_str(),
                (long long unsigned)value->toPointer());
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::obj_pointer ||
        type->kind == retrace::ValueTypeKind::linear_pointer ||
        type->kind == retrace::ValueTypeKind::reference) {
        // const retrace::PointerType *pointer_type = (const retrace::PointerType
        // *)type;
        abort();
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::handle) {
        const retrace::HandleType *handle_type = (const retrace::HandleType *)type;
        print_get_handle(out, call, handle_type, value->toUInt());
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::_enum) {
        fprintf(out, "%lli", value->toSInt());
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::bitmask) {
        trace::Bitmask *v = (trace::Bitmask *)value;
        fprintf(out, "0x%llx", v->value);
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::array) {
        const retrace::ArrayType *array_type = (const retrace::ArrayType *)type;
        trace::Array *v = value->toArray();
        if (!v) {
            fprintf(out, "NULL");
            return false;
        }
        fprintf(out, "(%s[]){", array_type->type->c_decl.c_str());
        for (uint32_t i = 0; i < v->values.size(); i++) {
            if (i)
                fprintf(out, ", ");
            print_value_expression(out, array_type->type, call, v->values[i]);
        }
        fprintf(out, "}");
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::attrib_array) {
        abort();
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::blob) {
        const retrace::BlobType *blob_type = (const retrace::BlobType *)type;
        if (value->toNull()) {
            fprintf(out, "NULL");
            return false;
        }

        trace::Pointer *p = dynamic_cast<trace::Pointer *>(value);
        if (p) {
            fprintf(out, "(%s)toPointer(%llu)", type->c_decl.c_str(), p->toUIntPtr());
            return false;
        }
      
        trace::Blob *v = dynamic_cast<trace::Blob *>(value);
        if (!v)
            return true;
      
        const char *type_name = blob_type->type->c_decl.c_str();
        if (strstr(type_name, "void"))
            type_name = "unsigned char";

        fprintf(out, "(%s)getData(%llu)", blob_type->c_decl.c_str(), get_blob_offset(v->buf, v->size));
      
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::_struct) {
        const retrace::StructType *struct_type = (const retrace::StructType *)type;
        trace::Struct *v = (trace::Struct *)value;
        fprintf(out, "(%s){", v->sig->name);
        for (uint32_t i = 0; i < v->members.size(); i++) {
            if (i)
                fprintf(out, ", ");
            print_value_expression(out, struct_type->members[i].type, call,
                                   v->members[i]);
        }
        fprintf(out, "}");
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::alias) {
        const retrace::AliasType *alias_type = (const retrace::AliasType *)type;
        print_value_expression(out, alias_type->type, call, value);
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::string) {
        trace::String *v = (trace::String *)value;
        fprintf(out, "R\"(%s)\"", v->value);
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::opaque) {
        fprintf(out, "(%s)toPointer(0x%llx)", type->c_decl.c_str(),
                (long long unsigned)value->toPointer());
        return false;
    }

    if (type->kind == retrace::ValueTypeKind::polymorphic) {
        const retrace::PolymorphicType *polymorphic_type =
            (const retrace::PolymorphicType *)type;
        if (print_value_expression(out, polymorphic_type->switchTypes[0], call,
                                   value))
            print_value_expression(out, polymorphic_type->defaultType, call, value);
        return false;
    }

    abort();
    return true;
}

static const std::string &
deduplicate_value_construction(const std::string &construction) {
    if (value_variables.find(construction) == value_variables.end()) {
        std::string variable_name =
            std::string("value") + std::to_string(value_index);
        value_index++;
      
        fprintf(values_file, "static auto %s = %s;\n", variable_name.c_str(),
                construction.c_str());
      
        value_variables[construction] = variable_name;
    }

    return value_variables.at(construction);
}

static std::string
get_value_construction(trace::Value *value) {
    if (!value) {
        return "nullptr";
    }

    if (auto v = dynamic_cast<trace::Bool *>(value)) {
        return v->value ? "&true_value" : "&false_value";
    }

    if (dynamic_cast<trace::Null *>(value)) {
        return "&null_value";
    }

    if (auto v = dynamic_cast<trace::SInt *>(value)) {
        return deduplicate_value_construction("new trace::SInt(" + std::to_string(v->value) + ")");
    }

    if (auto v = dynamic_cast<trace::Bitmask *>(value)) {
        const trace::BitmaskSig *sig = v->sig;
        std::string expr = "createBitmask(" + std::to_string(sig->id) + ", {";
        for (uint32_t i = 0; i < sig->num_flags; i++) {
            if (i)
                expr += ", ";
            expr += "{\"";
            expr += sig->flags[i].name;
            expr += "\", ";
            expr += std::to_string(sig->flags[i].value);
            expr += "}";
        }
        expr += "}, ";
        expr += std::to_string(v->value);
        expr += ")";
        return deduplicate_value_construction(expr);
    }

    if (auto v = dynamic_cast<trace::Pointer *>(value)) {
        return deduplicate_value_construction("new trace::Pointer(" + std::to_string(v->value) + ")");
    }

    if (auto v = dynamic_cast<trace::UInt *>(value)) {
        return deduplicate_value_construction("new trace::UInt(" + std::to_string(v->value) + ")");
    }

    if (auto v = dynamic_cast<trace::Float *>(value)) {
        return deduplicate_value_construction("new trace::Float(" + std::to_string(v->value) + ")");
    }

    if (auto v = dynamic_cast<trace::Double *>(value)) {
        return deduplicate_value_construction("new trace::Double(" + std::to_string(v->value) + ")");
    }

    if (auto v = dynamic_cast<trace::String *>(value)) {
        return deduplicate_value_construction("new trace::String(\"" + std::string(v->value) + "\")");
    }

    if (dynamic_cast<trace::WString *>(value)) {
        abort();
        return "nullptr";
    }

    if (dynamic_cast<trace::Enum *>(value)) {
        abort();
        return "nullptr";
    }

    if (auto v = dynamic_cast<trace::Struct *>(value)) {
        const trace::StructSig *sig = v->sig;
        std::string expr = "createStruct(" + std::to_string(sig->id) + ", \"" + sig->name + "\", {";
        for (uint32_t i = 0; i < v->members.size(); i++) {
            if (i)
                expr += ", ";
            expr += "{\"";
            expr += sig->member_names[i];
            expr += "\", ";
            expr += get_value_construction(v->members[i]);
            expr += "}";
        }
        expr += "})";
        return deduplicate_value_construction(expr);
    }

    if (auto v = dynamic_cast<trace::Array *>(value)) {
        std::string expr = "createArray({";
        for (uint32_t i = 0; i < v->values.size(); i++) {
            if (i)
                expr += ", ";
            expr += get_value_construction(v->values[i]);
        }
        expr += "})";
        return deduplicate_value_construction(expr);
    }

    if (dynamic_cast<trace::Blob *>(value)) {
        abort();
        return "nullptr";
    }

    if (dynamic_cast<trace::Repr *>(value)) {
        abort();
        return "nullptr";
    }

    abort();
}

static std::string
get_call_construction(trace::Call *call) {
    const trace::FunctionSig *sig = call->sig;
    std::string expr = "createCall(" + std::to_string(sig->id) + ", \"" + sig->name + "\", " + std::to_string(call->flags) + ", ";
    expr += get_value_construction(call->ret);
    expr += ", {";
    for (uint32_t i = 0; i < call->args.size(); i++) {
        if (i)
            expr += ", ";
        expr += "{\"";
        expr += sig->arg_names[i];
        expr += "\", ";
        expr += get_value_construction(call->args[i].value);
        expr += "}";
    }
    expr += "})";
    return deduplicate_value_construction(expr);
}

static bool value_type_has_handle(const retrace::ValueType *type) {
    if (type->kind == retrace::ValueTypeKind::_const)
        return value_type_has_handle(((const retrace::ConstType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::pointer)
        return value_type_has_handle(((const retrace::PointerType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::obj_pointer)
        return value_type_has_handle(((const retrace::ObjPointerType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::linear_pointer)
        return value_type_has_handle(((const retrace::LinearPointerType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::reference)
        return value_type_has_handle(((const retrace::ReferenceType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::handle)
        return true;

    if (type->kind == retrace::ValueTypeKind::bitmask)
        return value_type_has_handle(((const retrace::BitmaskType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::array)
        return value_type_has_handle(((const retrace::ArrayType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::attrib_array)
        return value_type_has_handle(((const retrace::AttribArrayType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::blob)
        return value_type_has_handle(((const retrace::BlobType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::alias)
        return value_type_has_handle(((const retrace::AliasType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::string)
        return value_type_has_handle(((const retrace::StringType *)type)->type);

    if (type->kind == retrace::ValueTypeKind::polymorphic)
        return value_type_has_handle(((const retrace::PolymorphicType *)type)->switchTypes[0]);

    return false;
}

void glretrace::dump_call_as_c(trace::Call &call) {
    if (ignore_calls.find(call.name()) != ignore_calls.end())
        return;

    bool is_active_uniform_block_name = !strcmp("glGetActiveUniformBlockName", call.name());

    if ((call.flags & trace::CALL_FLAG_NO_SIDE_EFFECTS) && !is_active_uniform_block_name)
        return;

    bool new_wsi_sequence =
        !strncmp("glX", call.name(), 3) || !strncmp("wgl", call.name(), 3);

    bool split_sequence = call_index == MAX_SEQUENCE_LENGTH;
    if (split_sequence)
        call_index = 0;

    if (new_wsi_sequence != is_wsi_sequence || split_sequence) {
        is_wsi_sequence = new_wsi_sequence;

        if (new_wsi_sequence || split_sequence) {
            if (sequence_file) {
                fprintf(sequence_file, "}\n");
                fclose(sequence_file);
                sequence_file = nullptr;

                fprintf(main_file, "    {sequence%u, nullptr, %llu},\n", sequence_index, data_offset);
                sequence_index++;
            }
        }
        if (!new_wsi_sequence || split_sequence) {
            char sequence_filename[256];
            snprintf(sequence_filename, sizeof(sequence_filename), "%s/sequence%u.c",
                     target_directory.c_str(), sequence_index);

            generated_filenames.push_back(sequence_filename);

            sequence_file = fopen(sequence_filename, "w");
            fprintf(sequence_file, "#include \"sequence.h\"\n\n");
            fprintf(sequence_file, "void\n");
            fprintf(sequence_file, "sequence%u() {\n", sequence_index);
            fprintf(sequence_file, "    GLvoid *ptr = NULL; (void)ptr;\n");

            out_param_index = 0;
        }
    }

    if (new_wsi_sequence) {
        fprintf(main_file, "    {nullptr, %s, 0},\n", get_call_construction(&call).c_str());
        return;
    }

    call_index++;

    if (is_active_uniform_block_name) {
        fprintf(sequence_file, "    mapUniformBlockName(%llu, %llu, \"%s\");\n",
                call.arg(0).toUInt(), call.arg(1).toUInt(), call.arg(4).toString());
        return;
    }

    const retrace::FunctionType *func_type = nullptr;
    if (gl_func_types.find(call.name()) != gl_func_types.end())
        func_type = &gl_func_types.at(call.name());

    if (!strcmp("glUnmapBuffer", call.name())) {
        fprintf(sequence_file, "    ptr = NULL;\n");
        fprintf(sequence_file,
                "    glGetBufferPointerv(%u, GL_BUFFER_MAP_POINTER, &ptr);",
                (uint32_t)call.arg(0).toUInt());
        fprintf(sequence_file, "    delRegionByPointer(ptr);\n");
    } else if (!strcmp("glUnmapBufferARB", call.name())) {
        fprintf(sequence_file, "    ptr = NULL;\n");
        fprintf(sequence_file,
                "    glGetBufferPointervARB(%u, GL_BUFFER_MAP_POINTER_ARB, &ptr);",
                (uint32_t)call.arg(0).toUInt());
        fprintf(sequence_file, "    delRegionByPointer(ptr);\n");
    } else if (!strcmp("glUnmapBufferOES", call.name())) {
        fprintf(sequence_file, "    ptr = NULL;\n");
        fprintf(sequence_file,
                "    glGetBufferPointervOES(%u, GL_BUFFER_MAP_POINTER_OES, &ptr);",
                (uint32_t)call.arg(0).toUInt());
        fprintf(sequence_file, "    delRegionByPointer(ptr);\n");
    } else if (!strcmp("glUnmapNamedBuffer", call.name())) {
        fprintf(sequence_file, "    ptr = NULL;\n");
        fprintf(sequence_file, "    glGetNamedBufferPointerv(");
        print_value_expression(sequence_file, func_type->parameter_types[0].type,
                               call, &call.arg(0));
        fprintf(sequence_file, ", GL_BUFFER_MAP_POINTER, &ptr);\n");
        fprintf(sequence_file, "    delRegionByPointer(ptr);\n");
    } else if (!strcmp("glUnmapNamedBufferEXT", call.name())) {
        fprintf(sequence_file, "    ptr = NULL;\n");
        fprintf(sequence_file, "    glGetNamedBufferPointervEXT(");
        print_value_expression(sequence_file, func_type->parameter_types[0].type,
                               call, &call.arg(0));
        fprintf(sequence_file, ", GL_BUFFER_MAP_POINTER, &ptr);\n");
        fprintf(sequence_file, "    delRegionByPointer(ptr);\n");
    } else if (!strcmp("glDeleteBuffers", call.name()) ||
               !strcmp("glDeleteBuffersARB", call.name())) {
        const trace::Array *buffers = (const trace::Array *)&call.arg(1);

        assert(func_type->parameter_types[1].type->kind ==
               retrace::ValueTypeKind::array);
        const retrace::ArrayType *buffers_type =
            (const retrace::ArrayType *)func_type->parameter_types[1].type;

        uint32_t n = (uint32_t)call.arg(0).toUInt();
        for (uint32_t i = 0; i < n; i++) {
            fprintf(sequence_file, "    ptr = NULL;\n");
            fprintf(sequence_file, "    glGetNamedBufferPointervEXT(");
            print_value_expression(sequence_file, buffers_type->type, call,
                                   buffers->values[i]);
            fprintf(sequence_file, ", GL_BUFFER_MAP_POINTER, &ptr);\n");
            fprintf(sequence_file, "    delRegionByPointer(ptr);\n");
        }
    } else if (!strcmp("glClientWaitSync", call.name())) {
        fprintf(sequence_file, "    clientWaitSync(%u, ",
                (uint32_t)call.ret->toUInt());
        print_value_expression(sequence_file, func_type->parameter_types[0].type,
                               call, &call.arg(0));
        fprintf(sequence_file, ", %u, %llu);\n", (uint32_t)call.arg(1).toUInt(),
                call.arg(2).toUInt());
    }

    bool has_out_handle = false;
    if (func_type) {
        for (uint32_t i = 0; i < call.args.size(); i++) {
            const retrace::ArgType *arg_type = &func_type->parameter_types[i];
            if (!arg_type->output || !value_type_has_handle(arg_type->type))
                continue;

            assert(arg_type->type->kind == retrace::ValueTypeKind::array);
            const retrace::ArrayType *array_type =
                (const retrace::ArrayType *)arg_type->type;
            auto array = dynamic_cast<trace::Array *>(call.args[i].value);

            fprintf(sequence_file, "    %s out%u_%u[%lu];\n",
                    array_type->type->c_decl.c_str(), out_param_index, i,
                    array->values.size());
            has_out_handle = true;
        }
    }

    fprintf(sequence_file, "    ");

    if (func_type->return_type->kind == retrace::ValueTypeKind::handle && call.ret) {
        const retrace::HandleType *handle_type =
            (const retrace::HandleType *)func_type->return_type;
        print_set_handle(sequence_file, call, handle_type, call.ret->toSInt());
    } else if (func_type->return_type->kind == retrace::ValueTypeKind::linear_pointer && call.ret) {
        fprintf(sequence_file, "addRegion(%llu, ", call.ret->toUIntPtr());
    }

    fprintf(sequence_file, "%s(", call.name());

    for (uint32_t i = 0; i < call.args.size(); i++) {
        if (i)
            fprintf(sequence_file, ", ");

        const retrace::ArgType *arg_type = nullptr;
        if (func_type)
            arg_type = &func_type->parameter_types[i];

        if (arg_type->output && value_type_has_handle(arg_type->type)) {
            fprintf(sequence_file, "out%u_%u", out_param_index, i);
            continue;
        }

        print_value_expression(sequence_file, arg_type->type, call,
                               call.args[i].value);
    }

    if (func_type->return_type->kind == retrace::ValueTypeKind::linear_pointer && call.ret) {
        const retrace::LinearPointerType *pointer_type =
            (const retrace::LinearPointerType *)func_type->return_type;

        uint32_t size = 1;
        for (uint32_t i = 0; i < call.args.size(); i++) {
            if (pointer_type->size && !strcmp(pointer_type->size, call.sig->arg_names[i])) {
                size = call.args[i].value->toUInt();
                break;
            }
        }

        fprintf(sequence_file, "), %u);\n", size);
    } else if (func_type->return_type->kind == retrace::ValueTypeKind::handle &&
               call.ret) {
        fprintf(sequence_file, "));\n");
    } else {
        fprintf(sequence_file, ");\n");
    }

    if (has_out_handle) {
        for (uint32_t i = 0; i < call.args.size(); i++) {
            const retrace::ArgType *arg_type = &func_type->parameter_types[i];
            if (!arg_type->output || !value_type_has_handle(arg_type->type))
                continue;
          
            auto array = dynamic_cast<trace::Array *>(call.args[i].value);
            const retrace::ArrayType *array_type =
                (const retrace::ArrayType *)arg_type->type;
            const retrace::HandleType *handle_type =
                (const retrace::HandleType *)array_type->type;
            register_handle_map(handle_type);
          
            for (uint32_t j = 0; j < array->values.size(); j++) {
                print_set_handle(sequence_file, call, handle_type, array->values[j]->toUInt());
                fprintf(sequence_file, "out%u_%u[%u]);\n", out_param_index, i, j);
            }
        }
        out_param_index++;
    }

    /* GL specific handling. */

    if (!strcmp("glViewport", call.name())) {
        fprintf(
            sequence_file, "    resize_window(%u, %u);\n",
            (uint32_t)(call.args[0].value->toUInt() + call.args[2].value->toUInt()),
            (uint32_t)(call.args[1].value->toUInt() +
                       call.args[3].value->toUInt()));
    } else if (!strcmp("glViewportArrayv", call.name())) {
        uint32_t first = call.args[0].value->toUInt();
        uint32_t count = call.args[1].value->toUInt();
        const trace::Array *v = dynamic_cast<trace::Array *>(call.args[2].value);
        if (first == 0 && count > 0) {
            fprintf(sequence_file, "    resize_window(%u, %u);\n",
                    (uint32_t)(v->values[0]->toUInt() + v->values[2]->toUInt()),
                    (uint32_t)(v->values[1]->toUInt() + v->values[3]->toUInt()));
        }
    } else if (!strcmp("glViewportIndexedf", call.name())) {
        uint32_t index = call.args[0].value->toUInt();
        if (index == 0) {
            fprintf(sequence_file, "    resize_window(%u, %u);\n",
                    (uint32_t)(call.args[1].value->toFloat() +
                               call.args[3].value->toFloat()),
                    (uint32_t)(call.args[2].value->toFloat() +
                               call.args[4].value->toFloat()));
        } 
    } else if (!strcmp("glViewportIndexedfv", call.name())) {
        uint32_t index = call.args[0].value->toUInt();
        const trace::Array *v = dynamic_cast<trace::Array *>(call.args[1].value);
        if (index == 0) {
            fprintf(sequence_file, "    resize_window(%u, %u);\n",
                    (uint32_t)(v->values[1]->toFloat() + v->values[3]->toFloat()),
                    (uint32_t)(v->values[2]->toFloat() + v->values[4]->toFloat()));
        }
    } else if (!strcmp("glActiveShaderProgram", call.name())) {
        pipeline_active_programs[(GLint)call.arg(0).toUInt()] =
            (GLint)call.arg(1).toUInt();
    } else if (!strcmp("glBindProgramPipeline", call.name())) {
        active_pipeline = (GLint)call.arg(0).toUInt();
    } else if (!strcmp("glUseProgram", call.name())) {
        active_program = (GLint)call.arg(0).toUInt();
    }
}

static void
copy_file(const char *filename, const char *content) {
    FILE *file = fopen((target_directory / filename).c_str(), "w");
    fprintf(file, "%s", content);
    fclose(file);
}

void
glretrace::dump_c_start() {
    target_directory = retrace::Cpath;

    std::filesystem::create_directory(target_directory);

    copy_file("trace_model.hpp", trace_model_hpp);
    copy_file("trace_model.cpp", trace_model_cpp);
    copy_file("glproc.h", glproc_h);
    copy_file("glproc.cpp", glproc_cpp);
    copy_file("os.hpp", os_hpp);
    copy_file("os_posix.cpp", os_posix_cpp);
    copy_file("os_string.hpp", os_string_hpp);
    copy_file("os_backtrace.hpp", os_backtrace_hpp);
    copy_file("os_backtrace.cpp", os_backtrace_cpp);
    copy_file("eglimports.h", eglimports_h);
    copy_file("glimports.h", glimports_h);
    copy_file("retrace_library.hpp", retrace_library_hpp);
    copy_file("retrace_swizzle.cpp", retrace_swizzle_cpp);
    copy_file("retrace_swizzle.hpp", retrace_swizzle_hpp);

    copy_file("retrace.hpp", R"(
#pragma once

#include <cstdint>
#include <iostream>

#include "trace_model.hpp"

namespace retrace {
    static int debug = 0;
    static int verbosity = 0;

    std::ostream &warning(trace::Call &call) {
        std::cerr << "warning: ";
        return std::cerr;
    }
}
)");

    values_file = fopen((target_directory / "values.hpp").c_str(), "w");
    fprintf(values_file, "%s", R"(
#include "trace_model.hpp"

struct NamedValue {
    const char *name;
    trace::Value *value;
};

static uint32_t call_number = 0;

static trace::Call *
createCall(trace::Id sig_id, const char *name, trace::CallFlags flags, trace::Value *ret, const std::vector<NamedValue> &args) {
    trace::FunctionSig *sig = new trace::FunctionSig;
    sig->id = sig_id;
    sig->name = name;
    sig->num_args = args.size();
    sig->arg_names = new const char *[sig->num_args];
    for (uint32_t i = 0; i < sig->num_args; i++)
        sig->arg_names[i] = args[i].name;

    /* Replay is always single threaded */
    trace::Call *call = new trace::Call(sig, flags, 0);

    call->ret = ret;
    for (uint32_t i = 0; i < args.size(); i++)
        call->args[i] = {args[i].value};

    call->no = call_number++;

    return call;
}

static trace::Array *
createArray(const std::vector<trace::Value *> &values) {
    trace::Array *array = new trace::Array(values.size());

    for (uint32_t i = 0; i < values.size(); i++)
      array->values[i] = values[i];

    return array;
}

static trace::Struct *
createStruct(trace::Id sig_id, const char *name, const std::vector<NamedValue> &members) {
    trace::StructSig *sig = new trace::StructSig;
    sig->id = sig_id;
    sig->name = name;
    sig->num_members = members.size();
    sig->member_names = new const char *[sig->num_members];
    for (uint32_t i = 0; i < sig->num_members; i++)
        sig->member_names[i] = members[i].name;

    trace::Struct *s = new trace::Struct(sig);

    for (uint32_t i = 0; i < members.size(); i++)
        s->members[i] = members[i].value;

    return s;
}

static trace::Bitmask *
createBitmask(trace::Id sig_id, const std::vector<trace::BitmaskFlag> &flags,
              unsigned long long value) {
    trace::BitmaskSig *sig = new trace::BitmaskSig;
    sig->id = sig_id;
    sig->num_flags = flags.size();
    sig->flags = new trace::BitmaskFlag[sig->num_flags];
    trace::BitmaskFlag *flags_copy = new trace::BitmaskFlag[sig->num_flags];
    for (uint32_t i = 0; i < sig->num_flags; i++)
        flags_copy[i] = flags[i];
    sig->flags = flags_copy;

    return new trace::Bitmask(sig, value);
}

trace::Null null_value;
trace::Bool true_value(true);
trace::Bool false_value(false);
)");

    main_file = fopen((target_directory / "main.cpp").c_str(), "w");
    fprintf(main_file, "%s", R"(
#include "private.hpp"

replay_args args;

void *getData(long long unsigned offset)
{
    return (uint8_t *)args.data->data + offset;
}

void *
_getPublicProcAddress(const char *procName)
{
    return args.get_public_proc_addr(procName);
}

void *
_getPrivateProcAddress(const char *procName)
{
    return args.get_private_proc_addr(procName);
}

#include "values.hpp"

const struct replay_sequence sequences[] = {
)");

    state_file = fopen((target_directory / "state.cpp").c_str(), "w");
    fprintf(state_file, "%s", R"(
#include "private.hpp"

void
addRegion(unsigned long long address, void *buffer, unsigned long long size) {
    trace::FunctionSig sig{};
    trace::CallFlags flags = 0;
    trace::Call call(&sig, flags, 0);
    retrace::addRegion(call, address, buffer, size);
}

void
delRegionByPointer(void *ptr)
{
    if (ptr)
        retrace::delRegionByPointer(ptr);
}

void *
toPointer(uintptr_t address) {
    trace::Pointer value(address);
    return retrace::toPointer(value);
}

void
resize_window(int width, int height) {
    args.resize_window(width, height);
}

static GLenum
blockOnFence(GLsync sync, GLbitfield flags) {
    GLenum result;

    do {
        result = glClientWaitSync(sync, flags, 1000);
    } while (result == GL_TIMEOUT_EXPIRED);

    return result;
}

GLenum
clientWaitSync(GLenum result, GLsync sync, GLbitfield flags, GLuint64 timeout) {
    switch (result) {
    case GL_ALREADY_SIGNALED:
    case GL_CONDITION_SATISFIED:
        // We must block, as following calls might rely on the fence being
        // signaled
        result = blockOnFence(sync, flags);
        break;
    case GL_TIMEOUT_EXPIRED:
        result = glClientWaitSync(sync, flags, timeout);
        break;
    default:
        break;
    }
    return result;
}


)");

    sequence_h_file = fopen((target_directory / "sequence.h").c_str(), "w");
    fprintf(sequence_h_file, "%s", R"(
#pragma once

#include <string.h>

#define RETRACE
#include "glproc.h"

#ifdef __cplusplus
extern "C" {
#endif

void *getData(long long unsigned offset);

void addRegion(unsigned long long address, void *buffer, unsigned long long size);
void delRegionByPointer(void *ptr);
void *toPointer(uintptr_t address);

void resize_window(int width, int height);

GLenum clientWaitSync(GLenum result, GLsync sync, GLbitfield flags, GLuint64 timeout);

void mapUniformBlockName(GLuint program, GLint index, const char *name);

)");

    data_file = fopen((target_directory / "data.bin").c_str(), "wb");
    data_buffer = (uint8_t *)malloc(DATA_CHUNK_SIZE);
    compressed_data_buffer = malloc(snappy::MaxCompressedLength(DATA_CHUNK_SIZE));
}

void
glretrace::dump_c_end() {
    if (sequence_file) {
        fprintf(sequence_file, "}\n");
        fclose(sequence_file);
        fprintf(main_file, "    {sequence%u, nullptr, %llu},\n", sequence_index, data_offset);
        sequence_index++;
    }

    fclose(values_file);

    fprintf(main_file, "%s", R"(
};

extern "C" {

void
get_replay_sequences(const replay_sequence **out_sequences, uint32_t *out_sequence_count,
                     const replay_args *_args)
{
    args = *_args;

    *out_sequences = sequences;
    *out_sequence_count = sizeof(sequences) / sizeof(sequences[0]);
}

}

)");

    fclose(main_file);

    FILE *header_file = fopen((target_directory / "private.hpp").c_str(), "w");
    fprintf(header_file, "%s", R"(
#pragma once

#include <unordered_map>

#undef Bool
#include "retrace_library.hpp"

#include "sequence.h"

#include "retrace_swizzle.hpp"

)");

    handle_maps.insert("retrace::map<uintptr_t> pointer_map");
      
    for (const std::string &decl : handle_maps) {
      fprintf(header_file, "extern %s;\n", decl.c_str());
      fprintf(state_file, "%s;\n", decl.c_str());
    }
  
    for (uint32_t i = 0; i < sequence_index; i++)
      fprintf(sequence_h_file, "void sequence%u();\n", i);
  
    fclose(state_file);
    fclose(header_file);
  
    fprintf(sequence_h_file, "%s", R"(
#ifdef __cplusplus
}
#endif
)");
    fclose(sequence_h_file);

    FILE *meson_file = fopen((target_directory / "meson.build").c_str(), "w");
    fprintf(meson_file, "project('replay', 'cpp', 'c')\n");
    fprintf(meson_file, "replay_lib = shared_library(\n");
    fprintf(meson_file, "  'replay',\n");

    for (uint32_t i = 0; i < generated_filenames.size(); i++) {
      fprintf(meson_file, "  '%s'", generated_filenames[i].c_str());
      if (i + 1 < generated_filenames.size())
        fprintf(meson_file, ",");
      fprintf(meson_file, "\n");
    }

    fprintf(meson_file, ")\n");

    fprintf(meson_file,
            "fs = import('fs')\nfs.copyfile('data.bin', 'libreplay.so.data')\n");

    fclose(meson_file);

    if (data_offset % DATA_CHUNK_SIZE)
        flush_data_buffer(data_offset % DATA_CHUNK_SIZE);
    fclose(data_file);
    free(data_buffer);
    free(compressed_data_buffer);
}

static void
dump_malloc_as_c(trace::Call &call) {
    unsigned long long size = call.arg(0).toUInt();
    unsigned long long address = call.ret->toUIntPtr();

    if (address) {
        fprintf(sequence_file, "    addRegion(%llu, malloc(%llu), %llu);\n",
                address, size, size);
    }
}

static void
dump_memcpy_as_c(trace::Call &call) {
    unsigned long long size = call.arg(2).toUInt();
    if (size) {
        trace::Blob *src_blob = dynamic_cast<trace::Blob *>(&call.arg(1));
        if (src_blob) {
            fprintf(sequence_file,
                    "    memcpy(toPointer(%llu), getData(%llu), %llu);\n",
                    (long long unsigned)call.arg(0).toPointer(), get_blob_offset(src_blob->buf, src_blob->size), size);
        } else {
            fprintf(sequence_file,
                    "    memcpy(toPointer(%llu), toPointer(%llu), %llu);\n",
                    (long long unsigned)call.arg(0).toPointer(),
                    (long long unsigned)call.arg(1).toPointer(), size);
        }
    }
}

const retrace::Entry retrace::stdc_dump_as_c_callbacks[] = {
    {"malloc", &dump_malloc_as_c}, {"memcpy", &dump_memcpy_as_c}, {NULL, NULL}};
