//
// Created by Des Caldnd on 10/2/2024.
//

#include "Classes/Utils.hpp"

namespace DesEngine
{
    std::vector<std::string> split(const std::string &str, const std::string &delemeter)
    {
        std::vector<std::string> res;

        size_t pos = 0, prev = 0;


        while ((pos = str.find(delemeter, prev)) != std::string::npos)
        {
            if (pos - prev != 0)
            {
                res.emplace_back(str.substr(prev, pos - prev));
            }
            prev = pos + std::max<size_t>(1, delemeter.size());
        }

        if (prev < str.size())
        {
            res.emplace_back(str.substr(prev, str.size() - prev));
        }

        return res;
    }
}