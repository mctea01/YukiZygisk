# SPDX-License-Identifier: Apache-2.0
#
# YukiZygisk control client build target.
#
# License: Apache-2.0
#
# Author: Anatdx

add_executable(yzctl
  host.cpp
  main.cpp
  status.cpp
)

target_compile_features(yzctl PRIVATE cxx_std_17)
target_compile_definitions(yzctl PRIVATE _GNU_SOURCE)
target_include_directories(
  yzctl
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/userspace/common
    ${PROJECT_SOURCE_DIR}/userspace/zygisk/daemon
)
