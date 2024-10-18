//
// Created by ivanl on 21.09.2024.
//

#include "Classes/LightObject.hpp"
#include "Classes/Utils.hpp"
#include "Classes/Scene.hpp"
#include "Widgets/glmainwindow.hpp"
#include "Widgets/glwidget.hpp"
#include <QOpenGLExtraFunctions>

namespace DesEngine
{
    LightObject::LightObject(Scene *scene, id_t id) : MeshObject(scene, id, scene->is_in_edit() ? "Primitives/sphere.obj" : "")
    {
        _cast_shadow = false;
        _draw_in_game = false;
        _depth_buffer = std::make_unique<QOpenGLFramebufferObject>(_scene->get_shadow_buffer_size(), QOpenGLFramebufferObject::Depth, GL_TEXTURE_2D);
    }

    QMatrix4x4 LightObject::get_light_projection_matrix()
    {
        QMatrix4x4 proj;
        proj.setToIdentity();
        if (_type == LightType::Directional)
            proj.ortho(-40, 40, -40, 40, -40, 40);
        else if (_type == LightType::Spot)
            proj.perspective(_cutoff, 1, 0.1, 1500);
        else
            proj.perspective(90, 1, 0.1, 1500);

        return proj;
    }

    QMatrix4x4 LightObject::get_light_matrix(unsigned int ind)
    {
        QMatrix4x4 light;

        light.setToIdentity();


        if (_type != LightType::Point)
        {
            if (_type == LightType::Spot)
                light.translate(_translate);
            else
                light.translate(get_translate());
            light *= get_rotation(_rot_x, _rot_y, _rot_z);
        } else
        {
            switch(ind)
            {
                case 0:
                    light.lookAt(_translate, _translate + QVector3D(1, 0, 0), QVector3D(0, -1, 0));
                    break;
                case 1:
                    light.lookAt(_translate, _translate + QVector3D(-1, 0, 0), QVector3D(0, -1, 0));
                    break;
                case 2:
                    light.lookAt(_translate, _translate + QVector3D(0, 1, 0), QVector3D(0, 0, 1));
                    break;
                case 3:
                    light.lookAt(_translate, _translate + QVector3D(0, -1, 0), QVector3D(0, 0, -1));
                    break;
                case 4:
                    light.lookAt(_translate, _translate + QVector3D(0, 0, 1), QVector3D(0, -1, 0));
                    break;
                case 5:
                    light.lookAt(_translate, _translate + QVector3D(0, 0, -1), QVector3D(0, -1, 0));
                    break;
            }
        }


        light.scale(_scale);
        light = _global_transform * light;

        return light;
    }

    QVector3D LightObject::get_diffuse_color() const
    {
        return _diffuse_color;
    }

    void LightObject::set_diffuse_color(const QVector3D &diffuseColor)
    {
        _diffuse_color = diffuseColor;
    }

    QVector3D LightObject::get_specular_color() const
    {
        return _specular_color;
    }

    void LightObject::set_specular_color(const QVector3D &specularColor)
    {
        _specular_color = specularColor;
    }

    float LightObject::get_cutoff() const
    {
        return _cutoff;
    }

    void LightObject::set_cutoff(float cutoff)
    {
        if (cutoff < 0)
            _cutoff = 0;
        else if (cutoff < 179)
            _cutoff = cutoff;
        else if (cutoff > 0)
            _cutoff = 179;

        if (_cutoff < _soft)
            _cutoff = _soft;
    }

    float LightObject::get_power() const
    {
        return _power;
    }

    void LightObject::set_power(float power)
    {
        _power = power;
    }

    LightObject::LightType LightObject::get_type() const
    {
        return _type;
    }

    void LightObject::set_type(LightObject::LightType type)
    {
        _type = type;
    }

    void LightObject::set_soft_cutoff(float cutoff)
    {
        if (cutoff < 0)
            _soft = 0;
        else if (cutoff < 179)
            _soft = cutoff;
        else if (cutoff > 0)
            _soft = 179;


        if (_soft > _cutoff)
            _soft = _cutoff;
    }

    float LightObject::get_soft_cutoff() const
    {
        return _soft;
    }

    QVector3D LightObject::get_translate() const
    {
        if (_type != LightType::Directional)
            return MeshObject::get_translate();
        else
        {
            QMatrix4x4 rot = get_rotation(_rot_x, _rot_y, _rot_z);

            return _scene->get_current_camera()->get_translate() + (rot * QVector4D(0, 0, 2, 1)).toVector3D();
        }
    }

