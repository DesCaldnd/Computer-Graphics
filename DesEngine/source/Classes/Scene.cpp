//
// Created by ivanl on 21.09.2024.
//

#include <QFile>
#include <QImageReader>
#include <ranges>
#include "Classes/Utils.hpp"
#include "Classes/Scene.hpp"
#include "Widgets/glmainwindow.hpp"
#include "Classes/SkyBoxObject.hpp"
#include "Widgets/glwidget.hpp"

namespace DesEngine
{
    Scene::Scene(GLMainWindow *parent) : _parent(parent)
    {
        _obj_loaders.insert(std::make_pair("Mesh", std::make_pair(&MeshObject::default_mesh_object_json_loader, &MeshObject::default_mesh_object_dialog_loader)));
        _obj_loaders.insert(std::make_pair("Camera", std::make_pair(&CameraObject::default_camera_object_json_loader, &CameraObject::default_camera_object_dialog_loader)));

        _game_mode_loaders.insert(std::make_pair("EditGamemode", std::make_pair(&EditGameMode::default_gamemode_json_loader,
                                                                            &EditGameMode::default_gamemode_dialog_loader)));

        QImageReader::setAllocationLimit(256);
    }

    void Scene::init_in_edit_mode()
    {
        _is_in_edit = true;

        _edit_mode = std::make_shared<EditGameMode>(this, get_new_id());
        _game_mode = std::make_shared<EditGameMode>(this, get_new_id());

        edit_camera = std::make_shared<FlyingCamera>(this, get_new_id());
        play_camera = std::make_shared<CameraObject>(this, get_new_id());

        register_object(play_camera);
        register_object(edit_camera);
        edit_camera->translate(QVector3D(0, -2, 0));

        set_aspect_ratio(_parent->glwidget->width() / (float) _parent->glwidget->height());

        std::shared_ptr<MeshObject> testMesh = std::make_shared<MeshObject>(this, get_new_id(), "Primitives/monkeys.obj");
        register_renderable(testMesh);

        std::shared_ptr<MeshObject> testPlane = std::make_shared<MeshObject>(this, get_new_id(), "Primitives/plane.obj");
        register_renderable(testPlane);
        testPlane->scale(QVector3D(13, 7, 10));
        testPlane->translate(QVector3D(0, 0, -5));
        testPlane->rotate_x(90);

        std::shared_ptr<LightObject> light = std::make_shared<LightObject>(this, get_new_id());
        register_renderable(light);
        register_light(light->get_id());

        light->rotate_x(40);
        light->rotate_y(30);
        light->set_type(LightObject::LightType::Directional);
        light->set_power(1);
        light->set_cutoff(45);

        std::shared_ptr<LightObject> light2 = std::make_shared<LightObject>(this, get_new_id());
        register_renderable(light2);
        register_light(light2->get_id());

        light2->rotate_x(-40);
        light2->rotate_y(30);
        light2->translate(QVector3D(3, 3, 3));
        light2->set_type(LightObject::LightType::Point);
        light2->set_power(10);
        light2->set_cutoff(45);
        light2->set_soft_cutoff(30);

        std::shared_ptr<SkyBoxObject> sbox = std::make_shared<SkyBoxObject>(this, get_new_id());
        register_renderable(sbox);

        _is_in_pause = false;
        _timer.start(1, this);
    }

    void Scene::load_from_file(std::string path, bool in_edit)
    {
        // TODO:
    }

    void Scene::save_to_file(std::string path)
    {
        // TODO:
    }

    id_t Scene::get_new_id()
    {
        return _max_id++;
    }

    void Scene::update()
    {
        _parent->glwidget->update();
    }

