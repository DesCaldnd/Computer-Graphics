//
// Created by ivanl on 21.09.2024.
//

#include "Classes/MeshObject.hpp"
#include <fstream>
#include <Classes/Scene.hpp>

namespace DesEngine
{
    MeshObject::MeshObject(Scene * scene, id_t id, const std::string &path) : LogicObject(scene, id), _filepath(path), _translate(0, 0, 0), _rotation(0, 0, 0, 0), _scale(1, 1, 1)
    {

        if (_filepath.empty())
            return;


        _global_transform.setToIdentity();
        std::ifstream file(_filepath);

        if (!file.is_open())
            throw std::runtime_error("Attempt of loading MeshObject from file: " + _filepath + ", but it could not be opened");

        std::vector<QVector3D> coords;
        std::vector<QVector2D> uvs;
        std::vector<QVector3D> normals;

        std::string mat_file;

        std::vector<vertex> vertexes;

        bool use_mtl = false;
        std::string mtl;

        while(!file.eof())
        {
            std::string str;
            std::getline(file, str);

            auto lst = split(str, " ");
            if (lst.empty())
                continue;
            auto type = get_param_type(lst[0]);

            if (!use_mtl)
            {

                switch (type)
                {
                    case file_param_type::comment:
                        qDebug() << "Comment: " << lst[1].c_str();
                        break;
                    case file_param_type::mtllib:
                        mat_file = lst[1];
                        break;
                    case file_param_type::vertex:
                        coords.emplace_back(std::stof(lst[1]), std::stof(lst[2]), std::stof(lst[3]));
                        break;
                    case file_param_type::uv:
                        uvs.emplace_back(std::stof(lst[1]), std::stof(lst[2]));
                        break;
                    case file_param_type::normal:
                        normals.emplace_back(std::stof(lst[1]), std::stof(lst[2]), std::stof(lst[3]));
                        break;
                    case file_param_type::usemtl:
                        mtl = lst[1];
                        _subs.emplace_back(scene, mat_file, file, coords.begin(), uvs.begin(), normals.begin(), mtl);
                        use_mtl = true;
                        break;
                }
            } else
            {
                _subs.emplace_back(scene, mat_file, file, coords.begin(), uvs.begin(), normals.begin(), mtl);
            }
        }
    }

    std::shared_ptr<LogicObject> MeshObject::default_mesh_object_json_loader(Scene *, id_t, nlohmann::json)
    {
        return std::shared_ptr<LogicObject>();
    }

    std::shared_ptr<LogicObject> MeshObject::default_mesh_object_dialog_loader(Scene *, id_t)
    {
        return std::shared_ptr<LogicObject>();
    }

    void MeshObject::draw(QOpenGLFunctions &funcs)
    {
        auto prog = _scene->get_current_prog();

        QMatrix4x4 model;

        model.setToIdentity();
        model.translate(_translate);
        model.rotate(_rotation);
        model.scale(_scale);
        model = _global_transform * model;

        prog->setUniformValue("model", model);

        for(auto&& subobj : _subs)
        {
            subobj._vert_buffer.bind();

            int offset = 0;

            int vertloc = prog->attributeLocation("a_position");
            prog->enableAttributeArray(vertloc);
            prog->setAttributeBuffer(vertloc, GL_FLOAT, offset, 3, sizeof(vertex));

            offset += sizeof(QVector3D);

            int uvloc = prog->attributeLocation("a_uv");
            prog->enableAttributeArray(uvloc);
            prog->setAttributeBuffer(uvloc, GL_FLOAT, offset, 2, sizeof(vertex));

            offset += sizeof(QVector2D);

            int normloc = prog->attributeLocation("a_normal");
            prog->enableAttributeArray(normloc);
            prog->setAttributeBuffer(normloc, GL_FLOAT, offset, 3, sizeof(vertex));

            offset += sizeof(QVector3D);

            funcs.glDrawElements(GL_TRIANGLES, subobj.count, GL_UNSIGNED_INT, 0);

            subobj._vert_buffer.release();
        }
    }

    void MeshObject::help_draw(std::function<void(LogicObject *)> uniform_values_loader)
    {

    }

    std::vector<property_t> MeshObject::get_properties()
    {
        return std::vector<property_t>();
    }

    void MeshObject::rotate(const QQuaternion &quat)
    {
        _rotation = quat * _rotation;
    }

