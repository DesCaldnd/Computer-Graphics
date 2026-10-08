# Глава 31: ассеты (docs/guide/31-assets.md). Исходные файлы генерируются во временном проекте (project_fixture.hpp).
ox_guide_example(assets_database SOURCES database.cpp DEPS Oxwald::assets)
ox_guide_example(assets_loading  SOURCES loading.cpp  DEPS Oxwald::assets)
ox_guide_example(assets_pak      SOURCES pak.cpp      DEPS Oxwald::assets Oxwald::scene)
# Запуск собранной игры из .oxpak, как OxwaldPlayer --pak (нужен runtime, собранный с assets).
ox_guide_example(assets_player   SOURCES pak_player.cpp DEPS Oxwald::runtime Oxwald::assets Oxwald::scene)
