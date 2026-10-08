# Глава 08: корутины (docs/guide/08-coroutines.md).
ox_guide_example(coro_basics       SOURCES basics.cpp       DEPS Oxwald::async)
ox_guide_example(coro_door         SOURCES door.cpp         DEPS Oxwald::async)
ox_guide_example(coro_dialogue     SOURCES dialogue.cpp     DEPS Oxwald::async)
ox_guide_example(coro_cancellation SOURCES cancellation.cpp DEPS Oxwald::async)
ox_guide_example(coro_pathfinding  SOURCES pathfinding.cpp  DEPS Oxwald::async)
# Запрос/ответ поверх сети: отдельная header-only цель async_net (есть, только если сконфигурирован модуль net).
ox_guide_example(coro_net_rpc      SOURCES net_rpc.cpp      DEPS Oxwald::async_net)
# Lua `await`: мост AsyncBridge компилируется в Oxwald::script, когда сконфигурирован async.
ox_guide_example(coro_lua_await    SOURCES lua_await.cpp    DEPS Oxwald::script Oxwald::async)
