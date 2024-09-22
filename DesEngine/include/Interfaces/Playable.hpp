//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_INTERFACES_PLAYABLE_HPP_
#define DESENGINE_DESENGINE_INCLUDE_INTERFACES_PLAYABLE_HPP_

#include <memory>

namespace DesEngine
{

	class LogicObject;

	class Playable
	{
	public:

		virtual void begin_play();
		virtual void event_loop(double seconds);
		virtual void end_play();

		virtual void begin_overlap(std::shared_ptr<LogicObject> obj);
		virtual void end_overlap(std::shared_ptr<LogicObject> obj);

		virtual void event_hit(std::shared_ptr<LogicObject> obj);

		virtual ~Playable() =default;
	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_INTERFACES_PLAYABLE_HPP_
