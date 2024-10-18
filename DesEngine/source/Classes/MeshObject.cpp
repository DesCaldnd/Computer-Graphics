//
// Created by ivanl on 21.09.2024.
//

#include "Classes/MeshObject.hpp"
#include <fstream>
#include <Classes/Scene.hpp>
#include "Widgets/glmainwindow.hpp"
#include <QFileDialog>
#include <QMessageBox>
#include "Classes/Utils.hpp"
#include <cmath>

namespace DesEngine
{
    MeshObject::MeshObject(Scene * scene, id_t id, const std::string &path) : LogicObject(scene, id), _filepath(path), _translate(0, 0, 0), _scale(1, 1, 1)
    {
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

    std::shared_ptr<LogicObject> MeshObject::default_mesh_object_json_loader(Scene *scene, id_t id, const nlohmann::json& js)
    {
        std::string path = js["file"].get<std::string>();

        auto res = std::make_shared<MeshObject>(scene, id, path);
        res->set_from_json(js);

        return res;
    }

    std::shared_ptr<LogicObject> MeshObject::default_mesh_object_dialog_loader(Scene *scene, id_t id)
    {
        std::string file = QFileDialog::getOpenFileName(scene->get_parent(), "Open mesh object", "./", "Obj wavefront (*.obj)").toStdString();

        try
        {
            auto ptr = std::make_shared<MeshObject>(scene, id, file);

            return ptr;
        } catch (std::runtime_error& e)
        {
            QMessageBox::information(scene->get_parent(), "Object loading error", e.what());
            return {};
        }
    }

    void MeshObject::draw(QOpenGLFunctions &funcs)
    {
        if (!draw_in_game() && !_scene->is_in_edit())
            return;

        auto prog = _scene->get_current_prog();

        QMatrix4x4 model;

        model.setToIdentity();
        model.translate(get_translate());
        model *= get_rotation(_rot_x, _rot_y, _rot_z);
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

            int tangloc = prog->attributeLocation("a_tangent");
            prog->enableAttributeArray(tangloc);
            prog->setAttributeBuffer(tangloc, GL_FLOAT, offset, 3, sizeof(vertex));

            offset += sizeof(QVector3D);

            int bitangloc = prog->attributeLocation("a_bitangent");
            prog->enableAttributeArray(bitangloc);
            prog->setAttributeBuffer(bitangloc, GL_FLOAT, offset, 3, sizeof(vertex));

            offset += sizeof(QVector3D);

            subobj._index_buffer.bind();

            subobj._mat->set_program(prog);

            funcs.glDrawElements(GL_TRIANGLES, subobj.count, GL_UNSIGNED_INT, 0);

            subobj._vert_buffer.release();
            subobj._index_buffer.release();
            subobj._mat->release();
        }
    }

