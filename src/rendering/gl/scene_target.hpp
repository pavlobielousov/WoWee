#pragma once
// The 3D scene at a fraction of the screen size (VITA-19): the renderers draw into an off-screen colour + depth target and
// the result is scaled up to the screen, where the interface then draws at full resolution. Measured on the device, the
// fragment work of the full-size scene was the larger half of the frame (half the pixels took 51 ms to 32 ms).
#include <vitaGL.h>

namespace wowee::rendering::gl {

class SceneTarget {
public:
    /// scale in (0.3, 1]; 1 disables the target (begin/end do nothing).
    bool initialize(int screenW, int screenH, float scale);
    [[nodiscard]] bool active() const { return fbo_ != 0; }
    void begin();   ///< bind the target, set the viewport, clear colour and depth
    void end();     ///< unbind, draw the target to the screen
    void shutdown();
    [[nodiscard]] int width() const { return w_; }
    [[nodiscard]] int height() const { return h_; }

private:
    GLuint fbo_ = 0, color_ = 0, depth_ = 0, program_ = 0, vbo_ = 0;
    GLint uTexture_ = -1;
    int w_ = 0, h_ = 0, screenW_ = 0, screenH_ = 0;
};

}  // namespace wowee::rendering::gl
