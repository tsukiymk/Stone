set(oss_dir "${PROJECT_SOURCE_DIR}/oss")
set(sqlite_dir "${oss_dir}/sqlite")

add_library(stone-clipboard STATIC WindowsClipboard.cpp WindowsClipboard.h)
target_compile_features(stone-clipboard PUBLIC cxx_std_20)
target_compile_definitions(stone-clipboard PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(stone-clipboard PRIVATE /W4 /utf-8)
target_link_libraries(stone-clipboard PUBLIC stone-ipc user32)
add_executable(StoneService WIN32 main.cpp ServiceHost.cpp ServiceHost.h JobProfiles.cpp Broker.cpp ActivityRoutes.cpp ApplicationLogs.cpp HostServices.cpp HostWifi.cpp HostInterfaces.cpp PowerRequests.cpp JobQueue.cpp ../host/ProcessLog.cpp StoneService.rc StoneService.ico resource.h app.manifest)
target_include_directories(StoneService PRIVATE
    "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}>")
add_library(stone-notifications STATIC WindowsNotifications.cpp WindowsNotifications.h NotificationHost.cpp NotificationHost.h PendingIntents.cpp PendingIntents.h)
target_compile_features(stone-notifications PUBLIC cxx_std_20)
target_compile_definitions(stone-notifications PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(stone-notifications PRIVATE /W4 /utf-8)
target_link_libraries(stone-notifications PUBLIC stone-ipc ole32 runtimeobject)
target_link_libraries(StoneService PRIVATE stone-shell stone-ipc stone-notifications stone-clipboard ole32 wlanapi iphlpapi ws2_32 wtsapi32 userenv)
target_compile_options(StoneService PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
target_link_libraries(stone-host PUBLIC stone-ipc stone-notifications stone-sharing stone-documents)
add_custom_command(TARGET StoneService POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:StoneService>/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/docs/third-party/THIRD_PARTY_NOTICES.txt"
        "$<TARGET_FILE_DIR:StoneService>/THIRD_PARTY_NOTICES.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${oss_dir}/zydis/LICENSE"
        "$<TARGET_FILE_DIR:StoneService>/licenses/Zydis-MIT.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${oss_dir}/zydis/dependencies/zycore/LICENSE"
        "$<TARGET_FILE_DIR:StoneService>/licenses/Zycore-MIT.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${oss_dir}/xz-embedded/COPYING"
        "$<TARGET_FILE_DIR:StoneService>/licenses/XZ-0BSD.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${oss_dir}/bionic/LICENSE"
        "$<TARGET_FILE_DIR:StoneService>/licenses/Bionic-BSD-2-Clause.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${oss_dir}/vulkan/LICENSE.txt"
        "$<TARGET_FILE_DIR:StoneService>/licenses/Vulkan-Apache-2.0.txt"
    VERBATIM)
