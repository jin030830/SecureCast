cmake_minimum_required(VERSION 3.28)

# test_pii 실행 파일
add_executable(test_pii
    test_detect_pii.cpp
    ../src/ocr-engine.cpp
)

target_compile_features(test_pii PRIVATE cxx_std_17)

# SC_ENABLE_TESTS 매크로로 detect_pii_for_test 진입점 활성화
target_compile_definitions(test_pii PRIVATE SC_ENABLE_TESTS)

# RE2 링크 (루트 CMakeLists에서 이미 탐색됨)
if(TARGET re2::re2)
    target_link_libraries(test_pii PRIVATE re2::re2)
elseif(DEFINED RE2_LIBRARIES)
    target_include_directories(test_pii PRIVATE ${RE2_INCLUDE_DIRS})
    target_link_libraries(test_pii PRIVATE ${RE2_LIBRARIES})
endif()

# Windows 런타임 (windowsapp = WinRT 기본 헤더 경로 확보용)
if(WIN32)
    target_link_libraries(test_pii PRIVATE windowsapp)
endif()

# 코퍼스 파일 + 런타임 DLL을 실행 파일 옆에 복사
add_custom_command(TARGET test_pii POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/pii_corpus.txt"
        "$<TARGET_FILE_DIR:test_pii>/pii_corpus.txt"
    COMMENT "Copy pii_corpus.txt next to test_pii"
)

# vcpkg 동적 빌드라면 re2.dll / abseil_dll.dll 이 필요하다.
# IMPORTED_LOCATION으로 DLL 경로를 얻어 복사한다.
if(WIN32 AND TARGET re2::re2)
    get_target_property(_re2_dll re2::re2 IMPORTED_LOCATION_RELWITHDEBINFO)
    if(NOT _re2_dll)
        get_target_property(_re2_dll re2::re2 IMPORTED_LOCATION_RELEASE)
    endif()
    if(_re2_dll)
        add_custom_command(TARGET test_pii POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_re2_dll}"
                "$<TARGET_FILE_DIR:test_pii>/"
            COMMENT "Copy re2.dll next to test_pii"
        )
        # abseil_dll.dll은 re2.dll 과 같은 디렉터리에 있다
        get_filename_component(_re2_bin_dir "${_re2_dll}" DIRECTORY)
        set(_abseil_dll "${_re2_bin_dir}/abseil_dll.dll")
        if(EXISTS "${_abseil_dll}")
            add_custom_command(TARGET test_pii POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_abseil_dll}"
                    "$<TARGET_FILE_DIR:test_pii>/"
                COMMENT "Copy abseil_dll.dll next to test_pii"
            )
        endif()
    endif()
endif()

add_test(
    NAME pii_detection
    COMMAND test_pii "${CMAKE_CURRENT_SOURCE_DIR}/pii_corpus.txt"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:test_pii>"
)
