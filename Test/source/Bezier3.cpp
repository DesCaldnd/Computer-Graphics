//
// Created by Des Caldnd on 10/20/2024.
//

#include "../include/Bezier3.hpp"
#include <Classes/Scene.hpp>
#include <Classes/Utils.hpp>
#include <QOpenGLExtraFunctions>
#include <Widgets/glmainwindow.hpp>
#include <Widgets/glwidget.hpp>
#include <QInputDialog>
#include <QMessageBox>

void Bezier3::rotate_x(float quat)
{
    _rot_x = fmodf(_rot_x + quat, 360);
    update_links();
}

void Bezier3::set_rotation_x(float quat)
{
    _rot_x = fmodf(quat, 360);
    update_links();
}

float Bezier3::get_rotation_x() const
{
    return _rot_x;
}

void Bezier3::rotate_y(float quat)
{
    _rot_y = fmodf(_rot_y + quat, 360);
    update_links();
}

void Bezier3::set_rotation_y(float quat)
{
    _rot_y = fmodf(quat, 360);
    update_links();
}

float Bezier3::get_rotation_y() const
{
    return _rot_y;
}

void Bezier3::rotate_z(float quat)
{
    _rot_z = fmodf(_rot_z + quat, 360);
    update_links();
}

void Bezier3::set_rotation_z(float quat)
{
    _rot_z = fmodf(quat, 360);
    update_links();
}

float Bezier3::get_rotation_z() const
{
    return _rot_z;
}

void Bezier3::scale(const QVector3D &vec)
{
    _scale *= vec;
    update_links();
}

void Bezier3::set_scale(const QVector3D &vec)
{
    _scale = vec;
    update_links();
}

QVector3D Bezier3::get_scale() const
{
    return _scale;
}

void Bezier3::translate(const QVector3D &vec)
{
    _translate += vec;
    update_links();
}

void Bezier3::set_translate(const QVector3D &vec)
{
    _translate = vec;
    update_links();
}

QVector3D Bezier3::get_translate() const
{
    return _translate;
}

void Bezier3::set_global_transform(const QMatrix4x4 &mat)
{
    _global_transform = mat;
    update_links();
}

QMatrix4x4 Bezier3::get_global_transform() const
{
    return _global_transform;
}

std::vector<DesEngine::property_t> Bezier3::get_properties()
{
    return bezier3_props;
}

void Bezier3::draw(QOpenGLFunctions &funcs)
{
    if (!draw_in_game() && !_scene->is_in_edit())
        return;

    auto sc_prog = _scene->get_current_prog();
    auto prog = _scene->get_program("Shaders/line");

    sc_prog->release();
    prog->bind();

    QMatrix4x4 model;

    model.setToIdentity();
    model.translate(get_translate());
    model *= DesEngine::get_rotation(get_rotation_x(), get_rotation_y(), get_rotation_z());
    model.scale(get_scale());
    model = get_global_transform() * model;

    prog->setUniformValue("view", _scene->get_current_camera()->get_look_matrix());
    prog->setUniformValue("proj", _scene->get_current_camera()->get_projection_matrix());
    prog->setUniformValue("model", model);

    if (!_vert_buffer.isCreated())
        throw std::runtime_error("Bezier3 object buffer didn't created");

    _vert_buffer.bind();

    int offset = 0;

    int vertloc = prog->attributeLocation("a_position");
    prog->enableAttributeArray(vertloc);
    prog->setAttributeBuffer(vertloc, GL_FLOAT, offset, 3, sizeof(QVector3D));

    funcs.glLineWidth(_scene->get_parent()->glwidget->width() / 40);

    funcs.glDrawArrays(GL_LINE_STRIP, 0, _points);

    _vert_buffer.release();

    prog->release();
    sc_prog->bind();

    QVector3D pos;
    pos = (model * QVector4D(_1p, 1)).toVector3D();
    _sph.set_translate(pos);
    _sph.draw(funcs);
    pos = (model * QVector4D(_2p, 1)).toVector3D();
    _sph.set_translate(pos);
    _sph.draw(funcs);
    pos = (model * QVector4D(_3p, 1)).toVector3D();
    _sph.set_translate(pos);
    _sph.draw(funcs);
    pos = (model * QVector4D(_4p, 1)).toVector3D();
    _sph.set_translate(pos);
    _sph.draw(funcs);

}

