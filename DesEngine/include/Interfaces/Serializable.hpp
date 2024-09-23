//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_INTERFACES_SERIALIZABLE_HPP_
#define DESENGINE_DESENGINE_INCLUDE_INTERFACES_SERIALIZABLE_HPP_

#include <cstddef>
#include <string>
#include <nlohmann/json.hpp>

namespace DesEngine
{
	using id_t = size_t;

	class Serializable
	{
		id_t _id;

	public:

		explicit Serializable(id_t id);

		virtual std::string get_class_name() const =0;
		id_t get_id() const;

        virtual nlohmann::json serialize() const =0;

		virtual ~Serializable() =default;
	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_INTERFACES_SERIALIZABLE_HPP_
