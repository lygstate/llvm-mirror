include_directories( ${CMAKE_CURRENT_BINARY_DIR}/.. 
                     ${CMAKE_CURRENT_SOURCE_DIR}/.. )

add_llvm_library(LLVMMBlazeInfo
  MBlazeTargetInfo.cpp
  )

add_llvm_library_dependencies(LLVMMBlazeInfo
  LLVMMC
  LLVMSupport
  LLVMTarget
  )

add_dependencies(LLVMMBlazeInfo MBlazeCommonTableGen)
