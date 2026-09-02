# `cmake --install` and find_package(Quartz): Quartz::quartz (C++), Quartz::quartz_c (C), Quartz::quartz-cli.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(quartz_installed quartz_options quartz_monocypher quartz_zstd quartz_support quartz_syntax quartz_sema quartz_ir
    quartz_vm quartz quartz_c)
if(TARGET quartz_shared)
    list(APPEND quartz_installed quartz_shared)
endif()
install(TARGETS ${quartz_installed} EXPORT QuartzTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The tools load the native tier from the library directory when it is not next to them.
set_target_properties(quartz-cli quartz-lsp PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
install(TARGETS quartz-cli EXPORT QuartzTargets RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS quartz-lsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET quartz_compiler)
    install(TARGETS quartz_compiler LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/quartz DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/src/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/quartz/detail
    FILES_MATCHING PATTERN "*.hpp")

set(quartz_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/Quartz)
install(EXPORT QuartzTargets NAMESPACE Quartz:: DESTINATION ${quartz_cmake_dir})
configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/QuartzConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/QuartzConfig.cmake INSTALL_DESTINATION ${quartz_cmake_dir})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/QuartzConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/QuartzConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/QuartzConfigVersion.cmake
    DESTINATION ${quartz_cmake_dir})
