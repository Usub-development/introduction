cmake_minimum_required(VERSION 3.27)
project(article)

set(CMAKE_CXX_STANDARD 23)
set(UREDIS_BUILD_EXAMPLES OFF)
set(UREDIS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UREDIS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(UREDIS_LOGS OFF CACHE BOOL "" FORCE)
set(UPQ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

include(FetchContent)
include_directories(${article_SOURCE_DIR})

FetchContent_Declare(
        uvent
        GIT_REPOSITORY https://github.com/Usub-development/uvent.git
        GIT_TAG main
        OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(
        unet
        GIT_REPOSITORY https://github.com/Usub-development/unet.git
        GIT_TAG main
        OVERRIDE_FIND_PACKAGE
)
# Loading ujson from upq
FetchContent_Declare(
        upq
        GIT_REPOSITORY https://github.com/Usub-development/upq.git
        GIT_TAG main
        OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(
        ulog
        GIT_REPOSITORY https://github.com/Usub-development/ulog.git
        GIT_TAG main
        OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(
        uredis
        GIT_REPOSITORY https://github.com/Usub-development/uredis.git
        GIT_TAG main
        FIND_PACKAGE_ARGS
)

FetchContent_MakeAvailable(uvent unet ulog upq uredis)

add_executable(${PROJECT_NAME}
        src/main.cpp
        include/utils/LoggingUtils.h
        include/handlers/UserHandler.h
        src/handlers/UserHandler.cpp
        include/api/dto/requests/User.h
        include/api/dto/requests/Article.h
        include/api/dto/responses/User.h
        include/api/dto/responses/Article.h
        include/utils/HttpError.h
        include/utils/Hash.h
)

target_include_directories(${PROJECT_NAME} PRIVATE
        /usr/local/include
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
        -lpq
        OpenSSL::Crypto
        usub::uvent
        usub::server
        ZLIB::ZLIB
        usub::upq
        usub::ulog
        usub::uredis
)
