//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_LIGHTOBJECT_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_LIGHTOBJECT_HPP_

#include "Classes/MeshObject.hpp"
#include <QVector3D>
#include <QVector4D>
#include <QOpenGLFramebufferObject>

namespace DesEngine
{
    class Scene;

	class LightObject final : public MeshObject
	{
    public:

        enum class LightType : GLuint
        {
            Directional = 1,
            Point = 2,
            Spot = 3
        };

    public:

        static std::shared_ptr<LogicObject> default_light_object_json_loader(Scene*, id_t, const nlohmann::json&);
        static std::shared_ptr<LogicObject> default_light_object_dialog_loader(Scene*, id_t);

        QVector3D get_diffuse_color() const;

        void set_diffuse_color(const QVector3D &diffuseColor);

        QVector3D get_specular_color() const;

        void set_specular_color(const QVector3D &specularColor);

        float get_cutoff() const;

        void set_cutoff(float cutoff);

        float get_soft_cutoff() const;

        nlohmann::json serialize() const override;

        void set_soft_cutoff(float cutoff);

        float get_power() const;

        void set_power(float power);

        LightType get_type() const;

        std::string get_class_name() const override;

        void set_type(LightType type);

        QVector3D get_translate() const override;

    private:

        friend class Scene;

        std::unique_ptr<QOpenGLFramebufferObject> _depth_buffer;

        QVector3D _diffuse_color = QVector3D(1, 1, 1);
        QVector3D _specular_color = QVector3D(1, 1, 1);

        float _cutoff = 45;
        float _soft = 30;
        float _power = 1;

        LightType _type = LightType::Directional;

    public:

        LightObject(Scene* scene, id_t id);

        QMatrix4x4 get_light_projection_matrix();
        QMatrix4x4 get_light_matrix(unsigned int ind = 0);


	};

} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_LIGHTOBJECT_HPP_
