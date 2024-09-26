//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_GAMEMODE_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_GAMEMODE_HPP_

#include "Interfaces/Playable.hpp"
#include <nlohmann/json.hpp>
#include "Interfaces/Serializable.hpp"

namespace DesEngine
{

    class Scene;

	class GameMode : public Playable, public Serializable
	{

    public:
        GameMode(Scene*, id_t);
	};

    class EditGameMode : public GameMode
    {

    public:

        EditGameMode(Scene*, id_t);

        std::string get_class_name() const override;
        nlohmann::json serialize() const override;

        static std::shared_ptr<GameMode> default_gamemode_json_loader(Scene*, id_t, nlohmann::json);
        static std::shared_ptr<GameMode> default_gamemode_dialog_loader(Scene*, id_t);
    };

} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_GAMEMODE_HPP_
