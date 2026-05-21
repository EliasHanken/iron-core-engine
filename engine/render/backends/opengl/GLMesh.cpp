#include "render/backends/opengl/GLMesh.h"

#include <glad/gl.h>

#include <cstddef>
#include <utility>

namespace iron {

GLMesh::GLMesh(const MeshData& data)
    : indexCount_(static_cast<std::int32_t>(data.indices.size())) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.vertices.size() * sizeof(Vertex)),
                 data.vertices.data(), GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.indices.size() * sizeof(std::uint32_t)),
                 data.indices.data(), GL_DYNAMIC_DRAW);

    // Vertex layout matches struct Vertex: position, normal, uv.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, uv)));

    glBindVertexArray(0);
}

void GLMesh::update(const MeshData& data) {
    indexCount_ = static_cast<std::int32_t>(data.indices.size());

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.vertices.size() * sizeof(Vertex)),
                 data.vertices.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.indices.size() * sizeof(std::uint32_t)),
                 data.indices.data(), GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
}

void GLMesh::release() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (ebo_) glDeleteBuffers(1, &ebo_);
    vao_ = vbo_ = ebo_ = 0;
    indexCount_ = 0;
}

GLMesh::~GLMesh() {
    release();
}

GLMesh::GLMesh(GLMesh&& other) noexcept
    : vao_(other.vao_), vbo_(other.vbo_), ebo_(other.ebo_),
      indexCount_(other.indexCount_) {
    other.vao_ = other.vbo_ = other.ebo_ = 0;
    other.indexCount_ = 0;
}

GLMesh& GLMesh::operator=(GLMesh&& other) noexcept {
    if (this != &other) {
        release();
        vao_ = other.vao_;
        vbo_ = other.vbo_;
        ebo_ = other.ebo_;
        indexCount_ = other.indexCount_;
        other.vao_ = other.vbo_ = other.ebo_ = 0;
        other.indexCount_ = 0;
    }
    return *this;
}

void GLMesh::draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

} // namespace iron
