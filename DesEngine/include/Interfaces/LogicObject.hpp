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
#include <QObject>

namespace DesEngine
{

	class Scene;

	class LogicObject : public QObject, public Serializable, public Drawable, public Editable, public Playable, public Physicable
	{
        Q_OBJECT

	protected:
		Scene* _scene;

    signals:

        void remove(id_t id);

	public:

		LogicObject(Scene* scene, id_t id);
	};



}

#endif //DESENGINE_DESENGINE_INCLUDE_INTERFACES_LOGICOBJECT_HPP_
