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

        rotate_x(rx - prev_rx);
        rotate_y(ry - prev_ry);
        rotate_z(rz - prev_rz);

        prev_rx = rx;
        prev_ry = ry;
        prev_rz = rz;
    }

    void Physicable::init_rb(std::unique_ptr<btCollisionShape>, float)
    {
//        prev_rx = get_rotation_x();
//        prev_ry = get_rotation_y();
//        prev_rz = get_rotation_z();
    }

    btRigidBody *Physicable::get_rb()
    {
        return _rb.get();
    }

    void Physicable::rb_deleter::operator()(btRigidBody *rb) const
    {
        if (rb && rb->getMotionState())
            delete rb->getMotionState();
        delete rb;
    }
}