cmake_minimum_required(VERSION 3.14)
project(MemoryTracker)
enable_language(CUDA)
set(CMAKE_CXX_STANDARD 14)

include_directories(.)

add_executable(MemoryTracker MemoryTracker.cpp MemoryTrackerPatches.cu)

include_directories(/usr/local/cuda/extras/Sanitizer/include)