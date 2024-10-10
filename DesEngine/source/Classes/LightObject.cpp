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
        if (cutoff < 179)
            _cutoff = cutoff;
        else if (cutoff > 0)
            _cutoff = 179;

        if (_cutoff < _soft)
            _soft = _cutoff;
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
        if (_type == LightType::Point && type != LightType::Point)
        {
            _depth_buffer = std::make_unique<QOpenGLFramebufferObject>(_scene->get_shadow_buffer_size(), QOpenGLFramebufferObject::Depth, GL_TEXTURE_2D);
        } else if (_type != LightType::Point && type == LightType::Point)
        {
//            _depth_buffer = std::make_unique<QOpenGLFramebufferObject>(_scene->get_shadow_buffer_size(), QOpenGLFramebufferObject::Depth, GL_TEXTURE_CUBE_MAP);
            _depth_buffer.reset(nullptr);

            auto size = _scene->get_shadow_buffer_size();
            QOpenGLExtraFunctions funcs;
            funcs.initializeOpenGLFunctions();

            funcs.glGenTextures(1, &m_shadowMapTex);
            funcs.glBindTexture(GL_TEXTURE_2D, m_shadowMapTex);
            funcs.glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT,
                         SHADOW_WIDTH, SHADOW_HEIGHT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
            GLfloat borderColor[] = { 1.0, 1.0, 1.0, 1.0 };
            funcs.glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

            // Create a frame-buffer and associate the texture with it.
            funcs.glGenFramebuffers(1, &m_shadowMapFBO);
            funcs.glBindFramebuffer(GL_FRAMEBUFFER, m_shadowMapFBO);
            funcs.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowMapTex, 0);

            // Let OpenGL know that we are not interested in colors for this buffer
            funcs.glDrawBuffer(GL_NONE);
            funcs.glReadBuffer(GL_NONE);

            // Cleanup for now.
            funcs.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            funcs.glBindTexture(GL_TEXTURE_2D, 0);
        }

        _type = type;
    }

    void LightObject::set_soft_cutoff(float cutoff)
    {
        if (cutoff < 179)
            _soft = cutoff;
        else if (cutoff > 0)
            _soft = 179;

        if (_soft > _cutoff)
            _cutoff = _soft;
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

} // DesEngine