void Bezier3::help_draw(std::function<void(LogicObject * )> uniform_values_loader, QOpenGLFunctions &funcs)
{
    if (!draw_in_game() && !_scene->is_in_edit())
        return;

    auto prog = _scene->get_current_prog();

    uniform_values_loader(this);

    QMatrix4x4 model;

    model.setToIdentity();
    model.translate(get_translate());
    model *= DesEngine::get_rotation(get_rotation_x(), get_rotation_y(), get_rotation_z());
    model.scale(get_scale());
    model = get_global_transform() * model;

    if (!_vert_buffer.isCreated())
        throw std::runtime_error("Bezier3 object buffer didn't created");

    _vert_buffer.bind();

    int offset = 0;

    int vertloc = prog->attributeLocation("a_position");
    prog->enableAttributeArray(vertloc);
    prog->setAttributeBuffer(vertloc, GL_FLOAT, offset, 3, sizeof(QVector3D));

    funcs.glLineWidth(_scene->get_parent()->glwidget->width() / 40);

    funcs.glDrawArrays(GL_LINE_STRIP, 0, _points);

    _vert_buffer.release();


    QVector3D pos;
    pos = (model * QVector4D(_1p, 1)).toVector3D();
    _sph.set_translate(pos);
    _sph.help_draw(uniform_values_loader, funcs);
    pos = (model * QVector4D(_2p, 1)).toVector3D();
    _sph.set_translate(pos);
    _sph.help_draw(uniform_values_loader,funcs);
    pos = (model * QVector4D(_3p, 1)).toVector3D();
    _sph.set_translate(pos);
    _sph.help_draw(uniform_values_loader,funcs);
    pos = (model * QVector4D(_4p, 1)).toVector3D();
    _sph.set_translate(pos);
    _sph.help_draw(uniform_values_loader,funcs);

}

bool Bezier3::cast_shadow()
{
    return _cast_shadow;
}

void Bezier3::set_cast_shadow(bool b)
{
    _cast_shadow = b;
}

bool Bezier3::draw_in_game()
{
    return _draw_in_game;
}

void Bezier3::set_draw_in_game(bool d)
{
    _draw_in_game = d;
}

std::string Bezier3::get_class_name() const
{
    return "Bezier3";
}

nlohmann::json Bezier3::serialize() const
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

    res["cast_shadow"] = _cast_shadow;
    res["draw_in_game"] = _draw_in_game;
    
    res["p1_x"] = _1p.x();
    res["p1_y"] = _1p.y();
    res["p1_z"] = _1p.z();

    res["p2_x"] = _2p.x();
    res["p2_y"] = _2p.y();
    res["p2_z"] = _2p.z();

    res["p3_x"] = _3p.x();
    res["p3_y"] = _3p.y();
    res["p3_z"] = _3p.z();

    res["p4_x"] = _4p.x();
    res["p4_y"] = _4p.y();
    res["p4_z"] = _4p.z();

    std::string m("mat");

    for(int i = 0; i < 16; ++i)
    {
        res[m + std::to_string(i / 4) + "x" + std::to_string(i % 4)] = _global_transform.data()[i];
    }

    return res;
}

Bezier3::Bezier3(DesEngine::Scene *scene, DesEngine::id_t id, size_t points) : DesEngine::LogicObject(scene, id), _vert_buffer(QOpenGLBuffer::Type::VertexBuffer),
                                                                               _ind_buffer(QOpenGLBuffer::IndexBuffer), _sph(scene, id, "Primitives/sphere.obj")
{
    _global_transform.setToIdentity();

    _sph.scale(QVector3D(0.25, 0.25, 0.25));
    scene->load_program("Shaders/line");

    std::vector<QVector3D> p;
    p.reserve(points + 1);

    for (size_t i = 0; i <= points; ++i)
    {
        p.push_back(get_at_param(((float)i) / points));
    }

    _points = points + 1;

    _vert_buffer.create();
    _vert_buffer.bind();
    _vert_buffer.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    _vert_buffer.allocate(p.data(), p.size() * sizeof(QVector3D));
    _vert_buffer.release();
}

std::shared_ptr<DesEngine::LogicObject> Bezier3::default_bezier3_object_dialog_loader(DesEngine::Scene *scene, DesEngine::id_t id)
{
    bool ok;

    std::string number = QInputDialog::getText(scene->get_parent(), "Bezier3", "Enter bezier3 interpolation points count", QLineEdit::Normal, "100", &ok).toStdString();

    if (!ok)
        return {};

    size_t count;

    try
    {
        count = std::stoull(number);
    } catch (std::invalid_argument& e)
    {
        QMessageBox::information(scene->get_parent(), "Bezier3 creation error", e.what());
        return {};
    }
    if (count < 5)
        count = 5;

    auto res = std::make_shared<Bezier3>(scene, id, count);

    return res;
}

