cmake_minimum_required(VERSION 3.21)
project(QuirewiseDocument VERSION 0.1.0 LANGUAGES C)
option(QUIREWISE_BUILD_SHARED "Build Quirewise libraries as shared libraries" OFF)
if(QUIREWISE_BUILD_SHARED)
    set(QW_LIBRARY_TYPE SHARED)
else()
    set(QW_LIBRARY_TYPE STATIC)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT TARGET Quirewise::Archive)
    find_package(QuirewiseArchive CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Data)
    find_package(QuirewiseData CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Compression)
    find_package(QuirewiseCompression CONFIG REQUIRED)
endif()
file(GLOB_RECURSE QW_SOURCES CONFIGURE_DEPENDS src/*.c src/*.cpp)
add_library(quirewise_document ${QW_LIBRARY_TYPE} ${QW_SOURCES})
add_library(Quirewise::Document ALIAS quirewise_document)
set_target_properties(quirewise_document PROPERTIES EXPORT_NAME Document POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_document PUBLIC c_std_11)
target_compile_definitions(quirewise_document PRIVATE
    QUIREWISE_STANDALONE_DOCX_PARSER=1)
target_include_directories(quirewise_document PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src/formats ${CMAKE_CURRENT_SOURCE_DIR}/src/backends)
target_link_libraries(quirewise_document PRIVATE Quirewise::Archive Quirewise::Data Quirewise::Compression)
install(TARGETS quirewise_document EXPORT QuirewiseDocumentTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT QuirewiseDocumentTargets FILE QuirewiseDocumentTargets.cmake NAMESPACE Quirewise:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseDocument")
configure_package_config_file(cmake/QuirewiseDocumentConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDocumentConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseDocument")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDocumentConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDocumentConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDocumentConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseDocument")

if(QUIREWISE_BUILD_SHARED AND WIN32)
    set_target_properties(
        quirewise_document
        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(quirewise_document_html_pdf_test tests/html_pdf_writer_test.c)
    target_compile_features(quirewise_document_html_pdf_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_html_pdf_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_html_pdf_test PRIVATE quirewise_document)
    add_test(NAME quirewise_document_html_pdf
        COMMAND quirewise_document_html_pdf_test)

    add_executable(quirewise_document_odt_pdf_test tests/odt_pdf_end_to_end_test.c)
    target_compile_features(quirewise_document_odt_pdf_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_odt_pdf_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    target_link_libraries(quirewise_document_odt_pdf_test PRIVATE quirewise_document)
    add_test(NAME quirewise_document_odt_pdf
        COMMAND quirewise_document_odt_pdf_test)

    add_executable(quirewise_document_pdf_reader_test tests/pdf_reader_test.c)
    target_compile_features(quirewise_document_pdf_reader_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_reader_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_reader_test PRIVATE quirewise_document)
    add_test(NAME quirewise_document_pdf_reader
        COMMAND quirewise_document_pdf_reader_test
            "${CMAKE_CURRENT_SOURCE_DIR}/../../external-consumer-template/fixtures/one-page.pdf")

    add_executable(quirewise_document_pdf_fuzz_smoke tests/pdf_fuzz_smoke.c)
    target_compile_features(quirewise_document_pdf_fuzz_smoke PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_fuzz_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_fuzz_smoke PRIVATE quirewise_document)
    add_test(NAME quirewise_document_pdf_fuzz
        COMMAND quirewise_document_pdf_fuzz_smoke 300
            "${CMAKE_CURRENT_SOURCE_DIR}/../../external-consumer-template/fixtures/one-page.pdf")

    add_executable(quirewise_document_convert_dispatch_test
        tests/convert_dispatch_test.c)
    target_compile_features(quirewise_document_convert_dispatch_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_convert_dispatch_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_convert_dispatch
        COMMAND quirewise_document_convert_dispatch_test)
endif()
