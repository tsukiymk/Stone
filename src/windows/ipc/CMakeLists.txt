add_library(stone-ipc STATIC ServiceProtocol.cpp ServiceProtocol.h ../client/BrokerClient.cpp ../client/BrokerClient.h ../client/ClientApplicationLogs.cpp ../client/ClientApplicationLogs.h ClientSignals.cpp ClientSignals.h LaunchProfile.cpp LaunchProfile.h HostStateCodec.cpp HostInterfacesCodec.cpp NotificationIdentity.cpp)
target_compile_features(stone-ipc PUBLIC cxx_std_20)
target_compile_definitions(stone-ipc PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(stone-ipc PRIVATE /W4 /utf-8)
target_link_libraries(stone-ipc PUBLIC advapi32 bcrypt shell32 user32)

target_include_directories(stone-ipc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src/windows/client")
