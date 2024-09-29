//
// Created by ivanl on 21.09.2024.
//

#include "Classes/Material.hpp"

namespace DesEngine
{
    void Material::load_albedo(std::string path)
    {
        _albedo = std::make_unique<QOpenGLTexture>(QImage(path.c_str()).mirrored());

        // Set nearest filtering mode for texture minification
        _albedo->setMinificationFilter(QOpenGLTexture::Nearest);

        // Set bilinear filtering mode for texture magnification
        _albedo->setMagnificationFilter(QOpenGLTexture::Linear);

        // Wrap texture coordinates by repeating
        // f.ex. texture coordinate (1.1, 1.2) is same as (0.1, 0.2)
        _albedo->setWrapMode(QOpenGLTexture::Repeat);
    }

    void Material::set_program(std::shared_ptr<QOpenGLShaderProgram> prog)
    {
        auto alb_loc = prog->uniformLocation("albedo");
        _albedo->bind(alb_loc);
        prog->setUniformValue("albedo", alb_loc);
    }

    void Material::release()
    {
        _albedo->release();
    }
} // DesEngine