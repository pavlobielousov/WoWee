#pragma once
// What a GL renderer needs from the frame (VITA-18): the camera, the sun and the fog. One struct, built by Renderer, so the
// individual renderers (terrain, later M2, WMO, water, sky) take the same thing.
#include <glm/glm.hpp>

namespace wowee::rendering::gl {

struct SceneParams {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};  // OpenGL clip space: [-1, 1] depth. NOT the camera's Vulkan matrix (see DEV_SETUP section 22).
    glm::mat4 cullViewProj{1.0f};  // the camera's own (Vulkan-convention) view-projection, which rendering::Frustum expects
    glm::vec3 eye{0.0f};
    glm::vec3 lightDir{0.0f, 0.0f, -1.0f};   // the direction the light travels, as upstream's lightDir
    glm::vec3 lightColor{0.8f};
    glm::vec3 ambient{0.3f};
    glm::vec3 fogColor{0.6f, 0.7f, 0.85f};
    float fogStart = 300.0f;
    float fogEnd = 1000.0f;
    float viewDistance = 1000.0f;
};

}  // namespace wowee::rendering::gl
