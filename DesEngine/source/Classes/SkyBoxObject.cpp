//
// Created by Des Caldnd on 9/23/2024.
//

#include "Classes/SkyBoxObject.hpp"
#include "Classes/Scene.hpp"

namespace DesEngine
{
    void SkyBoxObject::draw(QOpenGLFunctions &funcs)
    {
        _scene->get_current_prog()->release();
        _prog->bind();

        QMatrix4x4 model;

        model.setToIdentity();
        model.translate(_translate);
        model.rotate(_rotation);
        model.rotate(_rotation);
        model.scale(_scale);
        model = _global_transform * model;

        _prog->setUniformValue("model", model);
        _prog->setUniformValue("view", _scene->get_current_camera()->get_look_matrix());
        _prog->setUniformValue("proj", _scene->get_current_camera()->get_projection_matrix());

        auto&& subobj = _subs[0];

        if (!subobj._vert_buffer.isCreated() || !subobj._index_buffer.isCreated())
            throw std::runtime_error("Mesh object buffer didn't created");

        subobj._vert_buffer.bind();

        int offset = 0;

        int vertloc = _prog->attributeLocation("a_position");
        _prog->enableAttributeArray(vertloc);
        _prog->setAttributeBuffer(vertloc, GL_FLOAT, offset, 3, sizeof(vertex));

        offset += sizeof(QVector3D);

        int uvloc = _prog->attributeLocation("a_uv");
        _prog->enableAttributeArray(uvloc);
        _prog->setAttributeBuffer(uvloc, GL_FLOAT, offset, 2, sizeof(vertex));

        subobj._index_buffer.bind();

        subobj._mat->set_program(_prog);

        funcs.glDrawElements(GL_TRIANGLES, subobj.count, GL_UNSIGNED_INT, 0);

        subobj._vert_buffer.release();
        subobj._index_buffer.release();
        subobj._mat->release();

        _prog->release();
        _scene->get_current_prog()->bind();
    }

    SkyBoxObject::SkyBoxObject(Scene *scene, id_t id) : MeshObject(scene, id, "Primitives/inner_cube.obj")
    {
        _prog = _scene->load_program("Shaders/skybox.vsh", "Shaders/skybox.fsh");
        MeshObject::scale(QVector3D(500, 500, 500));
    }

    std::vector<property_t> SkyBoxObject::get_properties()
    {
        return MeshObject::get_properties();
    }

    std::string SkyBoxObject::get_class_name() const
    {
        return "SkyBox";
    }
} // DesEngine