    nlohmann::json LightObject::serialize() const
    {
        nlohmann::json res;
        res["mesh"] = MeshObject::serialize();

        res["diff_x"] = _diffuse_color.x();
        res["diff_y"] = _diffuse_color.y();
        res["diff_z"] = _diffuse_color.z();

        res["spec_x"] = _specular_color.x();
        res["spec_y"] = _specular_color.y();
        res["spec_z"] = _specular_color.z();

        res["cutoff"] = _cutoff;
        res["soft"] = _soft;
        res["power"] = _power;

        res["type"] = int(_type);

        return res;
    }

    std::string LightObject::get_class_name() const
    {
        return "Light";
    }

    std::shared_ptr<LogicObject> LightObject::default_light_object_json_loader(Scene *scene, id_t id, const nlohmann::json &js)
    {
        auto res = std::make_shared<LightObject>(scene, id);

        res->set_from_json(js["mesh"]);

        res->_diffuse_color.setX(js["diff_x"].get<float>());
        res->_diffuse_color.setY(js["diff_y"].get<float>());
        res->_diffuse_color.setZ(js["diff_z"].get<float>());

        res->_specular_color.setX(js["spec_x"].get<float>());
        res->_specular_color.setY(js["spec_y"].get<float>());
        res->_specular_color.setZ(js["spec_z"].get<float>());

        res->_cutoff = js["cutoff"].get<float>();
        res->_soft = js["soft"].get<float>();
        res->_power = js["power"].get<float>();

        res->_type = (LightType)js["type"].get<int>();

        return res;
    }

    std::shared_ptr<LogicObject> LightObject::default_light_object_dialog_loader(Scene *scene, id_t id)
    {
        auto obj = std::make_shared<LightObject>(scene, id);

        return obj;
    }

    std::vector<property_t> LightObject::get_properties()
    {
        auto res = light_props;
        auto mesh = MeshObject::get_properties();
        
        res.reserve(res.size() + mesh.size());
        
        for (auto&& p : mesh)
        {
            res.push_back(std::move(p));
        }
        
        return res;
    }


    const std::function<property_data_t(Editable*)> LightObject::getter_type = [](Editable* e) -> property_data_t {
        return (int)static_cast<LightObject*>(e)->get_type();
    };
    const std::function<bool(Editable*, property_data_t)> LightObject::setter_type = [](Editable* e, property_data_t data) -> bool {
        auto ind = std::get<int>(data);

        if (ind < 1 || ind > 3)
            return false;

        static_cast<LightObject*>(e)->set_type((LightType)ind);

        return true;
    };

    const std::function<property_data_t(Editable*)> LightObject::getter_diffuse_color_r = [](Editable* e) -> property_data_t {
        return static_cast<LightObject*>(e)->get_diffuse_color().x();
    };
    const std::function<bool(Editable*, property_data_t)> LightObject::setter_diffuse_color_r = [](Editable* e, property_data_t data) -> bool {
        QVector3D color = static_cast<LightObject*>(e)->get_diffuse_color();
        
        static_cast<LightObject*>(e)->set_diffuse_color(QVector3D(std::get<float>(data), color.y(), color.z()));
        
        return true;
    };

    const std::function<property_data_t(Editable*)> LightObject::getter_diffuse_color_g = [](Editable* e) -> property_data_t {
        return static_cast<LightObject*>(e)->get_diffuse_color().y();
    };
    const std::function<bool(Editable*, property_data_t)> LightObject::setter_diffuse_color_g = [](Editable* e, property_data_t data) -> bool {
        QVector3D color = static_cast<LightObject*>(e)->get_diffuse_color();

        static_cast<LightObject*>(e)->set_diffuse_color(QVector3D(color.x(), std::get<float>(data), color.z()));

        return true;
    };

    const std::function<property_data_t(Editable*)> LightObject::getter_diffuse_color_b = [](Editable* e) -> property_data_t {
        return static_cast<LightObject*>(e)->get_diffuse_color().z();
    };
    const std::function<bool(Editable*, property_data_t)> LightObject::setter_diffuse_color_b = [](Editable* e, property_data_t data) -> bool {
        QVector3D color = static_cast<LightObject*>(e)->get_diffuse_color();

        static_cast<LightObject*>(e)->set_diffuse_color(QVector3D(color.x(), color.y(), std::get<float>(data)));

        return true;
    };
    

