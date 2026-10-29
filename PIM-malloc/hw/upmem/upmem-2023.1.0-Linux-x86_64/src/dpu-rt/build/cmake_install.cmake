# Install script for directory: /home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "RelWithDebInfo")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/built-in" TYPE STATIC_LIBRARY FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/librt.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/built-in" TYPE STATIC_LIBRARY FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/librt_p.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/built-in" TYPE STATIC_LIBRARY FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/librtlto.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/built-in" TYPE STATIC_LIBRARY FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/librtlto_p.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/built-in" TYPE STATIC_LIBRARY FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/librtltothin.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/built-in" TYPE STATIC_LIBRARY FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/librtltothin_p.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/built-in" TYPE STATIC_LIBRARY FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/librtmcount.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include" TYPE DIRECTORY FILES
    "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/src/stdlib"
    "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/src/syslib"
    FILES_MATCHING REGEX "/[^/]*\\.h$")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/link" TYPE FILE FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/src/misc/dpu.lds")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/misc" TYPE FILE FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/src/misc/swreset.S")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/misc" TYPE FILE RENAME "crt0.o" FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/CMakeFiles/crt0.dir/src/misc/crt0.c.o")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/misc" TYPE FILE RENAME "crt0_p.o" FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/CMakeFiles/crt0_p.dir/src/misc/crt0.c.o")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/misc" TYPE FILE FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/accessMramFromDpu.bin")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/misc" TYPE FILE FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/coreDump.bin")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/misc" TYPE FILE FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/internalStateReset.bin")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/upmem/include/misc" TYPE FILE FILES "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/restoreRegisters.bin")
endif()

if(CMAKE_INSTALL_COMPONENT)
  set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
file(WRITE "/home/bongjoon/uMalloc/upmem/upmem-2023.1.0-Linux-x86_64/src/dpu-rt/build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
