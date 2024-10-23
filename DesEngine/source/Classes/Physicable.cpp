//
// Created by Des Caldnd on 10/23/2024.
//

#include "Interfaces/Physicable.hpp"
#include <btBulletDynamicsCommon.h>

namespace DesEngine
{

    void Physicable::update()
    {
        if (_rb == nullptr)
            return;

        auto glob = get_global_transform();
        glob = glob.inverted();

        auto&& pos = _rb->getWorldTransform();

        auto origin = pos.getOrigin();
        auto rotation = pos.getRotation();

        QVector4D position(origin.x(), origin.y(), origin.z(), 1);

        position = glob * position;

        set_translate(position.toVector3D());

        float rx, ry, rz;
        rotation.getEulerZYX(rz, ry, rx);

        set_rotation_x(rx - prev_rx);
        set_rotation_y(ry - prev_ry);
        set_rotation_z(rz - prev_rz);

        prev_rx = rx;
        prev_ry = ry;
        prev_rz = rz;
    }

    void Physicable::init_rb()
    {

    }
}