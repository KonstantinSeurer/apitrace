
#include "glproc.h"
#include "glretrace.hpp"
#include "retrace.hpp"
#include "retrace_codegen.hpp"
#include "retrace_swizzle.hpp"

#include "eglimports_h.h"
#include "glimports_h.h"
#include "glproc_cpp.h"
#include "glproc_h.h"

#include "md5.h"

#include <snappy.h>

#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

static std::unordered_set<std::string> ignore_calls = {
    "wglDescribePixelFormat",
    "glDebugMessageCallback",
};

class GLCodegen : public retrace::Codegen {
public:
    GLCodegen(const std::string &output_dir, const std::string &trace_name)
        : Codegen(output_dir, trace_name, glretrace::gl_func_types)
    {
    }

    virtual uint64_t get_handle_default_value(const char *name) override;

    void emit_gl_call(trace::Call &call);

private:
    GLint active_program = 0;
    GLint active_pipeline = 0;
    GLint active_pack_buffer = 0;
    std::unordered_map<GLint, GLint> pipeline_active_programs;

    std::unordered_map<uint32_t, uintptr_t> current_context;
};

uint64_t
GLCodegen::get_handle_default_value(const char *name) {
    if (strstr(name, "getCurrentContext")) {
        return current_context.at(thread_id);
    } else if (!strcmp("program", name)) {
        uint64_t program = active_program;

        if (active_pipeline && pipeline_active_programs.find(active_pipeline) != pipeline_active_programs.end())
            program = pipeline_active_programs.at(active_pipeline);

        return program;
    } else if (!strcmp("programObj", name)) {
        return active_program;
    }
    return 0;
}

