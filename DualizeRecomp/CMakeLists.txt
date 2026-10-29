add_executable(DualizeRecomp
    main.cpp
    machine.cpp
    boot.cpp
    profiler.cpp
    kernel/ios_kernel.cpp
    kernel/ios/devices.cpp
    kernel/ios/di.cpp
    kernel/ios/directory_disc.cpp
    kernel/ios/es.cpp
    kernel/ios/fs.cpp
    kernel/ios/stm_kd.cpp
    kernel/ios/wiimote.cpp
    kernel/ios/sysconf.cpp
    kernel/ios/wiimote_pairing.cpp
    hw/hw.cpp
    apu/native_dsp.cpp
    apu/audio_out.cpp
    hid/gamepads.cpp
    sdk/rvl_alarm.cpp
    sdk/rvl_exi.cpp
    sdk/rvl_interrupt.cpp
    sdk/rvl_ipc.cpp
    sdk/rvl_pad.cpp
    sdk/rvl_vi.cpp
    sdk/rvl_wud.cpp)

target_include_directories(DualizeRecomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/thirdparty/aurora/lib")
target_link_libraries(DualizeRecomp PRIVATE
    DualizeRecompLib dr_host aurora::core aurora::gx SDL3::SDL3)
if(WIN32)
    target_link_libraries(DualizeRecomp PRIVATE winmm shell32 ole32 uuid psapi)
    # Guest calls nest as host calls on the main thread (up to
    # DOLRECOMP_C_MAX_CALL_DEPTH of them).
    target_link_options(DualizeRecomp PRIVATE "LINKER:--stack,33554432")
else()
    target_link_libraries(DualizeRecomp PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()
aurora_copy_runtime_dlls(DualizeRecomp)
# The pipelines seen in play (scripts/pipeline_seed.py), merged into the
# user's cache and compiled before the game starts.
add_custom_command(TARGET DualizeRecomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/initial_pipeline_cache.db"
        "$<TARGET_FILE_DIR:DualizeRecomp>/initial_pipeline_cache.db")
