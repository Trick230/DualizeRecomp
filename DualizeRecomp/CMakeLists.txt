add_executable(DualizeRecomp
    main.cpp
    machine.cpp
    boot.cpp
    profiler.cpp
    cpu/cpu.cpp
    cpu/interpreter.cpp
    kernel/memory.cpp
    kernel/scheduler.cpp
    kernel/ios_kernel.cpp
    kernel/ios/bluetooth.cpp
    kernel/ios/devices.cpp
    kernel/ios/di.cpp
    kernel/ios/directory_disc.cpp
    kernel/ios/es.cpp
    kernel/ios/fs.cpp
    kernel/ios/stm_kd.cpp
    kernel/ios/wiimote.cpp
    hw/hw.cpp
    apu/native_dsp.cpp
    apu/audio_out.cpp
    hid/gamepads.cpp
    gpu/native_gx.cpp
    sdk/rvl_ipc.cpp)

target_include_directories(DualizeRecomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/thirdparty/aurora/lib")
target_link_libraries(DualizeRecomp PRIVATE
    DualizeRecompLib aurora::core aurora::gx SDL3::SDL3)
if(WIN32)
    target_link_libraries(DualizeRecomp PRIVATE winmm shell32 ole32 uuid psapi)
    # Guest calls nest as host calls on the main thread (up to
    # DOLRECOMP_C_MAX_CALL_DEPTH of them).
    target_link_options(DualizeRecomp PRIVATE "LINKER:--stack,33554432")
else()
    target_link_libraries(DualizeRecomp PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()
aurora_copy_runtime_dlls(DualizeRecomp)
