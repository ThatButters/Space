#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace space {

// Free-flying 6-DOF camera. Position is double precision so we can travel from planetary
// surfaces to intergalactic distances; orientation is a quaternion (no gimbal lock, no "up").
class Camera {
public:
    glm::dvec3 position{0.0, 0.0, 0.0};
    glm::quat orientation{1.f, 0.f, 0.f, 0.f};
    float fovY = glm::radians(70.f);
    double speed = 1.0; // world units per second

    glm::vec3 forward() const { return orientation * glm::vec3(0.f, 0.f, -1.f); }
    glm::vec3 right() const { return orientation * glm::vec3(1.f, 0.f, 0.f); }
    glm::vec3 up() const { return orientation * glm::vec3(0.f, 1.f, 0.f); }

    // Local-frame rotation deltas in radians.
    void rotate(float yaw, float pitch, float roll);
    void translateLocal(const glm::dvec3& delta);
};

} // namespace space
