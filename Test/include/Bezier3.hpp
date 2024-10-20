//
// Created by Des Caldnd on 10/20/2024.
//

#ifndef DESENGINE_BEZIER3_HPP
#define DESENGINE_BEZIER3_HPP

#include <Interfaces/LogicObject.hpp>
#include <QOpenGLBuffer>
#include <Classes/MeshObject.hpp>

namespace DesEngine
{
    class Scene;
}

class Bezier3 : public DesEngine::LogicObject
{
    static constexpr const float z = 0.000000001;

    QVector3D _translate = QVector3D(0, 0, 0), _scale = QVector3D(1, 1, 1);
    float _rot_x = 0, _rot_y = 0, _rot_z = 0;
    QMatrix4x4 _global_transform;

    QVector3D _1p = QVector3D(-0.5, -0.5, z), _2p = QVector3D(-0.5, 0.5, z), _3p = QVector3D(0.5, 0.5, z), _4p = QVector3D(0.5, -0.5, z);

    QOpenGLBuffer _vert_buffer, _ind_buffer;

    bool _cast_shadow = true;
    bool _draw_in_game = false;

    size_t _points;
    float _speed = 0.4;

    DesEngine::MeshObject _sph;
    DesEngine::id_t _lo = 0;
    float _link_param = 0;
public:
    void event_loop(double seconds) override;

private:

    void recalculate_buffers();

    void update_links();

protected:

    void set_from_json(const nlohmann::json& js);

    // region properties
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_call;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_link;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_unlink;

    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_speed;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_speed;

    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_1p_x;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_1p_x;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_1p_y;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_1p_y;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_1p_z;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_1p_z;

    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_2p_x;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_2p_x;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_2p_y;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_2p_y;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_2p_z;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_2p_z;

    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_3p_x;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_3p_x;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_3p_y;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_3p_y;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_3p_z;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_3p_z;

    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_4p_x;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_4p_x;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_4p_y;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_4p_y;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_4p_z;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_4p_z;

    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_translate_x;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_translate_x;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_translate_y;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_translate_y;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_translate_z;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_translate_z;

    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_rotate_x;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_rotate_x;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_rotate_y;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_rotate_y;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_rotate_z;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_rotate_z;

    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_scale_x;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_scale_x;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_scale_y;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_scale_y;
    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_scale_z;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_scale_z;

    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_cast_shadow;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_cast_shadow;

    static const std::function<DesEngine::property_data_t(DesEngine::Editable*)> getter_draw_in_game;
    static const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> setter_draw_in_game;

    static const std::vector<DesEngine::property_t> bezier3_props;

    // endregion properties

public:
    Bezier3(DesEngine::Scene* scene, DesEngine::id_t id, size_t points = 100);

    DesEngine::id_t get_link();
    void set_link(DesEngine::id_t id = 0);

    float get_speed();
    void set_speed(float speed);

    /**
     *
     * @param p
     * @param ind 1-4
     */
    void set_point(QVector3D p, unsigned int ind);
    QVector3D get_point(unsigned int ind);

    QVector3D get_at_param(float param);


    static std::shared_ptr<LogicObject> default_bezier3_object_json_loader(DesEngine::Scene*, DesEngine::id_t, const nlohmann::json&);
    static std::shared_ptr<LogicObject> default_bezier3_object_dialog_loader(DesEngine::Scene*, DesEngine::id_t);

    std::string get_class_name() const override;

    nlohmann::json serialize() const override;

    void draw(QOpenGLFunctions &funcs) override;

    void help_draw(std::function<void(LogicObject * )> uniform_values_loader, QOpenGLFunctions &funcs) override;

    bool cast_shadow() override;

    void set_cast_shadow(bool b);

    bool draw_in_game() override;

    void set_draw_in_game(bool d) override;

    std::vector<DesEngine::property_t> get_properties() override;

    void rotate_x(float quat) override;

    void set_rotation_x(float quat) override;

    float get_rotation_x() const override;

    void rotate_y(float quat) override;

    void set_rotation_y(float quat) override;

    float get_rotation_y() const override;

    void rotate_z(float quat) override;

    void set_rotation_z(float quat) override;

    float get_rotation_z() const override;

    void scale(const QVector3D &vec) override;

    void set_scale(const QVector3D &vec) override;

    QVector3D get_scale() const override;

    void translate(const QVector3D &vec) override;

    void set_translate(const QVector3D &vec) override;

    QVector3D get_translate() const override;

    void set_global_transform(const QMatrix4x4 &mat) override;

    QMatrix4x4 get_global_transform() const override;
};


#endif //DESENGINE_BEZIER3_HPP
