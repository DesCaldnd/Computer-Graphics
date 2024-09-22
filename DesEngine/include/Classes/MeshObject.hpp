//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_CLASSES_MESHOBJECT_HPP_
#define DESENGINE_DESENGINE_INCLUDE_CLASSES_MESHOBJECT_HPP_

#include "../Interfaces/LogicObject.hpp"
#include "Material.hpp"
#include <string>
#include <vector>

namespace DesEngine
{

	class MeshSubObject : public LogicObject
	{
		std::shared_ptr<Material> _mat;
	};

	class MeshObject : public LogicObject
	{
	private:

		std::string _filepath = "";
		std::vector<MeshSubObject> _subs;

	public:
	};

} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_CLASSES_MESHOBJECT_HPP_
