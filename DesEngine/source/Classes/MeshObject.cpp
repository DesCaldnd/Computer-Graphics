//
// Created by ivanl on 21.09.2024.
//

#include "Classes/MeshObject.hpp"

namespace DesEngine
{
    MeshObject::MeshObject(Scene * scene, id_t id, const std::string &path) : LogicObject(scene, id), _filepath(path)
    {
        //TODO: load mesh from file
    }
} // DesEngine