add_executable(DualizeRecomp
    main.cpp
    machine.cpp
    boot.cpp
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
    gpu/native_gx.cpp)

target_include_directories(DualizeRecomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/thirdparty/aurora/lib")
target_link_libraries(DualizeRecomp PRIVATE
    DualizeRecompLib aurora::core aurora::gx aurora::vi SDL3::SDL3)
if(WIN32)
    target_link_libraries(DualizeRecomp PRIVATE winmm shell32 ole32 uuid)
else()
    target_link_libraries(DualizeRecomp PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()
aurora_copy_runtime_dlls(DualizeRecomp)
