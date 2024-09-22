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
		float x, y, z;
		float u, v;
		QVector3D normal;
	};

	struct bounding_box_t
	{
		basic_point center;
		float half_side;

	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_BASICTYPES_HPP_
