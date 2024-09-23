//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_INTERFACES_PHYSICABLE_HPP_
#define DESENGINE_DESENGINE_INCLUDE_INTERFACES_PHYSICABLE_HPP_

#include <string>
#include <vector>
#include <QVector3D>
#include "../Classes/BasicTypes.hpp"
#include "Transformable.hpp"

namespace DesEngine
{

	enum class collision_t
	{
		COLLIDE, OVERLAP, NONE
	};

	class Physicable : public Transformable
	{
//	protected:
//		std::vector<basic_point> collision_points;
//		std::string collision_name;
//		bool _need_phys;
//		QVector3D speed;
//		float bounce;
//
	public:
//
//		QVector3D get_speed() const;
//		void set_speed(QVector3D sp);
//
//		bool need_phys() const;
//		void set_need_phys(bool n);
//
//		float get_bounce() const;
//		void set_bounce(float bounce);
//
//		const std::string& get_collision_name() const &;
//		//Not transformed
//		const std::vector<basic_point>& get_collision_points() const &;
//		bounding_box_t get_bounding_box() const;
//
//		void update(double seconds);

		virtual ~Physicable() =default;
	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_INTERFACES_PHYSICABLE_HPP_
