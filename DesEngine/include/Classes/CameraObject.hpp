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

	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_CAMERAOBJECT_HPP_
