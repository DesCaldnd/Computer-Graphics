//
// Created by Des Caldnd on 9/28/2024.
//

#ifndef DESENGINE_TETCUBE_HPP
#define DESENGINE_TETCUBE_HPP

#include "Interfaces/LogicObject.hpp"
#include <QOpenGLBuffer>

namespace DesEngine
{

    class TetCube : public LogicObject
    {
        QOpenGLBuffer _vert_buf;
        QOpenGLBuffer _ind_buf;

    public:

        TetCube(Scene* scene, id_t id);

        void draw(QOpenGLFunctions &funcs) override;

        void help_draw(std::function<void(LogicObject *)> uniform_values_loader) override;

        std::vector<property_t> get_properties() override;

        std::string get_class_name() const override;

        nlohmann::json serialize() const override;

        void rotate(const QQuaternion &quat) override;

        void set_rotation(const QQuaternion &quat) override;

        QQuaternion get_rotation() const override;

        void scale(const QVector3D &vec) override;

        void set_scale(const QVector3D &vec) override;

        QVector3D get_scale() const override;

        void translate(const QVector3D &vec) override;

        void set_translate(const QVector3D &vec) override;

        QVector3D get_translate() const override;

        void set_global_transform(const QMatrix4x4 &mat) override;

        QMatrix4x4 get_global_transform() const override;
    };

} // DesEngine

#endif //DESENGINE_TETCUBE_HPP
