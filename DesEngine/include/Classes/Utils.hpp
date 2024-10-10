//
// Created by Des Caldnd on 10/2/2024.
//

#ifndef DESENGINE_UTILS_HPP
#define DESENGINE_UTILS_HPP

#include <vector>
#include <string>
#include <QMatrix4x4>

namespace DesEngine
{
    std::vector<std::string> split(const std::string& str, const std::string& delemeter);

    QMatrix4x4 get_rotation(float x, float y, float z);
}

#endif //DESENGINE_UTILS_HPP
