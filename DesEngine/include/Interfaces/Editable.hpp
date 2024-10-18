//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_INTERFACES_EDITABLE_HPP_
#define DESENGINE_DESENGINE_INCLUDE_INTERFACES_EDITABLE_HPP_

#include <vector>
#include <variant>
#include <string>
#include <functional>

namespace DesEngine
{

	using call_t = void*;

	using property_data_t = std::variant<call_t, int, unsigned int, float, std::string, bool>;

	enum class property_data_type_t
	{
		CALL, INT, UINT, FLOAT, STRING, BOOLEAN
	};

	class Editable;

	struct property_t
	{
		property_data_type_t type;
		std::string name;

        /**
         * If type is call, calls setter
         */
		std::function<property_data_t(Editable*)> getter;

		// return true if set is success
		std::function<bool(Editable*, property_data_t)> setter;
	};


	class Editable
	{
	public:

		virtual std::vector<property_t> get_properties() =0;

		virtual ~Editable() =default;
	};

}

#endif //DESENGINE_DESENGINE_INCLUDE_INTERFACES_EDITABLE_HPP_
