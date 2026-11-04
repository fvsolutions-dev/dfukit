# The tests are in Python, on the shipped C code through its bindings
if(TARGET _dfukit)
  add_test(NAME pytest COMMAND ${Python_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(pytest PROPERTIES ENVIRONMENT_MODIFICATION
    "PYTEST_DISABLE_PLUGIN_AUTOLOAD=set:1;DFUKIT_BUILD_DIR=set:$<TARGET_FILE_DIR:_dfukit>;PYTHONPATH=path_list_prepend:${PROJECT_SOURCE_DIR}/python;PYTHONPATH=path_list_prepend:$<TARGET_FILE_DIR:_dfukit>")
endif()
