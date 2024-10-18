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

    public slots:

        /**
         *  Dont call at destructor. It id needed only for removing from scene while working
         */
        virtual void remove();

    signals:

        void remove_sig(id_t);

	public:

		LogicObject(Scene* scene, id_t id);
	};



}

#endif //DESENGINE_DESENGINE_INCLUDE_INTERFACES_LOGICOBJECT_HPP_
