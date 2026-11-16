# lRemote desktop integration
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/io.github.mpjsilva.lRemote.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/io.github.mpjsilva.lRemote.desktop COPYONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.mpjsilva.lRemote.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES io.github.mpjsilva.lRemote.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