    void Scene::draw(QOpenGLFunctions &functions)
    {

        auto new_frame_time = std::chrono::steady_clock::now();

        double seconds = std::chrono::duration<double, std::ratio<1, 1>>(new_frame_time - _previous_frame_time).count();

        _previous_frame_time = new_frame_time;

        if (_is_in_pause)
            return;

        for (auto &&object: _all_objects)
        {
            object.second->event_loop(seconds);
        }

        auto view = get_current_camera()->get_look_matrix();
        auto proj = get_current_camera()->get_projection_matrix();

        auto dpth_prog = get_program("Shaders/depth");
        auto dpth_point_prog = get_program("Shaders/depthp");
        auto pbr_prog = get_program("Shaders/pbr");

        size_t i = 0;
        for (auto&& light_id : _lights)
        {

            auto light = get_light(light_id);

            if (light->get_type() != LightObject::LightType::Point)
            {
                dpth_prog->bind();
                _current_prog = dpth_prog;
            }
            else
            {
                dpth_point_prog->bind();
                _current_prog = dpth_point_prog;
                functions.glBindFramebuffer(GL_FRAMEBUFFER, light->m_fbo);
            }


            auto depth_loader = [this](LogicObject* p){
                QMatrix4x4 model;

                model.setToIdentity();
                model.translate(p->get_translate());

                model *= get_rotation(p->get_rotation_x(), p->get_rotation_y(), p->get_rotation_z());

                model.scale(p->get_scale());
                model = p->get_global_transform() * model;

                _current_prog->setUniformValue("model", model);
            };

            light->_depth_buffer->bind();

            QMatrix4x4 proj_light = light->get_light_projection_matrix();

            _current_prog->setUniformValue("proj_light", proj_light);

            if (light->get_type() != LightObject::LightType::Point)
                _current_prog->setUniformValue("shadow_light", light->get_light_matrix().inverted());
            else
            {
                for (unsigned short j = 0; j < 6; ++j)
                {
                    _current_prog->setUniformValue((std::string("shadow_light[") + std::to_string(j) + "]").c_str(), light->get_light_matrix(j).inverted());
                }
            }

            functions.glViewport(0, 0, _depth_buffer_size.width(), _depth_buffer_size.height());
            functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);


            for (auto&& renderable : _renderable_objects)
            {
                if (renderable.second->cast_shadow())
                    renderable.second->help_draw(depth_loader, functions);
            }

            if (light->get_type() != LightObject::LightType::Point)
            {
                light->_depth_buffer->release();
            } else
            {
                functions.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
            _current_prog->release();

            GLuint texture = light->_depth_buffer->texture();

            if (light->get_type() != LightObject::LightType::Point)
            {
                functions.glActiveTexture(GL_TEXTURE0 + i * 2);
                functions.glBindTexture(GL_TEXTURE_2D, texture);
            }
            else
            {
                functions.glActiveTexture(GL_TEXTURE0 + i * 2 + 1);
                functions.glBindTexture(GL_TEXTURE_CUBE_MAP, light->m_shadowMap);
            }

            ++i;
        }

        functions.glViewport(0, 0, _parent->glwidget->width(), _parent->glwidget->height());


        bool g = pbr_prog->bind();

        i = 0;

        for (auto&& light_id : _lights)
        {
            auto light = get_light(light_id);

            QMatrix4x4 proj_light = light->get_light_projection_matrix();

            QMatrix4x4 light_mat = light->get_light_matrix(), shadow_light = light_mat.inverted();

            pbr_prog->setUniformValue((std::string("proj_light[") + std::to_string(i) + "]").c_str(), proj_light);
            pbr_prog->setUniformValue((std::string("shadow_light[") + std::to_string(i) + "]").c_str(), shadow_light);
            pbr_prog->setUniformValue((std::string("light[") + std::to_string(i) + "]").c_str(), light_mat);

            pbr_prog->setUniformValue((std::string("u_lights[") + std::to_string(i) + "].light_power").c_str(), light->get_power());
            pbr_prog->setUniformValue((std::string("u_lights[") + std::to_string(i) + "].cutoff").c_str(), light->get_cutoff());
            pbr_prog->setUniformValue((std::string("u_lights[") + std::to_string(i) + "].soft").c_str(), light->get_soft_cutoff());
            pbr_prog->setUniformValue((std::string("u_lights[") + std::to_string(i) + "].type").c_str(), (GLuint)light->get_type());
            pbr_prog->setUniformValue((std::string("u_lights[") + std::to_string(i) + "].diffuse_color").c_str(), light->get_diffuse_color());
            pbr_prog->setUniformValue((std::string("u_lights[") + std::to_string(i) + "].specular_color").c_str(), light->get_specular_color());
            pbr_prog->setUniformValue((std::string("u_lights[") + std::to_string(i) + "].shadow_map").c_str(), (GLuint)i);


            ++i;
        }

        pbr_prog->setUniformValue("u_light_count", (unsigned int)_lights.size());
        pbr_prog->setUniformValue("u_shadow_map_size", QVector2D(_depth_buffer_size.width(), _depth_buffer_size.height()));
        pbr_prog->setUniformValue("u_light_direction", QVector3D(0, 0, -1));
        pbr_prog->setUniformValue("proj", proj);
        pbr_prog->setUniformValue("view", view);


        _current_prog = pbr_prog;

        functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        for (auto&& renderable : _renderable_objects)
        {
            renderable.second->draw(functions);
        }

        pbr_prog->release();
        _current_prog = nullptr;
    }

