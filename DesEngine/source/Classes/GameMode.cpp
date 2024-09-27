//
// Created by ivanl on 21.09.2024.
//

#include "Classes/GameMode.hpp"

namespace DesEngine
{
    EditGameMode::EditGameMode(Scene *scene, id_t id) : GameMode(scene, id)
{

}

std::string EditGameMode::get_class_name() const
{
    return "EditGamemode";
}

nlohmann::json EditGameMode::serialize() const
{
    return nlohmann::json();
}

std::shared_ptr<GameMode> EditGameMode::default_gamemode_json_loader(Scene *, id_t, nlohmann::json)
{
    return std::shared_ptr<GameMode>();
}

std::shared_ptr<GameMode> EditGameMode::default_gamemode_dialog_loader(Scene *, id_t)
{
    return std::shared_ptr<GameMode>();
}


    GameMode::GameMode(Scene *scene, id_t id) : Serializable(id), _scene(scene)
    {

    }
} // DesEngine