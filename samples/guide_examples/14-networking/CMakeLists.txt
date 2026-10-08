# Глава 14: сетевая игра (docs/guide/14-networking.md).
ox_guide_example(net_bitstream   SOURCES bit_stream.cpp       DEPS Oxwald::net)
ox_guide_example(net_messages    SOURCES messages_rpc.cpp     DEPS Oxwald::net)
ox_guide_example(net_replication SOURCES replication.cpp      DEPS Oxwald::net)
ox_guide_example(net_prediction  SOURCES prediction.cpp       DEPS Oxwald::net)
ox_guide_example(net_dedicated   SOURCES dedicated_server.cpp DEPS Oxwald::net)
