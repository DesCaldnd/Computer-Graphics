//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_LIGHTOBJECT_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_LIGHTOBJECT_HPP_

#include "Interfaces/LogicObject.hpp"
#include <QVector3D>
#include <QVector4D>

namespace DesEngine
{
    class Scene;

	class LightObject : public LogicObject
	{
    public:

        enum class LightType : GLuint
        {
            Directional = 1,
            Point = 2,
            Spot = 3
        };

    private:

        QVector3D _ambience_color;
        QVector3D _diffuse_color;
        QVector3D _specular_color;

        QVector3D _position;
        QQuaternion _rotation;
        QVector3D _scale;

        QMatrix4x4 _global_transform;

        float _cutoff;
        float _power;

        LightType type;

    public:


        LightObject();
	};

} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_LIGHTOBJECT_HPP_