    void MeshObject::help_draw(std::function<void(LogicObject *)> uniform_values_loader, QOpenGLFunctions& funcs)
    {
        uniform_values_loader(this);
        auto prog = _scene->get_current_prog();

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

            subobj._index_buffer.bind();

            funcs.glDrawElements(GL_TRIANGLES, subobj.count, GL_UNSIGNED_INT, 0);

            subobj._vert_buffer.release();
            subobj._index_buffer.release();
        }
    }

    std::vector<property_t> MeshObject::get_properties()
    {
        return mesh_props;
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

        res["scaleX"] = _scale.x();
        res["scaleY"] = _scale.y();
        res["scaleZ"] = _scale.z();

        res["rotationX"] = _rot_x;
        res["rotationY"] = _rot_y;
        res["rotationZ"] = _rot_z;

        res["file"] = _filepath;

        res["cast_shadow"] = _cast_shadow;
        res["draw_in_game"] = _draw_in_game;

        std::string m("mat");

        for(int i = 0; i < 16; ++i)
        {
            res[m + std::to_string(i / 4) + "x" + std::to_string(i % 4)] = _global_transform.data()[i];
        }

        return res;
    }

    void MeshObject::set_from_json(const nlohmann::json &js)
    {
        _translate.setX(js["translateX"].get<float>());
        _translate.setY(js["translateY"].get<float>());
        _translate.setZ(js["translateZ"].get<float>());

        _scale.setX(js["scaleX"].get<float>());
        _scale.setY(js["scaleY"].get<float>());
        _scale.setZ(js["scaleZ"].get<float>());

        _rot_x = js["rotationX"].get<float>();
        _rot_y = js["rotationY"].get<float>();
        _rot_z = js["rotationZ"].get<float>();

        _cast_shadow = js["cast_shadow"].get<bool>();
        _draw_in_game = js["draw_in_game"].get<bool>();

        std::string m("mat");

        for(int i = 0; i < 16; ++i)
        {
            _global_transform.data()[i] = js[m + std::to_string(i / 4) + "x" + std::to_string(i % 4)].get<float>();
        }
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

    void MeshObject::rotate_x(float quat)
    {
        _rot_x += fmodf(quat, 360);
    }

    void MeshObject::set_rotation_x(float quat)
    {
        _rot_x = fmodf(quat, 360);
    }

    float MeshObject::get_rotation_x() const
    {
        return _rot_x;
    }

    void MeshObject::rotate_y(float quat)
    {
        _rot_y += fmodf(quat, 360);
    }

    void MeshObject::set_rotation_y(float quat)
    {
        _rot_y = fmodf(quat, 360);
    }

    float MeshObject::get_rotation_y() const
    {
        return _rot_y;
    }

    void MeshObject::rotate_z(float quat)
    {
        _rot_z += fmodf(quat, 360);
    }

    void MeshObject::set_rotation_z(float quat)
    {
        _rot_z = fmodf(quat, 360);
    }

    float MeshObject::get_rotation_z() const
    {
        return _rot_z;
    }

    bool MeshObject::cast_shadow()
    {
        return _cast_shadow;
    }

    void MeshObject::set_cast_shadow(bool s)
    {
        _cast_shadow = s;
    }

    bool MeshObject::draw_in_game()
    {
        return _draw_in_game;
    }

    void MeshObject::set_draw_in_game(bool d)
    {
        _draw_in_game = d;
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

        for (size_t i = 0; i < vertexes.size(); i += 3)
        {
            auto&& v1 = vertexes[i];
            auto&& v2 = vertexes[i + 1];
            auto&& v3 = vertexes[i + 2];

            QVector3D delta_pos_1 = v2.point - v1.point;
            QVector3D delta_pos_2 = v3.point - v1.point;

            QVector2D delta_uv_1 = v2.uv - v1.uv;
            QVector2D delta_uv_2 = v3.uv - v1.uv;

            float r = 1.0f / (delta_uv_1.x() * delta_uv_2.y() - delta_uv_1.y() * delta_uv_2.x());
            QVector3D tangent = (delta_pos_1 * delta_uv_2.y() - delta_pos_2 * delta_uv_1.y()) * r;
            QVector3D bitangent = (delta_pos_1 * delta_uv_1.x() - delta_pos_2 * delta_uv_2.x()) * r;

            v1.tangent = tangent;
            v2.tangent = tangent;
            v3.tangent = tangent;

            v1.bitangent = bitangent;
            v2.bitangent = bitangent;
            v3.bitangent = bitangent;
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

    const std::function<property_data_t(Editable*)> MeshObject::getter_translate_x = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->get_translate().x();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_translate_x = [](Editable* e, property_data_t data) -> bool {
        QVector3D translate = static_cast<MeshObject*>(e)->get_translate();
        static_cast<MeshObject*>(e)->set_translate(QVector3D(std::get<float>(data), translate.y(), translate.z()));
        return true;
    };

    const std::function<property_data_t(Editable*)> MeshObject::getter_translate_y = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->get_translate().y();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_translate_y = [](Editable* e, property_data_t data) -> bool {
        QVector3D translate = static_cast<MeshObject*>(e)->get_translate();
        static_cast<MeshObject*>(e)->set_translate(QVector3D(translate.x(), std::get<float>(data), translate.z()));
        return true;
    };

    const std::function<property_data_t(Editable*)> MeshObject::getter_translate_z = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->get_translate().z();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_translate_z = [](Editable* e, property_data_t data) -> bool {
        QVector3D translate = static_cast<MeshObject*>(e)->get_translate();
        static_cast<MeshObject*>(e)->set_translate(QVector3D(translate.x(), translate.y(), std::get<float>(data)));
        return true;
    };


    const std::function<property_data_t(Editable*)> MeshObject::getter_rotate_x = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->get_rotation_x();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_rotate_x = [](Editable* e, property_data_t data) -> bool {
        static_cast<MeshObject*>(e)->set_rotation_x(std::get<float>(data));
        return true;
    };


    const std::function<property_data_t(Editable*)> MeshObject::getter_rotate_y = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->get_rotation_y();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_rotate_y = [](Editable* e, property_data_t data) -> bool {
        static_cast<MeshObject*>(e)->set_rotation_y(std::get<float>(data));
        return true;
    };


    const std::function<property_data_t(Editable*)> MeshObject::getter_rotate_z = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->get_rotation_z();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_rotate_z = [](Editable* e, property_data_t data) -> bool {
        static_cast<MeshObject*>(e)->set_rotation_z(std::get<float>(data));
        return true;
    };


    const std::function<property_data_t(Editable*)> MeshObject::getter_scale_x = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->get_scale().x();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_scale_x = [](Editable* e, property_data_t data) -> bool {
        QVector3D scale = static_cast<MeshObject*>(e)->get_scale();
        static_cast<MeshObject*>(e)->set_scale(QVector3D(std::get<float>(data), scale.y(), scale.z()));
        return true;
    };

    const std::function<property_data_t(Editable*)> MeshObject::getter_scale_y = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->get_scale().y();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_scale_y = [](Editable* e, property_data_t data) -> bool {
        QVector3D scale = static_cast<MeshObject*>(e)->get_scale();
        static_cast<MeshObject*>(e)->set_scale(QVector3D(scale.x(), std::get<float>(data), scale.z()));
        return true;
    };

    const std::function<property_data_t(Editable*)> MeshObject::getter_scale_z = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->get_scale().z();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_scale_z = [](Editable* e, property_data_t data) -> bool {
        QVector3D scale = static_cast<MeshObject*>(e)->get_scale();
        static_cast<MeshObject*>(e)->set_scale(QVector3D(scale.x(), scale.y(), std::get<float>(data)));
        return true;
    };


    const std::function<property_data_t(Editable*)> MeshObject::getter_cast_shadow = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->cast_shadow();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_cast_shadow = [](Editable* e, property_data_t data) -> bool {
        static_cast<MeshObject*>(e)->set_cast_shadow(std::get<bool>(data));
        return true;
    };

    const std::function<property_data_t(Editable*)> MeshObject::getter_draw_in_game = [](Editable* e) -> property_data_t {
        return static_cast<MeshObject*>(e)->draw_in_game();
    };

    const std::function<bool(Editable*, property_data_t)> MeshObject::setter_draw_in_game = [](Editable* e, property_data_t data) -> bool {
        static_cast<MeshObject*>(e)->set_draw_in_game(std::get<bool>(data));
        return true;
    };

    const std::vector<property_t> MeshObject::mesh_props = {
            {property_data_type_t::FLOAT, "Translate x", getter_translate_x, setter_translate_x},
            {property_data_type_t::FLOAT, "Translate y", getter_translate_y, setter_translate_y},
            {property_data_type_t::FLOAT, "Translate z", getter_translate_z, setter_translate_z},
            {property_data_type_t::FLOAT, "Rotate x", getter_rotate_x, setter_rotate_x},
            {property_data_type_t::FLOAT, "Rotate y", getter_rotate_y, setter_rotate_y},
            {property_data_type_t::FLOAT, "Rotate z", getter_rotate_z, setter_rotate_z},
            {property_data_type_t::FLOAT, "Scale x", getter_scale_x, setter_scale_x},
            {property_data_type_t::FLOAT, "Scale y", getter_scale_y, setter_scale_y},
            {property_data_type_t::FLOAT, "Scale z", getter_scale_z, setter_scale_z},
            {property_data_type_t::BOOLEAN, "Cast shadow", getter_cast_shadow, setter_cast_shadow},
            {property_data_type_t::BOOLEAN, "Draw in game", getter_draw_in_game, setter_draw_in_game},
    };
} // DesEngine