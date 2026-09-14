#include "scene/Camera.h"

namespace space {

void Camera::rotate(float yaw, float pitch, float roll) {
    glm::quat qYaw = glm::angleAxis(yaw, glm::vec3(0.f, 1.f, 0.f));
    glm::quat qPitch = glm::angleAxis(pitch, glm::vec3(1.f, 0.f, 0.f));
    glm::quat qRoll = glm::angleAxis(roll, glm::vec3(0.f, 0.f, -1.f));
    orientation = glm::normalize(orientation * qYaw * qPitch * qRoll);
}

void Camera::translateLocal(const glm::dvec3& delta) {
    position += glm::dvec3(right()) * delta.x + glm::dvec3(up()) * delta.y + glm::dvec3(forward()) * -delta.z;
}

} // namespace space
