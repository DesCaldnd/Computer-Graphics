//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_CAMERAOBJECT_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_CAMERAOBJECT_HPP_

#include "MeshObject.hpp"

namespace DesEngine
{

	class CameraObject : public MeshObject
	{
	private:

		float _angle, _near_plane, _far_plane;
		float _aspect_ratio;
		bool is_orthogonal;

		QMatrix4x4 local_transform, global_transform;

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

}

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_CAMERAOBJECT_HPP_