std::shared_ptr<DesEngine::LogicObject>
Bezier3::default_bezier3_object_json_loader(DesEngine::Scene *scene, DesEngine::id_t id, const nlohmann::json &js)
{
    auto res = std::make_shared<Bezier3>(scene, id);
    res->set_from_json(js);

    return res;
}

void Bezier3::recalculate_buffers()
{
    std::vector<QVector3D> p;
    p.reserve(_points);

    for (size_t i = 0; i < _points; ++i)
    {
        p.push_back(get_at_param(((float)i) / (_points - 1)));
    }

    _vert_buffer.bind();
    _vert_buffer.write(0, p.data(), sizeof(QVector3D) * p.size());
    _vert_buffer.release();

    update_links();
}

void Bezier3::set_from_json(const nlohmann::json &js)
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
    
    _1p.setX(js["p1_x"].get<float>());
    _1p.setY(js["p1_y"].get<float>());
    _1p.setZ(js["p1_z"].get<float>());

    _2p.setX(js["p2_x"].get<float>());
    _2p.setY(js["p2_y"].get<float>());
    _2p.setZ(js["p2_z"].get<float>());

    _3p.setX(js["p3_x"].get<float>());
    _3p.setY(js["p3_y"].get<float>());
    _3p.setZ(js["p3_z"].get<float>());

    _4p.setX(js["p4_x"].get<float>());
    _4p.setY(js["p4_y"].get<float>());
    _4p.setZ(js["p4_z"].get<float>());

    std::string m("mat");

    for(int i = 0; i < 16; ++i)
    {
        _global_transform.data()[i] = js[m + std::to_string(i / 4) + "x" + std::to_string(i % 4)].get<float>();
    }
}

void Bezier3::set_point(QVector3D p, unsigned int ind)
{
    if (ind < 1)
        ind = 1;
    if (ind > 4)
        ind = 3;

    switch (ind)
    {
        case 1:
            _1p = p;
            break;
        case 2:
            _2p = p;
            break;
        case 3:
            _3p = p;
            break;
        case 4:
            _4p = p;
            break;
    }

    recalculate_buffers();
}

QVector3D Bezier3::get_point(unsigned int ind)
{
    if (ind < 1)
        ind = 1;
    if (ind > 4)
        ind = 3;

    switch (ind)
    {
        case 1:
            return _1p;
            break;
        case 2:
            return _2p;
            break;
        case 3:
            return _3p;
            break;
        case 4:
            return _4p;
            break;
    }

    return QVector3D(1, 1, 1);
}

QVector3D Bezier3::get_at_param(float param)
{
    if (param < 0)
        param = 0;
    if (param > 1)
        param = 1;


    return _1p * pow(1 - param, 3) + 3 * _2p * param * pow(1 - param, 2) + 3 * _3p * param * param * (1 - param) + _4p * pow(param, 3);
}

void Bezier3::update_links()
{
    auto obj = _scene->get_object(_lo);

    if (obj != nullptr)
    {
        QVector3D pos = get_at_param(_link_param);
        QMatrix4x4 model;

        model.setToIdentity();
        model.translate(get_translate());
        model *= DesEngine::get_rotation(get_rotation_x(), get_rotation_y(), get_rotation_z());
        model.scale(get_scale());
        model = get_global_transform() * model;

        pos = (model * QVector4D(pos, 1)).toVector3D();

        obj->set_translate(pos);
    }
}

DesEngine::id_t Bezier3::get_link()
{
    return _lo;
}

void Bezier3::set_link(DesEngine::id_t id)
{
    _lo = id;
}

void Bezier3::event_loop(double seconds)
{
    float val = std::fmod(get_speed() * seconds + _link_param, 1);
    _link_param = val > 0 ? val : 1 + val;

    update_links();
}

float Bezier3::get_speed()
{
    return _speed;
}

void Bezier3::set_speed(float speed)
{
    _speed = speed;
}

