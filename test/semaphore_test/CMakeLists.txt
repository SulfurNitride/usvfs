cmake_minimum_required(VERSION 3.16)

find_package(GTest CONFIG REQUIRED)

add_executable(semaphore_test
    main.cpp
    ${PROJECT_SOURCE_DIR}/src/usvfs_dll/mappingmutex.cpp
    ${PROJECT_SOURCE_DIR}/src/usvfs_dll/semaphore.cpp)
usvfs_set_test_properties(semaphore_test)
target_include_directories(semaphore_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/usvfs_dll)
target_link_libraries(semaphore_test PRIVATE
    shared GTest::gtest GTest::gtest_main)