    void MeshObject::set_rotation(const QQuaternion &quat)
    {
        _rotation = quat;
    }

    QQuaternion MeshObject::get_rotation() const
    {
        return _rotation;
    }

    void MeshObject::scale(const QVector3D &vec)
    {
        _scale.setX(_scale.x() * vec.x());
        _scale.setY(_scale.y() * vec.y());
        _scale.setZ(_scale.z() * vec.z());
    }

    void MeshObject::set_scale(const QVector3D &vec)
    {
        _scale = vec;
    }

    QVector3D MeshObject::get_scale() const
    {
        return _scale;
    }

    void MeshObject::translate(const QVector3D &vec)
    {
        _translate += vec;
    }

    void MeshObject::set_translate(const QVector3D &vec)
    {
        _translate = vec;
    }

    QVector3D MeshObject::get_translate() const
    {
        return _translate;
    }

    void MeshObject::set_global_transform(const QMatrix4x4 &mat)
    {
        _global_transform = mat;
    }

    QMatrix4x4 MeshObject::get_global_transform() const
    {
        return _global_transform;
    }

    std::string MeshObject::get_class_name() const
    {
        return "Mesh";
    }

    nlohmann::json MeshObject::serialize() const
    {
        nlohmann::json res;

        res["translateX"] = _translate.x();
        res["translateY"] = _translate.y();
        res["translateZ"] = _translate.z();

        res["scaleX"] = _translate.x();
        res["scaleY"] = _translate.y();
        res["scaleZ"] = _translate.z();

        res["rotationX"] = _rotation.x();
        res["rotationY"] = _rotation.y();
        res["rotationZ"] = _rotation.z();
        res["rotationScalar"] = _rotation.scalar();

        res["file"] = _filepath;

        std::string m("mat");

        for(int i = 0; i < 16; ++i)
        {
            res[m + std::to_string(i / 4) + "x" + std::to_string(i % 4)] = _global_transform.data()[0];
        }

        return res;
    }

    std::vector<std::string> MeshObject::split(const std::string &str, const std::string &delemeter)
    {
        std::vector<std::string> res;

        size_t pos = 0, prev = 0;


        while ((pos = str.find(delemeter, prev)) != std::string::npos)
        {
            if (pos - prev != 0)
            {
                res.emplace_back(str.substr(prev, pos - prev));
            }
            prev = pos + std::max<size_t>(1, delemeter.size());
        }

        if (prev < str.size())
        {
            res.emplace_back(str.substr(prev, str.size() - prev));
        }

        return res;
    }

    MeshObject::file_param_type MeshObject::get_param_type(const std::string &val)
    {
        if (val == "#")
            return file_param_type::comment;
        if (val == "mtllib")
            return file_param_type::mtllib;
        if (val == "v")
            return file_param_type::vertex;
        if (val == "vt")
            return file_param_type::uv;
        if (val == "vn")
            return file_param_type::normal;
        if (val == "f")
            return file_param_type::f;
        if (val == "usemtl")
            return file_param_type::usemtl;

        return file_param_type::unknown;
    }

    MeshSubObject::MeshSubObject(Scene *scene, const std::string &mat_file, std::ifstream& stream, std::vector<QVector3D>::iterator coord_it,
                                 std::vector<QVector2D>::iterator uv_it, std::vector<QVector3D>::iterator normal_it, std::string& out_str)
    {
        std::vector<vertex> vertexes;

        _mat = scene->get_material(out_str, mat_file);

        while (!stream.eof())
        {
            std::string line;

            std::getline(stream, line);

            auto lst = MeshObject::split(line, " ");

            if (lst.empty())
                continue;

            auto type = MeshObject::get_param_type(lst[0]);

            if (type == MeshObject::file_param_type::f)
            {
                for (int i = 0; i < 3; ++i)
                {
                    auto plist = MeshObject::split(lst[i + 1], "/");

                    vertexes.emplace_back(*(coord_it + std::stoull(plist[0]) - 1), *(uv_it + std::stoull(plist[1]) - 1),
                                          *(normal_it + std::stoull(plist[2]) - 1));
                }
            } else if (type == MeshObject::file_param_type::usemtl)
            {
                out_str = lst[1];
                break;
            }
        }

        _vert_buffer.create();
        _vert_buffer.bind();
        _vert_buffer.allocate(vertexes.data(), vertexes.size() * sizeof(vertex));
        _vert_buffer.release();

        count = vertexes.size();
    }
} // DesEngine