// region properties

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_call = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return 0;
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_link = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    bool ok;

    auto obj = static_cast<Bezier3*>(e);

    std::string number = QInputDialog::getText(obj->_scene->get_parent(), "Enter linkable id", "Id", QLineEdit::Normal, "", &ok).toStdString();

    if (!ok)
        return false;

    DesEngine::id_t id;

    try
    {
        id = std::stoull(number);
    } catch (std::invalid_argument& e)
    {
        QMessageBox::information(obj->_scene->get_parent(), "Id error", e.what());
        return false;
    }

    auto linkable = obj->_scene->get_object(id);

    if (linkable != nullptr)
    {
        obj->set_link(id);
    } else
    {
        QMessageBox::information(obj->_scene->get_parent(), "Link error", "Object does not exist or was not registered");
        return false;
    }

    return true;
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_unlink = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    static_cast<Bezier3*>(e)->set_link();
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_speed = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_speed();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_speed = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    static_cast<Bezier3*>(e)->set_speed(std::get<float>(data));
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_1p_x = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(1).x();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_1p_x = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(1);
    static_cast<Bezier3*>(e)->set_point(QVector3D(std::get<float>(data), translate.y(), translate.z()), 1);
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_1p_y = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(1).y();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_1p_y = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(1);
    static_cast<Bezier3*>(e)->set_point(QVector3D(translate.x(), std::get<float>(data), translate.z()), 1);
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_1p_z = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(1).z();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_1p_z = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(1);
    static_cast<Bezier3*>(e)->set_point(QVector3D(translate.x(), translate.y(), std::get<float>(data)), 1);
    return true;
};
const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_2p_x = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(2).x();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_2p_x = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(2);
    static_cast<Bezier3*>(e)->set_point(QVector3D(std::get<float>(data), translate.y(), translate.z()), 2);
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_2p_y = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(2).y();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_2p_y = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(2);
    static_cast<Bezier3*>(e)->set_point(QVector3D(translate.x(), std::get<float>(data), translate.z()), 2);
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_2p_z = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(2).z();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_2p_z = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(2);
    static_cast<Bezier3*>(e)->set_point(QVector3D(translate.x(), translate.y(), std::get<float>(data)), 2);
    return true;
};
const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_3p_x = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(3).x();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_3p_x = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(3);
    static_cast<Bezier3*>(e)->set_point(QVector3D(std::get<float>(data), translate.y(), translate.z()), 3);
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_3p_y = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(3).y();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_3p_y = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(3);
    static_cast<Bezier3*>(e)->set_point(QVector3D(translate.x(), std::get<float>(data), translate.z()), 3);
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_3p_z = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(3).z();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_3p_z = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(3);
    static_cast<Bezier3*>(e)->set_point(QVector3D(translate.x(), translate.y(), std::get<float>(data)), 3);
    return true;
};
const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_4p_x = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(4).x();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_4p_x = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(4);
    static_cast<Bezier3*>(e)->set_point(QVector3D(std::get<float>(data), translate.y(), translate.z()), 4);
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_4p_y = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(4).y();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_4p_y = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(4);
    static_cast<Bezier3*>(e)->set_point(QVector3D(translate.x(), std::get<float>(data), translate.z()), 4);
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_4p_z = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_point(4).z();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_4p_z = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_point(4);
    static_cast<Bezier3*>(e)->set_point(QVector3D(translate.x(), translate.y(), std::get<float>(data)), 4);
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_translate_x = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_translate().x();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_translate_x = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_translate();
    static_cast<Bezier3*>(e)->set_translate(QVector3D(std::get<float>(data), translate.y(), translate.z()));
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_translate_y = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_translate().y();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_translate_y = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_translate();
    static_cast<Bezier3*>(e)->set_translate(QVector3D(translate.x(), std::get<float>(data), translate.z()));
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_translate_z = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_translate().z();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_translate_z = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D translate = static_cast<Bezier3*>(e)->get_translate();
    static_cast<Bezier3*>(e)->set_translate(QVector3D(translate.x(), translate.y(), std::get<float>(data)));
    return true;
};


const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_rotate_x = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_rotation_x();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_rotate_x = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    static_cast<Bezier3*>(e)->set_rotation_x(std::get<float>(data));
    return true;
};


const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_rotate_y = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_rotation_y();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_rotate_y = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    static_cast<Bezier3*>(e)->set_rotation_y(std::get<float>(data));
    return true;
};


const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_rotate_z = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_rotation_z();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_rotate_z = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    static_cast<Bezier3*>(e)->set_rotation_z(std::get<float>(data));
    return true;
};


