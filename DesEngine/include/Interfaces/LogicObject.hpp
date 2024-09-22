//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_INTERFACES_LOGICOBJECT_HPP_
#define DESENGINE_DESENGINE_INCLUDE_INTERFACES_LOGICOBJECT_HPP_

#include "Transformable.hpp"
#include "Serializable.hpp"
#include "Drawable.hpp"
#include "Editable.hpp"
#include "Playable.hpp"
#include "Physicable.hpp"

namespace DesEngine
{

	class Scene;

	class LogicObject : public Serializable, public Drawable, public Editable, public Playable, public Physicable
	{
	protected:
		Scene* _scene;

	public:

		LogicObject(Scene* );
	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_INTERFACES_LOGICOBJECT_HPP_
