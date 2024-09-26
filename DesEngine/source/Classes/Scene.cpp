//
// Created by ivanl on 21.09.2024.
//

#include <QFile>
#include "Classes/Scene.hpp"
#include "Widgets/glmainwindow.hpp"
#include "Widgets/glwidget.hpp"

namespace DesEngine
{
    Scene::Scene(GLMainWindow *parent) : _parent(parent)
    {
        _obj_loaders.insert(std::make_pair("Mesh", std::make_pair(&MeshObject::default_mesh_object_json_loader, &MeshObject::default_mesh_object_dialog_loader)));
        _obj_loaders.insert(std::make_pair("Camera", std::make_pair(&CameraObject::default_camera_object_json_loader, &CameraObject::default_camera_object_dialog_loader)));

        _game_mode_loaders.insert(std::make_pair("Gamemode", std::make_pair(&EditGameMode::default_gamemode_json_loader,
                                                                            &EditGameMode::default_gamemode_dialog_loader)));

        load_program("Shaders/pbr.vsh", "Shaders/pbr.fsh");
        load_program("Shaders/depth.vsh", "Shaders/depth.fsh");
        load_program("Shaders/color_index.vsh", "Shaders/color_index.fsh");
    }

    void Scene::init_in_edit_mode()
    {
        _is_in_edit = true;

        _edit_mode = std::make_shared<EditGameMode>(this, get_new_id());
        _game_mode = std::make_shared<EditGameMode>(this, get_new_id());

        edit_camera = std::make_shared<CameraObject>(this, get_new_id());
        play_camera = std::make_shared<CameraObject>(this, get_new_id());

        register_object(play_camera);

        connect(&_timer, &QTimer::timeout, this, &Scene::update);
        _is_in_pause = false;
        _timer.start(1);
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

        for (auto &&object: _renderable_objects)
        {
            object.second->event_loop(seconds);
        }

        auto view = get_current_camera()->get_look_matrix();
        auto proj = get_current_camera()->get_projection_matrix();

        //TODO: shadow and light passes

        auto sh_it = get_program("Shaders/pbr.vsh", "Shaders/pbr.fsh");

        sh_it->bind();

        sh_it->setUniformValue("proj", proj);
        sh_it->setUniformValue("view", view);

        _current_prog = sh_it;

        for (auto&& renderable : _renderable_objects)
        {
            renderable.second->draw(functions);
        }

        sh_it->release();
        _current_prog = nullptr;
    }

    float Scene::get_aspect_ratio() const
    {
        return aspect_ratio;
    }

    void Scene::set_aspect_ratio(float ratio)
    {
        aspect_ratio = ratio;
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

        _timer.start(1);
    }

    GLMainWindow *Scene::get_window()
    {
        return _parent;
    }


    std::shared_ptr<Material> Scene::get_material(std::string name, std::string path)
    {
        auto it = _mat_lib.find(name);

        if (it != _mat_lib.end())
            return it->second;

        //TODO: load material

        it = _mat_lib.find(name);

        if (it != _mat_lib.end())
            return it->second;
        else
            throw std::runtime_error("Material request failed: material with name: " + name + "doesn't exist in library or in file " + path);
    }

    std::shared_ptr<QOpenGLShaderProgram> Scene::get_program(std::string vsh_path, std::string fsh_path)
    {
        return _prog_lib.at(std::make_pair(vsh_path, fsh_path));
    }

    void Scene::register_light(id_t id)
    {
        auto obj = get_object(id);

        auto light = std::dynamic_pointer_cast<LightObject>(obj);

        if (light != nullptr)
            _lights.insert(id);
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
    }

    void Scene::register_renderable(std::shared_ptr<LogicObject> obj)
    {
        _renderable_objects.insert(std::make_pair(obj->get_id(), obj));
    }

    void Scene::remove_renderable(id_t id)
    {
        _renderable_objects.erase(id);
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

    std::shared_ptr<QOpenGLShaderProgram> Scene::load_program(std::string vsh_path, std::string fsh_path)
    {
        std::shared_ptr<QOpenGLShaderProgram> prog = std::make_shared<QOpenGLShaderProgram>();

        bool res1 = prog->addShaderFromSourceFile(QOpenGLShader::ShaderTypeBit::Vertex, vsh_path.c_str());
        bool res2 = prog->addShaderFromSourceFile(QOpenGLShader::ShaderTypeBit::Fragment, fsh_path.c_str());

        bool res3 = prog->link();

        if (!(res1 && res2 && res3))
        {
            throw std::runtime_error("Program could not be compiled");
        }

        return prog;
    }
} // DesEngine