    float Scene::get_aspect_ratio() const
    {
        return aspect_ratio;
    }

    void Scene::set_aspect_ratio(float ratio)
    {
        aspect_ratio = ratio;
        if (edit_camera != nullptr)
            edit_camera->set_aspect_ratio(aspect_ratio);

        if (play_camera != nullptr)
            play_camera->set_aspect_ratio(aspect_ratio);
    }

    std::shared_ptr<CameraObject> Scene::get_current_camera()
    {
        return _is_in_edit ? edit_camera : play_camera;
    }

    std::shared_ptr<CameraObject> Scene::get_game_camera()
    {
        return play_camera;
    }

    void Scene::set_game_camera(std::shared_ptr<CameraObject> camera)
    {
        play_camera = camera;
        play_camera->set_aspect_ratio(aspect_ratio);
        update();
    }

    std::shared_ptr<GameMode> Scene::get_game_mode()
    {
        return _game_mode;
    }

    void Scene::set_game_mode(std::shared_ptr<GameMode> gm)
    {
        _game_mode = gm;
        if (!_is_in_edit)
            _game_mode->begin_play();
    }

    void Scene::set_game_mode(const std::string& str)
    {
        auto gm = load_game_mode(str);

        _game_mode = gm;

        if (!_is_in_edit)
            _game_mode->begin_play();
    }

    std::shared_ptr<GameMode> Scene::load_game_mode(const std::string& str)
    {
        auto it = _game_mode_loaders.find(str);

        if (it == _game_mode_loaders.end())
            throw std::runtime_error("No loader registered for game mode: " + str);

        auto game_mode = it->second.second(this, get_new_id());

        return game_mode;
    }

    void Scene::start_pause()
    {
        _is_in_pause = true;
        _timer.stop();
    }

    void Scene::end_pause()
    {
        _is_in_pause = false;

        _previous_frame_time = std::chrono::steady_clock::now();

        _timer.start(1, this);
    }

    GLMainWindow *Scene::get_window()
    {
        return _parent;
    }


    std::shared_ptr<Material> Scene::get_material(const std::string& name, const std::filesystem::path& path)
    {
        auto it = _mat_lib.find(name);

        if (it != _mat_lib.end())
            return it->second;

        Material::parse_file_to_map(path, _mat_lib);

        it = _mat_lib.find(name);

        if (it != _mat_lib.end())
            return it->second;
        else
            throw std::runtime_error("Material request failed: material with name: " + name + "doesn't exist in library or in file " + path.string());
    }

    std::shared_ptr<QOpenGLShaderProgram> Scene::get_program(std::string path)
    {
        return _prog_lib.at(path);
    }

    void Scene::register_light(id_t id)
    {
        if (_lights.size() >= MAX_LIGHTS)
            throw std::runtime_error("Reached max light count");
        auto obj = get_object(id);

        auto light = std::dynamic_pointer_cast<LightObject>(obj);

        if (light != nullptr)
        {
            _lights.insert(id);
        }
        else
            qDebug() << "Trying to insert light by id which is not pointing to LightObject or doesn`t exist in scene";
    }

    void Scene::remove_light(id_t id)
    {
        _lights.erase(id);
    }

    void Scene::register_object(std::shared_ptr<LogicObject> obj)
    {
        _all_objects.insert(std::make_pair(obj->get_id(), obj));
    }

