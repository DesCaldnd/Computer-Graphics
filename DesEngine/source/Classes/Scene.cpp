//
// Created by ivanl on 21.09.2024.
//

#include "Classes/Scene.hpp"
#include "Widgets/glmainwindow.hpp"
#include "Widgets/glwidget.hpp"

namespace DesEngine
{
    Scene::Scene(GLMainWindow *parent) : _parent(parent)
    {

    }

    void Scene::init_in_edit_mode()
    {

    }

    void Scene::load_from_file(std::string path, bool in_edit)
    {

    }

    void Scene::save_to_file(std::string path)
    {

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

        //TODO: set uniform variables

        //TODO: shadow and light passes

        auto sh_it = get_program("pbr.vsh", "pbr.fsh");

        sh_it->bind();
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
        //TODO: stop timer
    }

    void Scene::end_pause()
    {
        _is_in_pause = false;

        //TODO: start timer

        _previous_frame_time = std::chrono::steady_clock::now();
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

    void Scene::remove_light(id_t)
    {

    }

    void Scene::register_object(id_t id, std::shared_ptr<LogicObject>)
    {

    }

    void Scene::remove_object(id_t)
    {

    }

    void Scene::register_renderable(id_t, std::shared_ptr<LogicObject>)
    {

    }

    void Scene::remove_renderable(id_t)
    {

    }

    std::shared_ptr<LogicObject> Scene::load_object(std::string class_name, const nlohmann::json &json)
    {
        return std::shared_ptr<LogicObject>();
    }

    void Scene::add_object_loader(std::string class_name,
                                  std::pair<std::function<std::shared_ptr<LogicObject>(Scene *, id_t,
                                                                                       nlohmann::json)>, std::function<std::shared_ptr<LogicObject>(
                                          Scene *, id_t)>> functions)
    {

    }

    void Scene::add_gamemode_loader(std::string class_name,
                                    std::pair<std::function<std::shared_ptr<GameMode>(Scene*, id_t, nlohmann::json)>, std::function<std::shared_ptr<GameMode>(Scene*, id_t)>> func)
    {

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
} // DesEngine