    const std::function<property_data_t(Editable*)> LightObject::getter_specular_color_r = [](Editable* e) -> property_data_t {
        return static_cast<LightObject*>(e)->get_specular_color().x();
    };
    const std::function<bool(Editable*, property_data_t)> LightObject::setter_specular_color_r = [](Editable* e, property_data_t data) -> bool {
        QVector3D color = static_cast<LightObject*>(e)->get_specular_color();

        static_cast<LightObject*>(e)->set_specular_color(QVector3D(std::get<float>(data), color.y(), color.z()));

        return true;
    };

    const std::function<property_data_t(Editable*)> LightObject::getter_specular_color_g = [](Editable* e) -> property_data_t {
        return static_cast<LightObject*>(e)->get_specular_color().y();
    };
    const std::function<bool(Editable*, property_data_t)> LightObject::setter_specular_color_g = [](Editable* e, property_data_t data) -> bool {
        QVector3D color = static_cast<LightObject*>(e)->get_specular_color();

        static_cast<LightObject*>(e)->set_specular_color(QVector3D(color.x(), std::get<float>(data), color.z()));

        return true;
    };

    const std::function<property_data_t(Editable*)> LightObject::getter_specular_color_b = [](Editable* e) -> property_data_t {
        return static_cast<LightObject*>(e)->get_specular_color().z();
    };
    const std::function<bool(Editable*, property_data_t)> LightObject::setter_specular_color_b = [](Editable* e, property_data_t data) -> bool {
        QVector3D color = static_cast<LightObject*>(e)->get_specular_color();

        static_cast<LightObject*>(e)->set_specular_color(QVector3D(color.x(), color.y(), std::get<float>(data)));

        return true;
    };


    const std::function<property_data_t(Editable*)> LightObject::getter_power = [](Editable* e) -> property_data_t {
        return static_cast<LightObject*>(e)->get_power();
    };
    const std::function<bool(Editable*, property_data_t)> LightObject::setter_power = [](Editable* e, property_data_t data) -> bool {
        static_cast<LightObject*>(e)->set_power(std::get<float>(data));

        return true;
    };

    const std::function<property_data_t(Editable*)> LightObject::getter_cutoff_angle = [](Editable* e) -> property_data_t {
        return static_cast<LightObject*>(e)->get_cutoff();
    };
    const std::function<bool(Editable*, property_data_t)> LightObject::setter_cutoff_angle = [](Editable* e, property_data_t data) -> bool {
        static_cast<LightObject*>(e)->set_cutoff(std::get<float>(data));

        return true;
    };

    const std::function<property_data_t(Editable*)> LightObject::getter_soft_angle = [](Editable* e) -> property_data_t {
        return static_cast<LightObject*>(e)->get_soft_cutoff();
    };
    const std::function<bool(Editable*, property_data_t)> LightObject::setter_soft_angle = [](Editable* e, property_data_t data) -> bool {
        static_cast<LightObject*>(e)->set_soft_cutoff(std::get<float>(data));

        return true;
    };
    
    const std::vector<property_t> LightObject::light_props = {
            {property_data_type_t::INT, "Light type", getter_type, setter_type},
            {property_data_type_t::FLOAT, "Diffuse color r", getter_diffuse_color_r, setter_diffuse_color_r},
            {property_data_type_t::FLOAT, "Diffuse color g", getter_diffuse_color_g, setter_diffuse_color_g},
            {property_data_type_t::FLOAT, "Diffuse color b", getter_diffuse_color_b, setter_diffuse_color_b},
            {property_data_type_t::FLOAT, "Specular color r", getter_specular_color_r, setter_specular_color_r},
            {property_data_type_t::FLOAT, "Specular color g", getter_specular_color_g, setter_specular_color_g},
            {property_data_type_t::FLOAT, "Specular color b", getter_specular_color_b, setter_specular_color_b},
            {property_data_type_t::FLOAT, "Power", getter_power, setter_power},
            {property_data_type_t::FLOAT, "Cutoff angle", getter_cutoff_angle, setter_cutoff_angle},
            {property_data_type_t::FLOAT, "Soft cutoff angle", getter_soft_angle, setter_soft_angle}
    };

} // DesEngine