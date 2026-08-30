# SPDX-License-Identifier: MIT
# Vendored build of the cgutman/enet fork (see THIRD_PARTY.md). Kept as its own
# static library so the upstream C compiles with its own, relaxed warning set
# rather than Dish's -Werror lint wall, and so its include directory is exposed
# to the one Dish translation unit that speaks ENet.

add_library(dish_enet STATIC
    callbacks.c
    compress.c
    host.c
    list.c
    packet.c
    peer.c
    protocol.c
    unix.c)

target_include_directories(dish_enet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ENet's platform probes: HAS_* flags for the socket features it needs. These
# hold on every glibc/musl Linux, which is the only target here.
target_compile_definitions(dish_enet PRIVATE
    HAS_FCNTL=1
    HAS_IOCTL=1
    HAS_POLL=1
    HAS_GETADDRINFO=1
    HAS_GETNAMEINFO=1
    HAS_INET_PTON=1
    HAS_INET_NTOP=1
    HAS_MSGHDR_FLAGS=1
    HAS_SOCKLEN_T=1)

# Upstream C, not our code: keep it quiet rather than holding a third party to
# our conversion/shadow wall.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dish_enet PRIVATE -w)
endif()

set_target_properties(dish_enet PROPERTIES POSITION_INDEPENDENT_CODE ON)
