#pragma once

#include "core/editor_context.hpp"

#include <QDir>
#include <QObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <memory>
#include <vector>

// Several QtTest classes in one executable: each file registers its class with OX_EDITOR_TEST(Class).
namespace ox::editor::test {

using Factory = std::function<std::unique_ptr<QObject>()>;
std::vector<std::pair<const char*, Factory>>& registry();

struct Registrar {
    Registrar(const char* name, Factory f) { registry().emplace_back(name, std::move(f)); }
};

// Context whose preferences never touch the user's real preference file.
std::unique_ptr<EditorContext> makeContext();
QString tempRoot();
void pump(int ms = 30);

} // namespace ox::editor::test

#define OX_EDITOR_TEST(Class)                                                                                     \
    static ::ox::editor::test::Registrar s_reg_##Class(#Class, [] { return std::unique_ptr<QObject>(new Class()); })
