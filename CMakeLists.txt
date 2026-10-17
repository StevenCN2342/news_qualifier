cmake_minimum_required(VERSION 3.16)

project(news_qualifier VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(news_qualifier_core STATIC
    impl/atomic_content_qualifier.cpp
    impl/evidence_authenticity_qualifier.cpp
    impl/jury.cpp
    impl/logic_relationship_qualifier.cpp
    impl/multi_dimensional_qualifier.cpp
    impl/probability_adjuster.cpp
    impl/rejection_trigger.cpp
    json_reader/json_reader.cpp
    json_writer/json_writer.cpp
    model_config/model_config_reader.cpp
    runtime/qualifier_pipeline.cpp
    runtime/prediction_runner.cpp
)

target_include_directories(news_qualifier_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/json_reader"
    "${CMAKE_CURRENT_SOURCE_DIR}/json_writer"
    "${CMAKE_CURRENT_SOURCE_DIR}/model_config"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
)

if(MSVC)
    target_compile_options(news_qualifier_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(news_qualifier_core PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

add_executable(news_qualifier main.cpp)
target_link_libraries(news_qualifier PRIVATE news_qualifier_core)

if(MSVC)
    target_compile_options(news_qualifier PRIVATE /W4 /permissive-)
else()
    target_compile_options(news_qualifier PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

install(TARGETS news_qualifier RUNTIME DESTINATION bin)
