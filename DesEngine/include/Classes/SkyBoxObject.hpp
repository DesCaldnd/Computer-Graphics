//
// Created by Des Caldnd on 9/23/2024.
//

#ifndef DESENGINE_SKYBOXOBJECT_HPP
#define DESENGINE_SKYBOXOBJECT_HPP

#include "../Classes/MeshObject.hpp"

namespace DesEngine
{

    class SkyBoxObject : public MeshObject
    {
        std::shared_ptr<QOpenGLShaderProgram> _prog;

    public:

        SkyBoxObject(Scene* scene, id_t id);

        void draw(QOpenGLFunctions &funcs) override;

        std::vector<property_t> get_properties() override;

        std::string get_class_name() const override;

        bool cast_shadow() override;
    };

} // DesEngine

#endif //DESENGINE_SKYBOXOBJECT_HPP
