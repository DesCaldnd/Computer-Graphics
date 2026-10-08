# rhi_window_smoke — opens a GLFW window, clears the swapchain with an animated color through the render graph
# for N frames and exits. Not part of ctest (needs a display):  rhi_window_smoke --frames 120
if(NOT TARGET ox_rhi_glfw)
    return()
endif()
add_executable(rhi_window_smoke main.cpp)
target_link_libraries(rhi_window_smoke PRIVATE ox_rhi_glfw)
ox_set_warnings(rhi_window_smoke)
ox_deploy_vulkan_runtime(rhi_window_smoke)
set_target_properties(rhi_window_smoke PROPERTIES FOLDER "tools")
