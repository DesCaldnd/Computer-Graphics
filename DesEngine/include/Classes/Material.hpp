//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_MATERIAL_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_MATERIAL_HPP_

#include <QOpenGLShaderProgram>
#include <QImage>
#include <QOpenGLTexture>
#include <memory>
#include <filesystem>


namespace DesEngine
{

	class Material
	{
        bool _use_diffuse_map = false;
        QVector3D _diffuse_color = QVector3D(1, 1, 1);
        std::unique_ptr<QOpenGLTexture> _diffuse_map;

        bool _use_ambient_map = false;
        QVector3D _ambient_color = QVector3D(1, 1, 1);
        std::unique_ptr<QOpenGLTexture> _ambient_map;

        bool _use_specular_map = false;
        QVector3D _specular_color = QVector3D(1, 1, 1);
        std::unique_ptr<QOpenGLTexture> _specular_map;

        bool _use_specexp_map = false;
        GLfloat _specexp_color = 10;
        std::unique_ptr<QOpenGLTexture> _specexp_map;

        bool _use_normal_map = false;
        std::unique_ptr<QOpenGLTexture> _normal_map;

        GLfloat _transparent = 1;

        /**
         * Kd - diffuse
         * Ka - ambience
         * Ns - specular exponent
         * Ks - specular
         * d - transparency
         * map_Bump - normal map
         */

        enum class file_param_type
        {
            comment, newmtl, Kd, map_Kd, Ka, map_Ka, Ns, map_Ns, Ks, map_Ks, d, map_Bump, undef
        };

        static file_param_type get_param_type(const std::string& str);

    public:

        static std::unique_ptr<QOpenGLTexture> load_texture(const std::string& path, const std::filesystem::path& parent_path);

        void set_program(std::shared_ptr<QOpenGLShaderProgram> prog);

        void release();

        static void parse_file_to_map(const std::filesystem::path& path, std::unordered_map<std::string, std::shared_ptr<Material>>& map);
	};

} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_MATERIAL_HPP_
