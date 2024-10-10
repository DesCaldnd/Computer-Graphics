//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_INTERFACES_DRAWABLE_HPP_
#define DESENGINE_DESENGINE_INCLUDE_INTERFACES_DRAWABLE_HPP_

#include <QOpenGLFunctions>
#include <functional>

namespace DesEngine
{

	class LogicObject;

	class Drawable
	{
	public:

		virtual void draw(QOpenGLFunctions& funcs) =0;

		/**
		 * You need to call uniform_values_loader on this and then call help_draw on all subobjects
		 * @param uniform_values_loader
		 */
		virtual void help_draw(std::function<void(LogicObject*)> uniform_values_loader, QOpenGLFunctions& funcs) =0;

        virtual bool cast_shadow() =0;
        virtual bool draw_in_game() =0;
        virtual void set_draw_in_game(bool d) =0;

		virtual ~Drawable() =default;
	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_INTERFACES_DRAWABLE_HPP_
