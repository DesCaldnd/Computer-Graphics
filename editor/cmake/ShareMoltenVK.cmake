# Statically linked Qt Gui (vulkan feature) already loads Contents/Frameworks/libMoltenVK.dylib. A second copy loaded
# by the Vulkan loader through vulkan/MoltenVK_icd.json duplicates every Objective-C class and crashes, so point the
# ICD at the copy that is already loaded.  Usage: cmake -DBUNDLE=<path/to/App.app> -P ShareMoltenVK.cmake
set(_fw "${BUNDLE}/Contents/Frameworks/libMoltenVK.dylib")
set(_vk "${BUNDLE}/Contents/MacOS/vulkan")
if(EXISTS "${_fw}" AND EXISTS "${_vk}/MoltenVK_icd.json")
    file(REMOVE "${_vk}/libMoltenVK.dylib")
    file(WRITE "${_vk}/MoltenVK_icd.json" "{\n    \"file_format_version\" : \"1.0.0\",\n    \"ICD\": {\n        \"library_path\": \"../../Frameworks/libMoltenVK.dylib\",\n        \"api_version\" : \"1.4.0\",\n        \"is_portability_driver\" : true\n    }\n}\n")
endif()