    void Scene::remove_object(id_t id)
    {
        _all_objects.erase(id);

        if (_lights.contains(id))
        {
            remove_light(id);
        }
    }

    void Scene::register_renderable(std::shared_ptr<LogicObject> obj)
    {
        _renderable_objects.insert(std::make_pair(obj->get_id(), obj));

        if (!_all_objects.contains(obj->get_id()))
        {
            _all_objects.insert(std::make_pair(obj->get_id(), obj));
        }
    }

    void Scene::remove_renderable(id_t id)
    {
        _renderable_objects.erase(id);
        if (_all_objects.contains(id))
            remove_object(id);
    }

    std::shared_ptr<LogicObject> Scene::load_object(std::string class_name, const nlohmann::json &json)
    {
        auto it = _obj_loaders.find(class_name);

        if (it == _obj_loaders.end())
            throw std::runtime_error("Attempt of loading object of unknown type " + class_name);

        auto obj = it->second.first(this, get_new_id(), json);

        return obj;
    }

    void Scene::add_object_loader(std::string class_name,
                                  std::pair<std::function<std::shared_ptr<LogicObject>(Scene *, id_t,
                                                                                       nlohmann::json)>, std::function<std::shared_ptr<LogicObject>(
                                          Scene *, id_t)>> functions)
    {
        _obj_loaders.insert(std::make_pair(class_name, functions));
    }

    void Scene::add_gamemode_loader(std::string class_name,
                                    std::pair<std::function<std::shared_ptr<GameMode>(Scene*, id_t, nlohmann::json)>, std::function<std::shared_ptr<GameMode>(Scene*, id_t)>> func)
    {
        _game_mode_loaders.insert(std::make_pair(class_name, func));
    }

    size_t Scene::light_count()
    {
        return _lights.size();
    }

    void Scene::clear_lights()
    {
        _lights.clear();
    }

    std::shared_ptr<LogicObject> Scene::get_object(id_t id)
    {
        auto it = _all_objects.find(id);

        if (it != _all_objects.end())
            return it->second;

        return {};
    }

    std::shared_ptr<QOpenGLShaderProgram> Scene::load_program(const std::string& path, bool load_geometry)
    {
        if (_prog_lib.contains(path))
            return _prog_lib.at(path);

        std::shared_ptr<QOpenGLShaderProgram> prog = std::make_shared<QOpenGLShaderProgram>();

        bool res1 = prog->addShaderFromSourceFile(QOpenGLShader::ShaderTypeBit::Vertex, (path + ".vsh").c_str());

        bool res2 = prog->addShaderFromSourceFile(QOpenGLShader::ShaderTypeBit::Fragment, (path + ".fsh").c_str());

        bool res3 = true;

        if (load_geometry)
            res3 = prog->addShaderFromSourceFile(QOpenGLShader::ShaderTypeBit::Geometry, (path + ".gsh").c_str());

        bool res4 = prog->link();

        prog->log();

        if (!(res1 && res2 && res3 && res4))
        {
            throw std::runtime_error("Program could not be compiled");
        }

        _prog_lib.insert(std::make_pair(path, prog));

        return prog;
    }

    std::shared_ptr<QOpenGLShaderProgram> Scene::get_current_prog()
    {
        return _current_prog;
    }

    void Scene::init(QSize depth_buffer_size)
    {
        load_program("Shaders/pbr", false);
        load_program("Shaders/depth", false);
        load_program("Shaders/depthp", true);
//        load_program("Shaders/color_index.vsh", "Shaders/color_index.fsh");

        _depth_buffer_size = depth_buffer_size;

    }

    void Scene::timerEvent(QTimerEvent *)
    {
        update();
    }

    GLMainWindow *Scene::get_parent()
    {
        return _parent;
    }

    std::shared_ptr<LightObject> Scene::get_light(id_t id)
    {
        return std::dynamic_pointer_cast<LightObject>(_all_objects.at(id));
    }

    QSize Scene::get_shadow_buffer_size()
    {
        return _depth_buffer_size;
    }

    bool Scene::is_in_pause()
    {
        return _is_in_edit;
    }

    bool Scene::is_in_edit()
    {
        return _is_in_pause;
    }
} // DesEngine