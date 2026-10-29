cmake_minimum_required(VERSION 3.20)
project(communitygarage VERSION 1.1.5 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_subdirectory(platform)   # fh2mod_platform (DWrite.dll) and fh2mod_api

add_library(communitygarage SHARED mod/src/communitygarage.cpp mod/src/xhr_bridge.cpp)
target_link_libraries(communitygarage PRIVATE fh2mod_api winhttp bcrypt ole32 uuid)
set_target_properties(communitygarage PROPERTIES OUTPUT_NAME CommunityGarage PREFIX "")
target_compile_definitions(communitygarage PRIVATE COMMUNITYGARAGE_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(communitygarage PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_link_options(communitygarage PRIVATE -static)
endif()
