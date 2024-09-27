//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_BASICTYPES_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_BASICTYPES_HPP_

#include <QVector3D>

namespace DesEngine
{

	struct basic_point
	{
		float x, y, z;
	};

	struct vertex
	{
		QVector3D point;
        QVector2D uv; 
		QVector3D normal;

        vertex(const QVector3D& coord, const QVector2D& uvs, const QVector3D& normals) : point(coord), uv(uvs), normal(normals){}
	};

	struct bounding_box_t
	{
        // TODO: object aligned cube
		basic_point center;
		float half_side;

	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_BASICTYPES_HPP_
