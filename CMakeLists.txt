AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/EnhancedGroundTargeting.cpp")
AC_ADD_SCRIPT_LOADER("EnhancedGroundTargeting" "${CMAKE_CURRENT_LIST_DIR}/src/loader.h")

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_LIST_DIR}/conf/EnhancedGroundTargeting.conf.dist")