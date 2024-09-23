//
// Created by ivanl on 21.09.2024.
//

#include "Classes/Scene.hpp"

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
        return 0;
    }

    void Scene::update()
    {

    }

    void Scene::draw(QOpenGLFunctions &functions)
    {

    }

    float Scene::get_aspect_ratio() const
    {
        return 0;
    }

    void Scene::set_aspect_ratio(float ratio)
    {

    }

    std::shared_ptr<CameraObject> Scene::get_current_camera()
    {
        return std::shared_ptr<CameraObject>();
    }

    std::shared_ptr<CameraObject> Scene::get_game_camera()
    {
        return std::shared_ptr<CameraObject>();
    }

    void Scene::set_game_camera(std::shared_ptr<CameraObject>)
    {

    }

    std::shared_ptr<GameMode> Scene::get_game_mode()
    {
        return std::shared_ptr<GameMode>();
    }

    void Scene::set_game_mode(std::shared_ptr<GameMode> gm)
    {

    }

    void Scene::set_game_mode(std::string)
    {

    }

    std::shared_ptr<GameMode> Scene::load_game_mode(std::string)
    {
        return std::shared_ptr<GameMode>();
    }

    void Scene::start_pause()
    {

    }

    void Scene::end_pause()
    {

    }

    GLMainWindow *Scene::get_window()
    {
        return nullptr;
    }


    std::shared_ptr<Material> Scene::get_material(std::string name, std::string path)
    {
        return std::shared_ptr<Material>();
    }

    QOpenGLShaderProgram &Scene::get_program(std::string vsh_path, std::string fsh_path)
    {
        return _prog_lib.at(std::make_pair(vsh_path, fsh_path));
    }

    void Scene::register_light(id_t)
    {

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
                                    std::function<std::shared_ptr<GameMode>(Scene *, id_t, nlohmann::json)> func)
    {

    }
} // DesEngine