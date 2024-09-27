//
// Created by ivanl on 21.09.2024.
//

#include "Classes/CameraObject.hpp"

DesEngine::CameraObject::CameraObject(DesEngine::Scene *scene, DesEngine::id_t id) : MeshObject(scene, id, "")
{

}

void DesEngine::CameraObject::set_look_at(const QVector3D &look_at)
{

}

QVector3D DesEngine::CameraObject::get_look_at() const
{
    // TODO: lookat
    return QVector3D();
}

float DesEngine::CameraObject::get_angle() const
{
    return _angle;
}

void DesEngine::CameraObject::set_angle(float angle)
{
    _angle = angle;
}

float DesEngine::CameraObject::get_near_plane() const
{
    return _near_plane;
}

void DesEngine::CameraObject::set_near_plane(float near_plane)
{
    _near_plane = near_plane;
}

float DesEngine::CameraObject::get_far_plane() const
{
    return _far_plane;
}

void DesEngine::CameraObject::set_far_plane(float far_plane)
{
    _far_plane = far_plane;
}

bool DesEngine::CameraObject::get_is_orthogonal() const
{
    return is_orthogonal;
}

void DesEngine::CameraObject::set_is_orthogonal(bool orth)
{
    is_orthogonal = orth;
}

void DesEngine::CameraObject::set_aspect_ratio(float aspect_ratio)
{
    _aspect_ratio = aspect_ratio;
}

QMatrix4x4 DesEngine::CameraObject::get_look_matrix() const
{
    QMatrix4x4 model;

    model.setToIdentity();
    model.translate( _translate);
    model.rotate(_rotation);
    model.scale(_scale);
    model = _global_transform * model;

    return model.inverted();
}

QMatrix4x4 DesEngine::CameraObject::get_projection_matrix() const
{
    // TODO: make orthogonal
    QMatrix4x4 proj;

    proj.setToIdentity();
    proj.perspective(_angle, _aspect_ratio, _near_plane, _far_plane);

    return proj;
}

void DesEngine::CameraObject::draw(QOpenGLFunctions &funcs)
{
}

void DesEngine::CameraObject::help_draw(std::function<void(LogicObject * )> uniform_values_loader)
{
    MeshObject::help_draw(uniform_values_loader);
}

std::vector<DesEngine::property_t> DesEngine::CameraObject::get_properties()
{
    return MeshObject::get_properties();
}

void DesEngine::CameraObject::rotate(const QQuaternion &quat)
{
    MeshObject::rotate(quat);
}

void DesEngine::CameraObject::set_rotation(const QQuaternion &quat)
{
    MeshObject::set_rotation(quat);
}

QQuaternion DesEngine::CameraObject::get_rotation() const
{
    return MeshObject::get_rotation();
}

void DesEngine::CameraObject::scale(const QVector3D &vec)
{
    MeshObject::scale(vec);
}

void DesEngine::CameraObject::set_scale(const QVector3D &vec)
{
    MeshObject::set_scale(vec);
}

QVector3D DesEngine::CameraObject::get_scale() const
{
    return MeshObject::get_scale();
}

void DesEngine::CameraObject::translate(const QVector3D &vec)
{
    MeshObject::translate(vec);
}

void DesEngine::CameraObject::set_translate(const QVector3D &vec)
{
    MeshObject::set_translate(vec);
}

QVector3D DesEngine::CameraObject::get_translate() const
{
    return MeshObject::get_translate();
}

void DesEngine::CameraObject::set_global_transform(const QMatrix4x4 &mat)
{
    MeshObject::set_global_transform(mat);
}

QMatrix4x4 DesEngine::CameraObject::get_global_transform() const
{
    return MeshObject::get_global_transform();
}

std::string DesEngine::CameraObject::get_class_name() const
{
    return "Camera";
}

nlohmann::json DesEngine::CameraObject::serialize() const
{
    return MeshObject::serialize();
}

std::shared_ptr<DesEngine::LogicObject>
DesEngine::CameraObject::default_camera_object_json_loader(DesEngine::Scene *, DesEngine::id_t, nlohmann::json)
{
    return std::shared_ptr<LogicObject>();
}

std::shared_ptr<DesEngine::LogicObject>
DesEngine::CameraObject::default_camera_object_dialog_loader(DesEngine::Scene *, DesEngine::id_t)
{
    return std::shared_ptr<LogicObject>();
}
