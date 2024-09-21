//
// Created by ivanl on 21.09.2024.
//

#include "../../include/Interfaces/Serializable.hpp"



DesEngine::id_t DesEngine::Serializable::get_id() const
{
	return _id;
}

DesEngine::Serializable::Serializable(DesEngine::id_t id) : _id(id)
{

}
