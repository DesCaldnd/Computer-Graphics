//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_MATERIAL_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_MATERIAL_HPP_

#include <QOpenGLShaderProgram>
#include <QImage>
#include <QOpenGLTexture>
#include <memory>


namespace DesEngine
{

	class Material
	{
        std::unique_ptr<QOpenGLTexture> _albedo;

    public:

        void load_albedo(std::string path);

        void set_program(std::shared_ptr<QOpenGLShaderProgram> prog);

        void release();
	};

} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_MATERIAL_HPP_
