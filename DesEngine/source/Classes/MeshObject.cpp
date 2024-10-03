//
// Created by ivanl on 21.09.2024.
//

#include "Classes/MeshObject.hpp"
#include <fstream>
#include <Classes/Scene.hpp>
#include "Classes/Utils.hpp"

namespace DesEngine
{
    MeshObject::MeshObject(Scene * scene, id_t id, const std::string &path) : LogicObject(scene, id), _filepath(path), _translate(0, 0, 0), _scale(1, 1, 1)
    {
        _rotation.normalize();
        _global_transform.setToIdentity();

        if (_filepath.empty())
            return;

        std::ifstream file(_filepath);

        if (!file.is_open())
            throw std::runtime_error("Attempt of loading MeshObject from file: " + _filepath + ", but it could not be opened");

        std::vector<QVector3D> coords;
        std::vector<QVector2D> uvs;
        std::vector<QVector3D> normals;

        std::filesystem::path mat_file;

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
                    {
                        mat_file = std::filesystem::path(path);
                        mat_file.replace_filename(lst[1]);
                        break;
                    }
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
        model.rotate(_rotation);
        model.scale(_scale);
        model = _global_transform * model;

        prog->setUniformValue("model", model);

        for(auto&& subobj : _subs)
        {

            if (!subobj._vert_buffer.isCreated() || !subobj._index_buffer.isCreated()
            )
                throw std::runtime_error("Mesh object buffer didn't created");

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

            subobj._index_buffer.bind();

            subobj._mat->set_program(prog);

            funcs.glDrawElements(GL_TRIANGLES, subobj.count, GL_UNSIGNED_INT, 0);

            subobj._vert_buffer.release();
            subobj._index_buffer.release();
            subobj._mat->release();
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
        _rotation = quat.normalized() * _rotation;
        _rotation.normalize();
    }

    void MeshObject::set_rotation(const QQuaternion &quat)
    {
        _rotation = quat;
        _rotation;
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

    void MeshObject::event_loop(double seconds)
    {
        float angle = 45;

//        rotate(QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), angle * seconds));

//        translate(QVector3D(0, 0, 0.01 * seconds));
    }

    MeshSubObject::MeshSubObject(Scene *scene, const std::filesystem::path &mat_file, std::ifstream& stream, std::vector<QVector3D>::iterator coord_it,
                                 std::vector<QVector2D>::iterator uv_it, std::vector<QVector3D>::iterator normal_it, std::string& out_str) : _vert_buffer(QOpenGLBuffer::VertexBuffer), _index_buffer(QOpenGLBuffer::IndexBuffer)
    {
        std::vector<vertex> vertexes;
        std::vector<GLuint> indices;

        _mat = scene->get_material(out_str, mat_file);

        while (!stream.eof())
        {
            std::string line;

            std::getline(stream, line);

            auto lst = split(line, " ");

            if (lst.empty())
                continue;

            auto type = MeshObject::get_param_type(lst[0]);

            if (type == MeshObject::file_param_type::f)
            {
                for (int i = 0; i < 3; ++i)
                {
                    auto plist = split(lst[i + 1], "/");

                    vertexes.emplace_back(*(coord_it + std::stoull(plist[0]) - 1), *(uv_it + std::stoull(plist[1]) - 1),
                                          *(normal_it + std::stoull(plist[2]) - 1));
                    indices.emplace_back(indices.size());
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


        _index_buffer.create();
        _index_buffer.bind();
        _index_buffer.allocate(indices.data(), indices.size() * sizeof(GLuint));

        count = indices.size();
    }
} // DesEngine