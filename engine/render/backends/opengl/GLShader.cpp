#include "render/backends/opengl/GLShader.h"

#include "core/Log.h"

#include <glad/gl.h>

namespace iron {

namespace {
// Compiles one shader stage; returns 0 and logs on failure.
GLuint compileStage(GLenum type, const std::string& src) {
    const GLuint shader = glCreateShader(type);
    const char* cstr = src.c_str();
    glShaderSource(shader, 1, &cstr, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        Log::error("GLShader: %s stage compile failed: %s",
                   type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
} // namespace

GLShader::GLShader(const std::string& vertexSrc, const std::string& fragmentSrc) {
    const GLuint vs = compileStage(GL_VERTEX_SHADER, vertexSrc);
    const GLuint fs = compileStage(GL_FRAGMENT_SHADER, fragmentSrc);
    if (vs == 0 || fs == 0) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    GLint ok = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        Log::error("GLShader: link failed: %s", log);
        glDeleteProgram(program_);
        program_ = 0;
    }

    // The stage objects are no longer needed once linked (or once link
    // failed) — the program keeps its own linked copy. Release them on
    // both paths.
    glDeleteShader(vs);
    glDeleteShader(fs);
}

GLShader::~GLShader() {
    if (program_) {
        glDeleteProgram(program_);
    }
}

void GLShader::bind() const {
    glUseProgram(program_);
}

// Setting a uniform on an invalid (unlinked) program is a logic error that
// GL would silently ignore. Guard so it stays an obvious no-op rather than a
// confusing "the matrix didn't upload" mystery.

void GLShader::setMat4(const char* name, const Mat4& m) const {
    if (!program_) return;
    const GLint loc = glGetUniformLocation(program_, name);
    // Mat4 is column-major already, so transpose = GL_FALSE.
    glUniformMatrix4fv(loc, 1, GL_FALSE, m.m);
}

void GLShader::setInt(const char* name, int value) const {
    if (!program_) return;
    glUniform1i(glGetUniformLocation(program_, name), value);
}

void GLShader::setVec3(const char* name, Vec3 v) const {
    if (!program_) return;
    glUniform3f(glGetUniformLocation(program_, name), v.x, v.y, v.z);
}

} // namespace iron