const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_scale_x = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_scale().x();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_scale_x = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D scale = static_cast<Bezier3*>(e)->get_scale();
    static_cast<Bezier3*>(e)->set_scale(QVector3D(std::get<float>(data), scale.y(), scale.z()));
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_scale_y = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_scale().y();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_scale_y = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D scale = static_cast<Bezier3*>(e)->get_scale();
    static_cast<Bezier3*>(e)->set_scale(QVector3D(scale.x(), std::get<float>(data), scale.z()));
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_scale_z = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->get_scale().z();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_scale_z = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    QVector3D scale = static_cast<Bezier3*>(e)->get_scale();
    static_cast<Bezier3*>(e)->set_scale(QVector3D(scale.x(), scale.y(), std::get<float>(data)));
    return true;
};


const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_cast_shadow = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->cast_shadow();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_cast_shadow = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    static_cast<Bezier3*>(e)->set_cast_shadow(std::get<bool>(data));
    return true;
};

const std::function<DesEngine::property_data_t(DesEngine::Editable*)> Bezier3::getter_draw_in_game = [](DesEngine::Editable* e) -> DesEngine::property_data_t {
    return static_cast<Bezier3*>(e)->draw_in_game();
};

const std::function<bool(DesEngine::Editable*, DesEngine::property_data_t)> Bezier3::setter_draw_in_game = [](DesEngine::Editable* e, DesEngine::property_data_t data) -> bool {
    static_cast<Bezier3*>(e)->set_draw_in_game(std::get<bool>(data));
    return true;
};

const std::vector<DesEngine::property_t> Bezier3::bezier3_props = {
        {DesEngine::property_data_type_t::CALL, "Link object", getter_call, setter_link},
        {DesEngine::property_data_type_t::CALL, "Unlink object", getter_call, setter_unlink},
        {DesEngine::property_data_type_t::FLOAT, "Speed", getter_speed, setter_speed},
        {DesEngine::property_data_type_t::FLOAT, "First point x", getter_1p_x, setter_1p_x},
        {DesEngine::property_data_type_t::FLOAT, "First point y", getter_1p_y, setter_1p_y},
        {DesEngine::property_data_type_t::FLOAT, "First point z", getter_1p_z, setter_1p_z},
        {DesEngine::property_data_type_t::FLOAT, "Second point x", getter_2p_x, setter_2p_x},
        {DesEngine::property_data_type_t::FLOAT, "Second point y", getter_2p_y, setter_2p_y},
        {DesEngine::property_data_type_t::FLOAT, "Second point z", getter_2p_z, setter_2p_z},
        {DesEngine::property_data_type_t::FLOAT, "Third point x", getter_3p_x, setter_3p_x},
        {DesEngine::property_data_type_t::FLOAT, "Third point y", getter_3p_y, setter_3p_y},
        {DesEngine::property_data_type_t::FLOAT, "Third point z", getter_3p_z, setter_3p_z},
        {DesEngine::property_data_type_t::FLOAT, "Fourth point x", getter_4p_x, setter_4p_x},
        {DesEngine::property_data_type_t::FLOAT, "Fourth point y", getter_4p_y, setter_4p_y},
        {DesEngine::property_data_type_t::FLOAT, "Fourth point z", getter_4p_z, setter_4p_z},
        {DesEngine::property_data_type_t::FLOAT, "Translate x", getter_translate_x, setter_translate_x},
        {DesEngine::property_data_type_t::FLOAT, "Translate y", getter_translate_y, setter_translate_y},
        {DesEngine::property_data_type_t::FLOAT, "Translate z", getter_translate_z, setter_translate_z},
        {DesEngine::property_data_type_t::FLOAT, "Rotate x", getter_rotate_x, setter_rotate_x},
        {DesEngine::property_data_type_t::FLOAT, "Rotate y", getter_rotate_y, setter_rotate_y},
        {DesEngine::property_data_type_t::FLOAT, "Rotate z", getter_rotate_z, setter_rotate_z},
        {DesEngine::property_data_type_t::FLOAT, "Scale x", getter_scale_x, setter_scale_x},
        {DesEngine::property_data_type_t::FLOAT, "Scale y", getter_scale_y, setter_scale_y},
        {DesEngine::property_data_type_t::FLOAT, "Scale z", getter_scale_z, setter_scale_z},
        {DesEngine::property_data_type_t::BOOLEAN, "Cast shadow", getter_cast_shadow, setter_cast_shadow},
        {DesEngine::property_data_type_t::BOOLEAN, "Draw in game", getter_draw_in_game, setter_draw_in_game},
};


// endregion properties

