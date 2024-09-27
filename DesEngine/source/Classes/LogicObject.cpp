//
// Created by Des Caldnd on 9/27/2024.
//
#include "Interfaces/LogicObject.hpp"

namespace DesEngine
{
    LogicObject::LogicObject(Scene *scene, id_t id) : Serializable(id), _scene(scene)
    {

    }
}