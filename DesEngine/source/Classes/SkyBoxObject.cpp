//
// Created by Des Caldnd on 9/23/2024.
//

#include "Classes/SkyBoxObject.hpp"
#include "Classes/Scene.hpp"
#include "Classes/Utils.hpp"

namespace DesEngine
{
    void SkyBoxObject::draw(QOpenGLFunctions &funcs)
    {
        _scene->get_current_prog()->release();
        _prog->bind();

        QMatrix4x4 model;

        model.setToIdentity();
        model.translate(_scene->get_current_camera()->get_translate());
        model.translate(_translate);
        model *= get_rotation(_rot_x, _rot_y, _rot_z);
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
        _prog = _scene->load_program("Shaders/skybox", false);
        MeshObject::scale(QVector3D(500, 500, 500));
    }

    std::vector<property_t> SkyBoxObject::get_properties()
    {
        return {};
    }

    std::string SkyBoxObject::get_class_name() const
    {
        return "SkyBox";
    }

    bool SkyBoxObject::cast_shadow()
    {
        return false;
    }

    std::shared_ptr<LogicObject> SkyBoxObject::default_skybox_object_dialog_loader(Scene *, id_t)
    {
        //TODO:
        return std::shared_ptr<LogicObject>();
    }

    std::shared_ptr<LogicObject> SkyBoxObject::default_skybox_object_json_loader(Scene *scene, id_t id, const nlohmann::json &js)
    {
        auto res = std::make_shared<SkyBoxObject>(scene, id);

        res->set_from_json(js);

        return res;
    }
} // DesEngine