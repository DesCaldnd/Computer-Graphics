//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_CAMERAOBJECT_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_CAMERAOBJECT_HPP_

#include "MeshObject.hpp"

class QKeyEvent;
class QWheelEvent;
class QMouseEvent;

namespace DesEngine
{


	class CameraObject : public MeshObject
	{
	private:

		float _angle = 85, _near_plane = 0.1, _far_plane = 1000;
		float _aspect_ratio;
		bool is_orthogonal = false;

//		QMatrix4x4 local_transform, global_transform;

	public:

        CameraObject(Scene*, id_t );

		void set_look_at(const QVector3D& look_at);
		QVector3D get_look_at() const;

		float get_angle() const;
		void set_angle(float angle);

		float get_near_plane() const;
		void set_near_plane(float near_plane);

		float get_far_plane() const;
		void set_far_plane(float far_plane);

		bool get_is_orthogonal() const;
		void set_is_orthogonal(bool orth);

		void set_aspect_ratio(float aspect_ratio);

		QMatrix4x4 get_look_matrix() const;
		QMatrix4x4 get_projection_matrix() const;


        void draw(QOpenGLFunctions& funcs) override;
        void help_draw(std::function<void(LogicObject*)> uniform_values_loader) override;
        std::vector<property_t> get_properties() override;

        void rotate(const QQuaternion& quat) override;
        void set_rotation(const QQuaternion& quat) override;
        QQuaternion get_rotation() const override;

        void scale(const QVector3D& vec) override;
        void set_scale(const QVector3D& vec) override;
        QVector3D get_scale() const override;

        void translate(const QVector3D& vec) override;
        void set_translate(const QVector3D& vec) override;
        QVector3D get_translate() const override;

        void set_global_transform(const QMatrix4x4& mat) override;
        QMatrix4x4 get_global_transform() const override;

        std::string get_class_name() const override;
        nlohmann::json serialize() const override;


        static std::shared_ptr<LogicObject> default_camera_object_json_loader(Scene*, id_t, nlohmann::json);
        static std::shared_ptr<LogicObject> default_camera_object_dialog_loader(Scene*, id_t);
	};

    class FlyingCamera : public CameraObject
    {
        Q_OBJECT

    private:

        void control_button_press(bool activated, int axis, bool direction);

    private slots:

        void keyPressEvent(::QKeyEvent* event);
        void keyReleaseEvent(::QKeyEvent* event);

        void wheelEvent(::QWheelEvent* event);
        void mouseMoveEvent(::QMouseEvent *event);
        void mousePressEvent(::QMouseEvent *event);

    public:

        FlyingCamera(Scene* scene, id_t id);

        void event_loop(double seconds) override;

        void rotate_x(float angle);
        void rotate_y(float angle);

    protected:

        QVector3D _direction = QVector3D(0, 0, 0);
        float _speed = 1;

        QVector2D _mouse_position;

        QQuaternion _rotate_x, _rotate_y;


    };
}

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_CAMERAOBJECT_HPP_
