add_executable(DbToDot EXCLUDE_FROM_ALL db_to_dot.cpp)
set_target_properties(DbToDot PROPERTIES
    OUTPUT_NAME "db-to-dot"
    DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX}
)
target_link_libraries(DbToDot Core)
