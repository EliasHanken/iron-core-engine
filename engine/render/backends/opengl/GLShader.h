#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cstdint>
#include <string>

namespace iron {

// Compiles and links a GLSL vertex + fragment shader into a GL program.
// On failure, isValid() is false and the compile/link error is logged.
class GLShader {
public:
    GLShader(const std::string& vertexSrc, const std::string& fragmentSrc);
    ~GLShader();

    GLShader(const GLShader&) = delete;
    GLShader& operator=(const GLShader&) = delete;

    bool isValid() const { return program_ != 0; }
    void bind() const;

    void setMat4(const char* name, const Mat4& m) const;
    void setInt(const char* name, int value) const;
    void setFloat(const char* name, float value) const;
    void setVec3(const char* name, Vec3 v) const;
    void setVec2(const char* name, Vec2 v) const;

private:
    std::uint32_t program_ = 0;
};

} // namespace iron
