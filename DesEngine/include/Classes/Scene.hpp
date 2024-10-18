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
#include <filesystem>
#include <string>
#include <functional>
#include <chrono>
#include <QBasicTimer>
#include <nlohmann/json.hpp>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>

#define MAX_LIGHTS 5

class QMouseEvent;

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

		bool _is_in_edit = true, _is_in_pause = true;
		id_t _max_id = 1;

		std::shared_ptr<CameraObject> edit_camera, play_camera;
		std::shared_ptr<GameMode> _game_mode, _edit_mode;

		std::unordered_map<std::string, std::shared_ptr<Material>> _mat_lib;
		std::unordered_map<std::string, std::shared_ptr<QOpenGLShaderProgram>> _prog_lib;
        std::shared_ptr<QOpenGLShaderProgram> _current_prog;

		std::unordered_set<id_t> _lights;
		std::unordered_map<id_t, std::shared_ptr<LogicObject>> _all_objects;

        QSize _depth_buffer_size;

		std::unordered_map<id_t, std::shared_ptr<LogicObject>> _renderable_objects;

		std::unordered_map<std::string, std::pair<std::function<std::shared_ptr<LogicObject>(Scene*, id_t, const nlohmann::json&)>, std::function<std::shared_ptr<LogicObject>(Scene*, id_t)>>> _obj_loaders;
		std::unordered_map<std::string, std::pair<std::function<std::shared_ptr<GameMode>(Scene*, id_t, const nlohmann::json&)>, std::function<std::shared_ptr<GameMode>(Scene*, id_t)>>> _game_mode_loaders;

		GLMainWindow* _parent;

		std::chrono::steady_clock::time_point _previous_frame_time;

        QBasicTimer _timer;

        float aspect_ratio;

        std::unique_ptr<QOpenGLFramebufferObject> _color_buf;

        std::vector<QMetaObject::Connection> _connections;


        void setup_edit_shortcuts();
        void setup_tab_widget();

    public slots:

        void update();

	public:

        QSize get_shadow_buffer_size();

		explicit Scene(GLMainWindow* parent);

        GLMainWindow* get_parent();

        void init(QSize depth_buffer_size = QSize(4096, 4096));

        /**
         * Can be called on uninitialized scene
         * @param new_size
         */
        void resize(QSize new_size);

		void init_in_edit_mode();

        /**
         * If scene already exists you MUST call clear() and then init()
         * @param path
         * @param in_edit
         */
		void load_from_file(const std::string& path, bool in_edit);

        /**
         * saves and loads only renderables. If you want to save/load nested objects, you can do it manually
         * If you have FUNDAMENTAL objects (f.e. play camera) nested, you MUST save it, otherwise you will have runtime error on load
         * @param path
         */
		void save_to_file(const std::string& path);

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
		std::shared_ptr<Material> get_material(const std::string& name, const std::filesystem::path& path);


        /**
         *
         * @param path - path without extension (vertex shader - path.vsh, fragment shader - path.fsh, geometry shader - path.gsh)
         * @param load_geometry - true if need to load geometry shader
         * @return
         */
        std::shared_ptr<QOpenGLShaderProgram> load_program(const std::string& path, bool load_geometry = false);
        std::shared_ptr<QOpenGLShaderProgram> get_program(std::string path);

		void register_light(id_t);
		void remove_light(id_t);
        size_t light_count();
        void clear_lights();

		void register_object(std::shared_ptr<LogicObject>);
        void remove_object(id_t);

		void register_renderable(std::shared_ptr<LogicObject>);

		std::shared_ptr<LogicObject> load_object(std::string class_name, const nlohmann::json& json);

        bool is_in_edit();
        bool is_in_pause();

        /**
         * Can return nullptr
         * @param id
         * @return
         */
        std::shared_ptr<LogicObject> get_object(id_t id);
        std::shared_ptr<LightObject> get_light(id_t id);

		/**
		 * @param class_name
		 * @param functions - first function to call at load from file, other - in editor, with dialog
		 */
		void add_object_loader(std::string class_name, std::pair<std::function<std::shared_ptr<LogicObject>(Scene*, id_t, const nlohmann::json&)>, std::function<std::shared_ptr<LogicObject>(Scene*, id_t)>> functions);
		void add_gamemode_loader(std::string class_name, std::pair<std::function<std::shared_ptr<GameMode>(Scene*, id_t, const nlohmann::json&)>, std::function<std::shared_ptr<GameMode>(Scene*, id_t)>> func);

        void clear();

        void timerEvent(QTimerEvent*) override;

        id_t select_object_by_mouse();
        id_t select_object_by_screen_coords(QPoint coords);

    public slots:

        void mousePressEvent(::QMouseEvent *event);
        void remove_renderable(id_t);

        void add_object_dialog();
        void register_light_dialog();
        void unregister_light_dialog();
        void load_gamemode_dialog();
        void set_camera_dialog();

	};

} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_SCENE_HPP_