void
GLCodegen::emit_gl_call(trace::Call &call) {
    if (ignore_calls.find(call.name()) != ignore_calls.end())
        return;

    bool is_active_uniform_block_name = !strcmp("glGetActiveUniformBlockName", call.name());

    if ((call.flags & trace::CALL_FLAG_NO_SIDE_EFFECTS) && !is_active_uniform_block_name)
        return;

    if (!strcmp("CGLSetCurrentContext", call.name())) {
        if (call.ret->toBool())
            current_context[thread_id] = call.arg(0).toUIntPtr();
    } else if (!strcmp("eglMakeCurrent", call.name())) {
        if (call.ret->toBool())
            current_context[thread_id] = call.arg(3).toUIntPtr();
    } else if (!strcmp("glXMakeCurrent", call.name())) {
        if (call.ret->toBool())
            current_context[thread_id] = call.arg(2).toUIntPtr();
    } else if (!strcmp("glXMakeContextCurrent", call.name())) {
        if (call.ret->toBool())
            current_context[thread_id] = call.arg(3).toUIntPtr();
    } else if (!strcmp("wglMakeCurrent", call.name())) {
        current_context[thread_id] = 0;
        if (call.ret->toBool())
            current_context[thread_id] = call.arg(1).toUIntPtr();
    } else if (!strcmp("wglMakeContextCurrentARB", call.name())) {
        current_context[thread_id] = 0;
        if (call.ret->toBool())
            current_context[thread_id] = call.arg(2).toUIntPtr();
    } else if (!strcmp("wglShareLists", call.name())) {
        if (call.ret->toBool())
            current_context[thread_id] = call.arg(0).toUIntPtr();
    }

    bool new_wsi_sequence =
        !strncmp("glX", call.name(), 3) || !strncmp("wgl", call.name(), 3);
    if (new_wsi_sequence) {
        end_sequence();
        emit_constructed_call(call);
        return;
    }

    begin_sequence(call.thread_id);

    const retrace::FunctionType *func_type = nullptr;
    if (function_types.find(call.name()) != function_types.end())
        func_type = &function_types.at(call.name());

    if (is_active_uniform_block_name) {
        emit_set_handle(call, (const retrace::HandleType *)func_type->parameter_types[1].type, call.arg(1).toSInt());
        sequence_c << "mapUniformBlockName(";
        emit_get_handle(call, (const retrace::HandleType *)func_type->parameter_types[0].type, call.arg(0).toSInt());
        sequence_c << ", " << call.arg(1).toUInt() << "llu, \"" << call.arg(4).toString() << "\");\n";
        return;
    }

    if (!strcmp("glUnmapBuffer", call.name())) {
        sequence_c << "    ptr = NULL;\n";
        sequence_c << "    glGetBufferPointerv(" << call.arg(0).toUInt() << ", GL_BUFFER_MAP_POINTER, &ptr);";
        sequence_c << "    delRegionByPointer(ptr);\n";
    } else if (!strcmp("glUnmapBufferARB", call.name())) {
        sequence_c << "    ptr = NULL;\n";
        sequence_c << "    glGetBufferPointervARB(" << call.arg(0).toUInt() << ", GL_BUFFER_MAP_POINTER_ARB, &ptr);",
        sequence_c << "    delRegionByPointer(ptr);\n";
    } else if (!strcmp("glUnmapBufferOES", call.name())) {
        sequence_c << "    ptr = NULL;\n";
        sequence_c << "    glGetBufferPointervOES(" << call.arg(0).toUInt() << ", GL_BUFFER_MAP_POINTER_OES, &ptr);";
        sequence_c << "    delRegionByPointer(ptr);\n";
    } else if (!strcmp("glUnmapNamedBuffer", call.name())) {
        sequence_c << "    ptr = NULL;\n";
        sequence_c << "    glGetNamedBufferPointerv(";
        emit_value_expression(call, func_type->parameter_types[0].type, &call.arg(0));
        sequence_c << ", GL_BUFFER_MAP_POINTER, &ptr);\n";
        sequence_c << "    delRegionByPointer(ptr);\n";
    } else if (!strcmp("glUnmapNamedBufferEXT", call.name())) {
        sequence_c << "    ptr = NULL;\n";
        sequence_c << "    glGetNamedBufferPointervEXT(";
        emit_value_expression(call, func_type->parameter_types[0].type, &call.arg(0));
        sequence_c << ", GL_BUFFER_MAP_POINTER, &ptr);\n";
        sequence_c << "    delRegionByPointer(ptr);\n";
    } else if (!strcmp("glDeleteBuffers", call.name()) ||
               !strcmp("glDeleteBuffersARB", call.name())) {
        const trace::Array *buffers = (const trace::Array *)&call.arg(1);

        assert(func_type->parameter_types[1].type->kind ==
               retrace::ValueTypeKind::array);
        const retrace::ArrayType *buffers_type =
            (const retrace::ArrayType *)func_type->parameter_types[1].type;

        uint32_t n = (uint32_t)call.arg(0).toUInt();
        for (uint32_t i = 0; i < n; i++) {
            sequence_c << "    ptr = NULL;\n";
            sequence_c << "    glGetNamedBufferPointervEXT(";
            emit_value_expression(call, buffers_type->type, buffers->values[i]);
            sequence_c << ", GL_BUFFER_MAP_POINTER, &ptr);\n";
            sequence_c << "    delRegionByPointer(ptr);\n";
        }
    } else if (!strcmp("glClientWaitSync", call.name())) {
        sequence_c << "    clientWaitSync(" << call.ret->toUInt() << ", ";
        emit_value_expression(call, func_type->parameter_types[0].type, &call.arg(0));
        sequence_c << ", " << call.arg(1).toUInt() << ", " << call.arg(2).toUInt() << "llu);\n";
    }

    bool has_out_pointer = false;
    if (func_type) {
        for (uint32_t i = 0; i < call.args.size(); i++) {
            const retrace::ArgType *arg_type = &func_type->parameter_types[i];
            if (arg_type->output && (arg_type->type->kind == retrace::ValueTypeKind::pointer ||
                                     arg_type->type->kind == retrace::ValueTypeKind::opaque))
                has_out_pointer = true;
        }
    }

    std::unordered_map<std::string, std::string> arg_overrides;
    bool free_ptr = false;
    if (has_out_pointer && !active_pack_buffer) {
        if (!strcmp("glGetTexnImage", call.name())) {
            sequence_c << "    ptr = malloc(" << call.arg(4).toUInt() << "llu);\n";
            arg_overrides["pixels"] = "ptr";
            free_ptr = true;
        } else if (!strcmp("glGetTextureImage", call.name())) {
            sequence_c << "    ptr = malloc(" << call.arg(4).toUInt() << "llu);\n";
            arg_overrides["pixels"] = "ptr";
            free_ptr = true;
        } else if (!strcmp("glReadPixels", call.name())) {
            sequence_c << "    ptr = malloc(" << call.arg(2).toSInt() * call.arg(3).toSInt() * 64 << "llu);\n";
            arg_overrides["pixels"] = "ptr";
            free_ptr = true;
        } else if (!strcmp("glReadnPixels", call.name())) {
            sequence_c << "    ptr = malloc(" << call.arg(6).toSInt() << "llu);\n";
            arg_overrides["data"] = "ptr";
            free_ptr = true;
        } else {
            std::cout << "warning: Skipping call " << call.name() << " because." << std::endl;
            return;
        }
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

            sequence_c << "    " << array_type->type->c_decl << " out" << out_param_index << "_" << i
                       << "[" << array->values.size() << "];\n";
            has_out_handle = true;
        }
    }

    sequence_c << "    ";

    if (func_type->return_type->kind == retrace::ValueTypeKind::handle && call.ret) {
        const retrace::HandleType *handle_type =
            (const retrace::HandleType *)func_type->return_type;
        emit_set_handle(call, handle_type, call.ret->toSInt());
    } else if (func_type->return_type->kind == retrace::ValueTypeKind::linear_pointer && call.ret) {
        sequence_c << "addRegion(" << call.ret->toUIntPtr() << "llu, ";
    }

    emit_call(call, arg_overrides);

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

        sequence_c << "), " << size << ");\n";
    } else if (func_type->return_type->kind == retrace::ValueTypeKind::handle &&
               call.ret) {
        sequence_c << ");\n";
    } else {
        sequence_c << ");\n";
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

            for (uint32_t j = 0; j < array->values.size(); j++) {
                emit_set_handle(call, handle_type, array->values[j]->toUInt());
                sequence_c << "out" << out_param_index << "_" << i << "[" << j << "];\n";
            }
        }
        out_param_index++;
    }

    if (free_ptr)
        sequence_c << "    free(ptr);\n";

    /* GL specific handling. */

    if (!strcmp("glViewport", call.name())) {
        sequence_c << "    resize_window(" << (call.args[0].value->toUInt() + call.args[2].value->toUInt())
                   << ", " << (call.args[1].value->toUInt() + call.args[3].value->toUInt()) << ");\n";
    } else if (!strcmp("glViewportArrayv", call.name())) {
        uint32_t first = call.args[0].value->toUInt();
        uint32_t count = call.args[1].value->toUInt();
        const trace::Array *v = dynamic_cast<trace::Array *>(call.args[2].value);
        if (first == 0 && count > 0) {
            sequence_c << "    resize_window(" << (v->values[0]->toUInt() + v->values[2]->toUInt())
                       << ", " << (v->values[1]->toUInt() + v->values[3]->toUInt()) << ");\n";
        }
    } else if (!strcmp("glViewportIndexedf", call.name())) {
        uint32_t index = call.args[0].value->toUInt();
        if (index == 0) {
            sequence_c <<  "    resize_window(" << (uint32_t)(call.args[1].value->toFloat() + call.args[3].value->toFloat())
                       << ", " << (uint32_t)(call.args[2].value->toFloat() + call.args[4].value->toFloat()) << ");\n";
        } 
    } else if (!strcmp("glViewportIndexedfv", call.name())) {
        uint32_t index = call.args[0].value->toUInt();
        const trace::Array *v = dynamic_cast<trace::Array *>(call.args[1].value);
        if (index == 0) {
            sequence_c << "    resize_window(" << (uint32_t)(v->values[0]->toFloat() + v->values[2]->toFloat())
                       << ", " << (uint32_t)(v->values[1]->toFloat() + v->values[3]->toFloat()) << ");\n";
        }
    } else if (!strcmp("glActiveShaderProgram", call.name())) {
        pipeline_active_programs[(GLint)call.arg(0).toUInt()] =
            (GLint)call.arg(1).toUInt();
    } else if (!strcmp("glBindProgramPipeline", call.name())) {
        active_pipeline = (GLint)call.arg(0).toUInt();
    } else if (!strcmp("glUseProgram", call.name()) || !strcmp("glUseProgramObjectARB", call.name())) {
        active_program = (GLint)call.arg(0).toUInt();
    } else if (!strcmp("glBindBuffer", call.name())) {
        if (call.arg(0).toSInt() == GL_PIXEL_PACK_BUFFER)
            active_program = (GLint)call.arg(1).toUInt();
    }
};

