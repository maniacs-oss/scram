install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/config.rng"
  "${CMAKE_CURRENT_SOURCE_DIR}/input.rng"
  "${CMAKE_CURRENT_SOURCE_DIR}/report.rng"
  DESTINATION share/scram
  COMPONENT scram
  )

if(BUILD_GUI)
  install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/gui.rng"
    DESTINATION share/scram
    COMPONENT gui
  )
endif()
