find_package(Qt6 REQUIRED COMPONENTS Test)

enable_testing(true)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure)

include_directories("../src")

set(testFiles
    testalgorithm.cpp
    testbittorrentpeeraddress.cpp
    testbittorrenttorrentdescriptor.cpp
    testbittorrenttrackerentry.cpp
    testconceptsexplicitlyconvertibleto.cpp
    testconceptsstringable.cpp
    testglobal.cpp
    testorderedset.cpp
    testpath.cpp
    testutilsbytearray.cpp
    testutilscompare.cpp
    testutilsdatetime.cpp
    testutilsdict.cpp
    testutilsfs.cpp
    testutilsgzip.cpp
    testutilshashvalue.cpp
    testutilsio.cpp
    testutilsmisc.cpp
    testutilsnet.cpp
    testutilsnumber.cpp
    testutilsstring.cpp
    testutilsversion.cpp
)

foreach(testFile ${testFiles})
    get_filename_component(testFilename "${testFile}" NAME_WLE)

    add_executable("${testFilename}" "${testFile}")
    target_link_libraries("${testFilename}" PRIVATE Qt::Test qbt_base)
    add_test(NAME "${testFilename}" COMMAND "${testFilename}")

    add_dependencies(check "${testFilename}")
endforeach()

if (GUI AND (CMAKE_SYSTEM_NAME STREQUAL "Windows"))
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    add_executable(testwindowsdpirecovery
        testwindowsdpirecovery.cpp
        ../src/gui/windowsdpirecovery.cpp
    )
    target_include_directories(testwindowsdpirecovery PRIVATE ${Boost_INCLUDE_DIRS})
    target_link_libraries(testwindowsdpirecovery PRIVATE qbt_common_cfg Qt::Test Qt::Widgets shcore uuid)
    add_test(NAME testwindowsdpirecovery COMMAND testwindowsdpirecovery)
    add_dependencies(check testwindowsdpirecovery)
endif()
