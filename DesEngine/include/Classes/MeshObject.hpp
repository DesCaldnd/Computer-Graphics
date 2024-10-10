//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_MESHOBJECT_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_MESHOBJECT_HPP_

#include "../Interfaces/LogicObject.hpp"
#include "Material.hpp"
#include <string>
#include <filesystem>
#include <QOpenGLBuffer>
#include <vector>

namespace DesEngine
{

    class Scene;
    class MeshObject;

	class MeshSubObject
	{

    public:

        friend MeshObject;

        std::shared_ptr<Material> _mat;

        QOpenGLBuffer _vert_buffer;
        QOpenGLBuffer _index_buffer;
        size_t count;

        MeshSubObject(Scene* scene, const std::filesystem::path& mat_file, std::ifstream& stream, std::vector<QVector3D>::iterator coord_it, std::vector<QVector2D>::iterator uv_it, std::vector<QVector3D>::iterator normal_it, std::string&);
	};

	class MeshObject : public LogicObject
	{
	private:

		std::string _filepath = "";

        enum class file_param_type
        {
            comment, mtllib, vertex, uv, normal, f, usemtl, unknown
        };

        static file_param_type get_param_type(const std::string& val);

    protected:

        std::vector<MeshSubObject> _subs;

        float _rot_x = 0, _rot_y = 0, _rot_z = 0;
        QVector3D _translate;
        QVector3D _scale;
        bool _cast_shadow = true, _draw_in_game = true;

        QMatrix4x4 _global_transform;

        /**
         * Sets all except file
         */
        void set_from_json(const nlohmann::json& js);


	public:

        friend MeshSubObject;

        MeshObject(Scene*, id_t, const std::string& path);

        static std::shared_ptr<LogicObject> default_mesh_object_json_loader(Scene*, id_t, const nlohmann::json&);
        static std::shared_ptr<LogicObject> default_mesh_object_dialog_loader(Scene*, id_t);

        void draw(QOpenGLFunctions& funcs) override;
        void help_draw(std::function<void(LogicObject*)> uniform_values_loader, QOpenGLFunctions& funcs) override;

        bool cast_shadow() override;
        void set_cast_shadow(bool s);

        bool draw_in_game() override;

        void set_draw_in_game(bool d) override;

        std::vector<property_t> get_properties() override;

        void rotate_x(float quat) override;

        void set_rotation_x(float quat) override;

        float get_rotation_x() const override;

        void rotate_y(float quat) override;

        void set_rotation_y(float quat) override;

        float get_rotation_y() const override;

        void rotate_z(float quat) override;

        void set_rotation_z(float quat) override;

        float get_rotation_z() const override;

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

        void event_loop(double seconds) override;
    };

} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_MESHOBJECT_HPP_
