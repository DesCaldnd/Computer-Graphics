//
// Created by ivanl on 21.09.2024.
//

#include "Classes/Material.hpp"
#include <fstream>
#include "Classes/Utils.hpp"

namespace DesEngine
{

    void Material::set_program(std::shared_ptr<QOpenGLShaderProgram> prog)
    {
        //TODO: fully set material to program
        prog->setUniformValue("u_use_diffuse_map", _use_diffuse_map);
        if (_use_diffuse_map)
        {
            auto diff_loc = prog->uniformLocation("u_diffuse_map");
            _diffuse_map->bind(diff_loc);
            prog->setUniformValue("u_diffuse_map", diff_loc);
        } else
        {
            prog->setUniformValue("u_diffuse_color", _diffuse_color);
        }
    }

    void Material::release()
    {
        if (_use_diffuse_map)
            _diffuse_map->release();
    }

    void Material::parse_file_to_map(const std::filesystem::path& path, std::unordered_map<std::string, std::shared_ptr<Material>> &map)
    {
        std::ifstream stream(path);

        if (!stream.is_open())
            throw std::runtime_error("Attempt of parsing materials from file \"" + path.string() + "\", but it could not be opened");

        std::shared_ptr<Material> mat;

        while(!stream.eof())
        {
            std::string str;
            std::getline(stream, str);
            auto lst = split(str, " ");

            if (lst.empty())
                continue;

            switch (get_param_type(lst[0]))
            {

                case file_param_type::comment:
                    qDebug() << "Comment: " << str.c_str();
                    break;
                case file_param_type::newmtl:
                    mat = std::make_shared<Material>();
                    map.insert(std::make_pair(lst[1], mat));
                    break;
                case file_param_type::Kd:
                    mat->_diffuse_color = QVector3D(std::stof(lst[1]), std::stof(lst[2]), std::stof(lst[3]));
                    break;
                case file_param_type::map_Kd:
                    mat->_diffuse_map = load_texture(str.substr(str.find(lst[1])), path);
                    mat->_use_diffuse_map = true;
                    break;
                case file_param_type::Ka:
                    mat->_ambient_color = QVector3D(std::stof(lst[1]), std::stof(lst[2]), std::stof(lst[3]));
                    break;
                case file_param_type::map_Ka:
                    mat->_ambient_map = load_texture(str.substr(str.find(lst[1])), path);
                    mat->_use_ambient_map = true;
                    break;
                case file_param_type::Ns:
                    mat->_specexp_color = std::stof(lst[1]);
                    break;
                case file_param_type::map_Ns:
                    mat->_specexp_map = load_texture(str.substr(str.find(lst[1])), path);
                    mat->_use_specexp_map = true;
                    break;
                case file_param_type::Ks:
                    mat->_specular_color = QVector3D(std::stof(lst[1]), std::stof(lst[2]), std::stof(lst[3]));
                    break;
                case file_param_type::map_Ks:
                    mat->_specular_map = load_texture(str.substr(str.find(lst[1])), path);
                    mat->_use_specular_map = true;
                    break;
                case file_param_type::d:
                    mat->_transparent = std::stof(lst[1]);
                    break;
                case file_param_type::map_Bump:
                    mat->_normal_map = load_texture(str.substr(str.find(lst[1])), path);
                    mat->_use_normal_map = true;
                    break;
                case file_param_type::undef:
                    qDebug() << "Unknown argument: " << str.c_str();
                    break;
            }
        }
    }

    Material::file_param_type Material::get_param_type(const std::string &str)
    {
        if (str == "newmtl")
            return file_param_type::newmtl;
        if (str == "#")
            return file_param_type::comment;
        if (str == "Kd")
            return file_param_type::Kd;
        if (str == "map_Kd")
            return file_param_type::map_Kd;
        if (str == "Ka")
            return file_param_type::Ka;
        if (str == "map_Ka")
            return file_param_type::map_Ka;
        if (str == "Ns")
            return file_param_type::Ns;
        if (str == "map_Ns")
            return file_param_type::map_Ns;
        if (str == "Ks")
            return file_param_type::Ks;
        if (str == "map_Ks")
            return file_param_type::map_Ks;
        if (str == "d")
            return file_param_type::d;
        if (str == "map_Bump")
            return file_param_type::map_Bump;

        return Material::file_param_type::undef;
    }

    std::unique_ptr<QOpenGLTexture> Material::load_texture(const std::string& path, const std::filesystem::path& parent_path)
    {
        std::filesystem::path mpath(path);


        if (!mpath.is_absolute())
        {
            auto cpath = parent_path;
            cpath.remove_filename();

            mpath = cpath / mpath;
        }

        auto tex = std::make_unique<QOpenGLTexture>(QImage(mpath.string().c_str()).mirrored());

        // Set nearest filtering mode for texture minification
        tex->setMinificationFilter(QOpenGLTexture::Nearest);

        // Set bilinear filtering mode for texture magnification
        tex->setMagnificationFilter(QOpenGLTexture::Linear);

        // Wrap texture coordinates by repeating
        // f.ex. texture coordinate (1.1, 1.2) is same as (0.1, 0.2)
        tex->setWrapMode(QOpenGLTexture::Repeat);

        return tex;
    }
} // DesEngine