//
// Created by Des Caldnd on 9/28/2024.
//

#include "Classes/TetCube.hpp"
#include "Classes/Scene.hpp"

namespace DesEngine
{
    void TetCube::draw(QOpenGLFunctions &funcs)
    {

        auto prog = _scene->get_current_prog();

        QMatrix4x4 model;
        model.setToIdentity();
//        model.scale(0.005);

        prog->setUniformValue("model", model);


        bool g = _vert_buf.bind();
        g = _ind_buf.bind();

        int vert_loc = prog->attributeLocation("a_position");
        prog->enableAttributeArray(vert_loc);
        prog->setAttributeBuffer(vert_loc, GL_FLOAT, 0, 3, sizeof(GLfloat) * 3);

        funcs.glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);


        _vert_buf.release();
        _ind_buf.release();
    }

    void TetCube::help_draw(std::function<void(LogicObject *)> uniform_values_loader)
    {

    }

    std::vector<property_t> TetCube::get_properties()
    {
        return std::vector<property_t>();
    }

    std::string TetCube::get_class_name() const
    {
        return "TCube";
    }

    nlohmann::json TetCube::serialize() const
    {
        return nlohmann::json();
    }

    void TetCube::rotate(const QQuaternion &quat)
    {

    }

    void TetCube::set_rotation(const QQuaternion &quat)
    {

    }

    QQuaternion TetCube::get_rotation() const
    {
        return QQuaternion(0, 0, 0, 0);
    }

    void TetCube::scale(const QVector3D &vec)
    {

    }

    void TetCube::set_scale(const QVector3D &vec)
    {

    }

    QVector3D TetCube::get_scale() const
    {
        return QVector3D(1, 1, 1);
    }

    void TetCube::translate(const QVector3D &vec)
    {

    }

    void TetCube::set_translate(const QVector3D &vec)
    {

    }

    QVector3D TetCube::get_translate() const
    {
        return QVector3D(0, 0 ,0);
    }

    void TetCube::set_global_transform(const QMatrix4x4 &mat)
    {

    }

    QMatrix4x4 TetCube::get_global_transform() const
    {
        QMatrix4x4 m;
        m.setToIdentity();
        return m;
    }

    TetCube::TetCube(Scene *scene, id_t id) : LogicObject(scene, id),
//    _vert_buf(QOpenGLBuffer::VertexBuffer),
    _ind_buf(QOpenGLBuffer::IndexBuffer)
    {
//        const GLfloat cube[] =
//                {
//                        -0.5f, -0.5f, 0,
//                        0.5f, -0.5f, 0,
//                        0.5f, 0.5f, 0,
//                        -0.5f, 0.5f, 0,
//                };
        const GLfloat cube[] =
                {
                        0, -0.5f, -0.5f,
                        0, 0.5f, -0.5f,
                        0, 0.5f, 0.5f,
                        0, -0.5f, 0.5f
                };

        const GLuint cubeFaces[] =
                {
                        3, 1, 0,
                        2,  1, 3
                };

        _vert_buf.create();
        bool g = _vert_buf.bind();
//        _vert_buf.setUsagePattern(QOpenGLBuffer::UsagePattern::StaticDraw);
        _vert_buf.allocate(cube, 12 * sizeof(GLfloat ));


        _ind_buf.create();
        g = _ind_buf.bind();
//        _ind_buf.setUsagePattern(QOpenGLBuffer::UsagePattern::StaticDraw);
        _ind_buf.allocate(cubeFaces, 6 * sizeof(GLuint));

        _vert_buf.release();
        _ind_buf.release();

    }
} // DesEngine