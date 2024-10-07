//
// Created by ivanl on 21.09.2024.
//

#include "Classes/CameraObject.hpp"
#include "Classes/Scene.hpp"
#include "Widgets/glwidget.hpp"
#include "Widgets/glmainwindow.hpp"
#include <QKeyEvent>
#include <QShortcut>
#include <QCoreApplication>
#include <QApplication>

DesEngine::CameraObject::CameraObject(DesEngine::Scene *scene, DesEngine::id_t id) : MeshObject(scene, id, "")
{

}

void DesEngine::CameraObject::set_look_at(const QVector3D &look_at)
{
    // TODO: set look at
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

//    model = model.inverted();

    QVector4D eye(0, 0, 0, 1);
    QVector4D lookat(0, 1, 0, 0);

    eye = model * eye;
    lookat = model * lookat;

    lookat += eye;

    QMatrix4x4 view;
    view.setToIdentity();
    view.lookAt(eye.toVector3D(), lookat.toVector3D(), QVector3D(0, 0, 1));

    return view;
}

QMatrix4x4 DesEngine::CameraObject::get_projection_matrix() const
{
    QMatrix4x4 proj;

    proj.setToIdentity();

    if (!is_orthogonal)
        proj.perspective(_angle, _aspect_ratio, _near_plane, _far_plane);
    else
        proj.ortho(-1 * _aspect_ratio, 1 * _aspect_ratio, -1, 1, _near_plane, _far_plane);

    return proj;
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

void DesEngine::FlyingCamera::event_loop(double seconds)
{
    QMatrix4x4 model;

    model.setToIdentity();
    model.rotate(_rotation);
    model = _global_transform * model;

    QVector3D forward = _direction;
//    auto z = forward.z();
//    auto y = forward.y();
//    forward.setY(-z);
//    forward.setZ(y);
    forward.normalize();
    forward *= _speed;
    forward *= seconds;
    forward = (model * QVector4D(forward, 0)).toVector3D();

    translate(forward);
}

DesEngine::FlyingCamera::FlyingCamera(DesEngine::Scene *scene, DesEngine::id_t id) : CameraObject(scene, id)
{
    connect(_scene->get_parent()->glwidget, &GLWidget::keyPressSignal, this, &FlyingCamera::keyPressEvent);
    connect(_scene->get_parent()->glwidget, &GLWidget::keyReleaseSignal, this, &FlyingCamera::keyReleaseEvent);
    connect(_scene->get_parent()->glwidget, &GLWidget::wheelSignal, this, &FlyingCamera::wheelEvent);
    connect(_scene->get_parent()->glwidget, &GLWidget::mouseMoveSignal, this, &FlyingCamera::mouseMoveEvent);
    connect(_scene->get_parent()->glwidget, &GLWidget::mousePressSignal, this, &FlyingCamera::mousePressEvent);

    QPointF globalCursorPos = QCursor::pos();
    QPointF local_cursor_pos = _scene->get_parent()->glwidget->mapFromGlobal(globalCursorPos);
    _mouse_position.setX(local_cursor_pos.x());
    _mouse_position.setY(local_cursor_pos.y());
}

void DesEngine::FlyingCamera::control_button_press(bool activated, int axis, bool direction)
{
    float diff = 1;
    if (!activated)
        diff *= -1;

    if (!direction)
        diff *= -1;

    switch(axis)
    {
        case 0:
            _direction.setX(_direction.x() + diff);
            break;
        case 1:
            _direction.setY(_direction.y() + diff);
            break;
        case 2:
            _direction.setZ(_direction.z() + diff);
            break;
    }
}


void DesEngine::FlyingCamera::keyReleaseEvent(::QKeyEvent *event)
{
    if (!event->isAutoRepeat())
    {
        if (event->key() == Qt::Key_W)
            control_button_press(false, 1, true);
        if (event->key() == Qt::Key_D)
            control_button_press(false, 0, true);
        if (event->key() == Qt::Key_E)
            control_button_press(false, 2, true);
        if (event->key() == Qt::Key_S)
            control_button_press(false, 1, false);
        if (event->key() == Qt::Key_A)
            control_button_press(false, 0, false);
        if (event->key() == Qt::Key_Q)
            control_button_press(false, 2, false);
        if (event->key() == Qt::Key_F)
            _direction = QVector3D(0, 0, 0);
    }
}

void DesEngine::FlyingCamera::keyPressEvent(::QKeyEvent *event)
{
    if (!event->isAutoRepeat())
    {
        if (event->key() == Qt::Key_W)
            control_button_press(true, 1, true);
        if (event->key() == Qt::Key_D)
            control_button_press(true, 0, true);
        if (event->key() == Qt::Key_E)
            control_button_press(true, 2, true);
        if (event->key() == Qt::Key_S)
            control_button_press(true, 1, false);
        if (event->key() == Qt::Key_A)
            control_button_press(true, 0, false);
        if (event->key() == Qt::Key_Q)
            control_button_press(true, 2, false);
        if (event->key() == Qt::Key_F5)
            set_is_orthogonal(!get_is_orthogonal());
        if (event->key() == Qt::Key_Escape)
        {
//            _scene->get_parent()->glwidget->setCursor(Qt::CursorShape::ArrowCursor);
//            _scene->get_parent()->glwidget->releaseKeyboard();
//            _scene->get_parent()->glwidget->releaseMouse();
        }
    }
}

void DesEngine::FlyingCamera::wheelEvent(::QWheelEvent *event)
{
    auto degree = event->angleDelta().y();

    if (degree > 0)
    {
        _speed *= 1.25 * (degree / 240.0f + 1);
    } else if (degree < 0)
    {
        _speed /= 1.25 * (-degree / 240.0f + 1);
    }
}

void DesEngine::FlyingCamera::mouseMoveEvent(::QMouseEvent *event)
{
    if (event->buttons() != Qt::LeftButton)
        return;

    QVector2D diff = QVector2D(event->position()) - _mouse_position;
    _mouse_position = QVector2D(event->position());

    float angle = diff.length() / 2.0;

    rotate_x(diff.x() / 2);
    rotate_y(diff.y() / 2);
}

void DesEngine::FlyingCamera::mousePressEvent(::QMouseEvent *event)
{
    if (event->buttons() == Qt::LeftButton)
    {
        _mouse_position = QVector2D(event->position());
//        _scene->get_parent()->glwidget->setCursor(Qt::CursorShape::BlankCursor);
//        _scene->get_parent()->glwidget->grabKeyboard();
//        _scene->get_parent()->glwidget->grabMouse();
    }
}

void DesEngine::FlyingCamera::rotate_x(float angle)
{
    QQuaternion quat = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), -angle);
    _rotate_x = quat * _rotate_x;
    _rotation = _rotate_x * _rotate_y;
}

void DesEngine::FlyingCamera::rotate_y(float angle)
{
    QQuaternion quat = QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), -angle);
    _rotate_y = quat * _rotate_y;
    _rotation = _rotate_x * _rotate_y;
}
