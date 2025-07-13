set(MODULE_LIST
    DogeEd
    MainFrame
    LevelEditor
    DogeLauncher    # DogeLauncher must be the last one
)

foreach(module IN LISTS MODULE_LIST)
    add_subdirectory(${module})
    set_target_properties(${module} PROPERTIES FOLDER "Engine/Editor")
endforeach()
