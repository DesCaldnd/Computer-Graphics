//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_INTERFACES_DRAWABLE_HPP_
#define DESENGINE_DESENGINE_INCLUDE_INTERFACES_DRAWABLE_HPP_

#include <QOpenGLFunctions>

namespace DesEngine
{

	class Drawable
	{
	public:

		// TODO: draw function argument
		virtual void draw(QOpenGLFunctions* funcs) =0;

		virtual ~Drawable() =default;
	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_INTERFACES_DRAWABLE_HPP_