GLCodegen *codegen = nullptr;

void
glretrace::codegen_start() {
    std::string trace_name = std::filesystem::path(retrace::trace_filename).stem();

    codegen = new GLCodegen(retrace::Cpath, trace_name);
    codegen->begin_trace();

    codegen->copy_file("glproc.h", glproc_h);
    codegen->copy_file("glproc.cpp", glproc_cpp);
    codegen->source_filenames.push_back("glproc.cpp");
    codegen->copy_file("eglimports.h", eglimports_h);
    codegen->copy_file("glimports.h", glimports_h);

    codegen->sequence_h << R"(
GLenum clientWaitSync(GLenum result, GLsync sync, GLbitfield flags, GLuint64 timeout);

GLint mapUniformBlockName(GLuint program, GLint index, const char *name);
)";

    codegen->state_cpp << R"(
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

GLint
mapUniformBlockName(GLuint program, GLint index, const char *name) {
    GLint num_blocks = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &num_blocks);
    for (int i = 0; i < num_blocks; i++) {
        GLint buf_len;
        glGetActiveUniformBlockiv(program, i, GL_UNIFORM_BLOCK_NAME_LENGTH, &buf_len);
        std::vector<char> name_buf(buf_len + 1);
        GLint length;
        glGetActiveUniformBlockName(program, i, name_buf.size(), &length, name_buf.data());
        name_buf[length] = '\0';
        if (!strcmp(name, name_buf.data()))
            return i;
    }
    return index;
} 
)";
}

void
glretrace::call_codegen(trace::Call &call) {
    codegen->emit_gl_call(call);
}

void
glretrace::codegen_end() {
    codegen->end_trace();
    delete codegen;
    codegen = nullptr;
}

static void
malloc_codegen(trace::Call &call) {
    codegen->emit_malloc(call);
}

static void
memcpy_codegen(trace::Call &call) {
    codegen->emit_memcpy(call);
}

const retrace::Entry retrace::stdc_codegen_callbacks[] = {
    {"malloc", &malloc_codegen}, {"memcpy", &memcpy_codegen}, {NULL, NULL}};
