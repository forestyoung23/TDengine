if(TD_LINUX)
    aux_source_directory(. AZ_TEST_SRC)

    add_executable(azTest ${AZ_TEST_SRC})
    DEP_ext_gtest(azTest)
    target_include_directories(azTest
        PUBLIC
        "${TD_SOURCE_DIR}/include/libs/azure"
        "${CMAKE_CURRENT_SOURCE_DIR}/../inc"
    )

    target_link_libraries(azTest PRIVATE
        az
    )
    enable_testing()
    add_test(
        NAME az_test
        COMMAND azTest
    )
endif(TD_LINUX)
