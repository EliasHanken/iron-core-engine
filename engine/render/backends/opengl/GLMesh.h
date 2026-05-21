#pragma once

#include "scene/Mesh.h"

#include <cstdint>

namespace iron {

// Uploads a MeshData to the GPU as a vertex array object (VAO) + vertex buffer
// (VBO) + index buffer (EBO). Owns those GL objects; frees them on destruction.
class GLMesh {
public:
    explicit GLMesh(const MeshData& data);
    ~GLMesh();

    GLMesh(const GLMesh&) = delete;
    GLMesh& operator=(const GLMesh&) = delete;
    GLMesh(GLMesh&& other) noexcept;
    GLMesh& operator=(GLMesh&& other) noexcept;

    void draw() const;

private:
    void release();

    std::uint32_t vao_ = 0;
    std::uint32_t vbo_ = 0;
    std::uint32_t ebo_ = 0;
    std::int32_t indexCount_ = 0;
};

} // namespace iron
