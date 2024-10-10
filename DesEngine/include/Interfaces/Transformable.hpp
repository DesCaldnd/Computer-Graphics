//
// Created by ivanl on 21.09.2024.
//


#ifndef DESENGINE_DESENGINE_INCLUDE_INTERFACES_TRANSFORMABLE_HPP_
#define DESENGINE_DESENGINE_INCLUDE_INTERFACES_TRANSFORMABLE_HPP_

#include <QMatrix4x4>
#include <QVector3D>
#include <QQuaternion>

namespace DesEngine
{

	class Transformable
	{
	public:

		virtual void rotate_x(float quat) =0;
		virtual void set_rotation_x(float quat) =0;
		virtual float get_rotation_x() const =0;

        virtual void rotate_y(float quat) =0;
        virtual void set_rotation_y(float quat) =0;
        virtual float get_rotation_y() const =0;

        virtual void rotate_z(float quat) =0;
        virtual void set_rotation_z(float quat) =0;
        virtual float get_rotation_z() const =0;

		virtual void scale(const QVector3D& vec) =0;
		virtual void set_scale(const QVector3D& vec) =0;
		virtual QVector3D get_scale() const =0;

		virtual void translate(const QVector3D& vec) =0;
		virtual void set_translate(const QVector3D& vec) =0;
		virtual QVector3D get_translate() const =0;

		virtual void set_global_transform(const QMatrix4x4& mat) =0;
		virtual QMatrix4x4 get_global_transform() const =0;

		virtual ~Transformable() =default;
	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_INTERFACES_TRANSFORMABLE_HPP_
