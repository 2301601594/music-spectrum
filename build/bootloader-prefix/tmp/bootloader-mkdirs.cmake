# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/dongj/esp/v5.4.2/esp-idf/components/bootloader/subproject"
  "/home/dongj/esp/Code/file_serving/build/bootloader"
  "/home/dongj/esp/Code/file_serving/build/bootloader-prefix"
  "/home/dongj/esp/Code/file_serving/build/bootloader-prefix/tmp"
  "/home/dongj/esp/Code/file_serving/build/bootloader-prefix/src/bootloader-stamp"
  "/home/dongj/esp/Code/file_serving/build/bootloader-prefix/src"
  "/home/dongj/esp/Code/file_serving/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/dongj/esp/Code/file_serving/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/dongj/esp/Code/file_serving/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
