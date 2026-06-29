find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

execute_process(
        COMMAND ${BISON_EXECUTABLE}
                --defines=${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.h
                -o ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/yacc.y
        RESULT_VARIABLE BISON_GENERATE_RESULT)
if (NOT BISON_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate parser with bison")
endif ()

execute_process(
        COMMAND ${FLEX_EXECUTABLE}
                --outfile=${CMAKE_CURRENT_SOURCE_DIR}/lex.yy.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/lex.l
        RESULT_VARIABLE FLEX_GENERATE_RESULT)
if (NOT FLEX_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate lexer with flex")
endif ()

bison_target(yacc yacc.y ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.cpp
        DEFINES_FILE ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.h)
flex_target(lex lex.l ${CMAKE_CURRENT_SOURCE_DIR}/lex.yy.cpp)
add_flex_bison_dependency(lex yacc)

set(SOURCES ${BISON_yacc_OUTPUT_SOURCE} ${FLEX_lex_OUTPUTS} ast.cpp)
add_library(parser STATIC ${SOURCES})

add_executable(test_parser test_parser.cpp)
target_link_libraries(test_parser parser)
add_test(NAME test_parser COMMAND test_parser
        WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
