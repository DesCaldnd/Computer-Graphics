//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_SCENE_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_SCENE_HPP_

#include "../Interfaces/LogicObject.hpp"
#include "CameraObject.hpp"
#include "LightObject.hpp"
#include "Material.hpp"
#include "GameMode.hpp"
#include <unordered_map>
#include <unordered_set>
#include <forward_list>
#include <memory>
#include <string>
#include <functional>
#include <chrono>
#include <QBasicTimer>
#include <nlohmann/json.hpp>
#include <QOpenGLShaderProgram>

namespace std
{
    template<>
    struct hash<std::pair<std::string, std::string>>
    {
        size_t operator()(const std::pair<std::string, std::string>& pair) const
        {
            hash<std::string> h;

            return h(pair.first) + h(pair.second);
        }
    };
}

namespace DesEngine
{

    class GLMainWindow;


	class Scene : public QObject
	{
		Q_OBJECT

	private:

		bool _is_in_edit, _is_in_pause = true;
		id_t _max_id = 1;

		std::shared_ptr<CameraObject> edit_camera, play_camera;
		std::shared_ptr<GameMode> _game_mode, _edit_mode;

		std::unordered_map<std::string, std::shared_ptr<Material>> _mat_lib;
		std::unordered_map<std::pair<std::string, std::string>, std::shared_ptr<QOpenGLShaderProgram>> _prog_lib;
        std::shared_ptr<QOpenGLShaderProgram> _current_prog;

		std::unordered_set<id_t> _lights;
		std::unordered_map<id_t, std::shared_ptr<LogicObject>> _all_objects;

		std::unordered_map<id_t, std::shared_ptr<LogicObject>> _renderable_objects;

		std::unordered_map<std::string, std::pair<std::function<std::shared_ptr<LogicObject>(Scene*, id_t, nlohmann::json)>, std::function<std::shared_ptr<LogicObject>(Scene*, id_t)>>> _obj_loaders;
		std::unordered_map<std::string, std::pair<std::function<std::shared_ptr<GameMode>(Scene*, id_t, nlohmann::json)>, std::function<std::shared_ptr<GameMode>(Scene*, id_t)>>> _game_mode_loaders;

		GLMainWindow* _parent;

		std::chrono::steady_clock::time_point _previous_frame_time;

        QBasicTimer _timer;

        float aspect_ratio;

    public slots:

        void update();

	public:

		explicit Scene(GLMainWindow* parent);

        void init();

		void init_in_edit_mode();

		void load_from_file(std::string path, bool in_edit);
		void save_to_file(std::string path);

		[[nodiscard("At every call scene`s max id increments")]] id_t get_new_id();

		void draw(QOpenGLFunctions& functions);

        float get_aspect_ratio() const;
        void set_aspect_ratio(float ratio);

        std::shared_ptr<QOpenGLShaderProgram> get_current_prog();

		std::shared_ptr<CameraObject> get_current_camera();
		std::shared_ptr<CameraObject> get_game_camera();
		void set_game_camera(std::shared_ptr<CameraObject>);

		std::shared_ptr<GameMode> get_game_mode();
		void set_game_mode(std::shared_ptr<GameMode> gm);
		void set_game_mode(const std::string&);
		std::shared_ptr<GameMode> load_game_mode(const std::string&);

		void start_pause();
		void end_pause();

		GLMainWindow* get_window();

		/**
		 * Returns material if exists in library,
		 * otherwise adds to library all materials from file with scene wide unique names.
		 *
		 * If material wasn`t added throws runtime_error
		 */
		std::shared_ptr<Material> get_material(std::string name, std::string path);


        std::shared_ptr<QOpenGLShaderProgram> load_program(std::string vsh_path, std::string fsh_path);
        std::shared_ptr<QOpenGLShaderProgram> get_program(std::string vsh_path, std::string fsh_path);

		void register_light(id_t);
		void remove_light(id_t);
        size_t light_count();
        void clear_lights();

		void register_object(std::shared_ptr<LogicObject>);
		void remove_object(id_t);

		void register_renderable(std::shared_ptr<LogicObject>);
		void remove_renderable(id_t);

		std::shared_ptr<LogicObject> load_object(std::string class_name, const nlohmann::json& json);

        /**
         * Can return nullptr
         * @param id
         * @return
         */
        std::shared_ptr<LogicObject> get_object(id_t id);

		/**
		 * @param class_name
		 * @param functions - first function to call at load from file, other - in editor, with dialog
		 */
		void add_object_loader(std::string class_name, std::pair<std::function<std::shared_ptr<LogicObject>(Scene*, id_t, nlohmann::json)>, std::function<std::shared_ptr<LogicObject>(Scene*, id_t)>> functions);
		void add_gamemode_loader(std::string class_name, std::pair<std::function<std::shared_ptr<GameMode>(Scene*, id_t, nlohmann::json)>, std::function<std::shared_ptr<GameMode>(Scene*, id_t)>> func);

        void timerEvent(QTimerEvent*) override;
	};

} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_SCENE_HPP_
