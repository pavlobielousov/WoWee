#include "rendering/gl/scene_target.hpp"

#include "core/logger.hpp"
#include "rendering/gl/gl_program.hpp"
#include "rendering/gl/shader_sources.hpp"

#include <algorithm>

namespace wowee::rendering::gl {

bool SceneTarget::initialize(int screenW, int screenH, float scale) {
    shutdown();
    if (scale >= 0.999f) return true;  // full size: draw straight to the screen
    scale = std::max(scale, 0.3f);
    screenW_ = screenW;
    screenH_ = screenH;
    w_ = std::max(64, static_cast<int>(screenW * scale) & ~1);
    h_ = std::max(64, static_cast<int>(screenH * scale) & ~1);

    glGenTextures(1, &color_);
    glBindTexture(GL_TEXTURE_2D, color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w_, h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
    glGenRenderbuffers(1, &depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w_, h_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Scene target ", w_, "x", h_, " is not complete (0x", std::hex, status, "), drawing at full size");
        shutdown();
        return false;
    }

    program_ = linkProgram(blitProgram());
    if (program_ == 0) {
        LOG_ERROR("Scene blit program did not link, drawing at full size");
        shutdown();
        return false;
    }
    uTexture_ = glGetUniformLocation(program_, "uTexture");
    // Triangle strip over the whole screen: x, y, u, v.
    const float quad[16] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    LOG_WARNING("Scene target ", w_, "x", h_, " for a ", screenW_, "x", screenH_, " screen");
    return true;
}

void SceneTarget::begin() {
    if (!fbo_) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w_, h_);
    glDepthMask(GL_TRUE);
}

void SceneTarget::end() {
    if (!fbo_) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screenW_, screenH_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(program_);
    glUniform1i(uTexture_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, color_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<const void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<const void*>(8));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

void SceneTarget::shutdown() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (program_) glDeleteProgram(program_);
    if (depth_) glDeleteRenderbuffers(1, &depth_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_) glDeleteTextures(1, &color_);
    vbo_ = program_ = depth_ = fbo_ = color_ = 0;
}

}  // namespace wowee::rendering::gl
