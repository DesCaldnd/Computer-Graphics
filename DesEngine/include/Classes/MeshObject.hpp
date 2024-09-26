//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_MESHOBJECT_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_MESHOBJECT_HPP_

#include "../Interfaces/LogicObject.hpp"
#include "Material.hpp"
#include <string>
#include <vector>

namespace DesEngine
{

    class Scene;

	class MeshSubObject : public LogicObject
	{
		std::shared_ptr<Material> _mat;
	};

	class MeshObject : public LogicObject
	{
	private:

		std::string _filepath = "";
		std::vector<MeshSubObject> _subs;

	public:

        MeshObject(Scene*, id_t, const std::string& path);

        static std::shared_ptr<LogicObject> default_mesh_object_json_loader(Scene*, id_t, nlohmann::json);
        static std::shared_ptr<LogicObject> default_mesh_object_dialog_loader(Scene*, id_t);

        void draw(QOpenGLFunctions& funcs) override;
        void help_draw(std::function<void(LogicObject*)> uniform_values_loader) override;
        std::vector<property_t> get_properties() override;

        void rotate(const QQuaternion& quat) override;
        void set_rotation(const QQuaternion& quat) override;
        QQuaternion get_rotation() const override;

        void scale(const QVector3D& vec) override;
        void set_scale(const QVector3D& vec) override;
        QVector3D get_scale() const override;

        void translate(const QVector3D& vec) override;
        void set_translate(const QVector3D& vec) override;
        QVector3D get_translate() const override;

        void set_global_transform(const QMatrix4x4& mat) override;
        QMatrix4x4 get_global_transform() const override;

        std::string get_class_name() const override;
        nlohmann::json serialize() const override;
	};

} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_MESHOBJECT_HPP_
