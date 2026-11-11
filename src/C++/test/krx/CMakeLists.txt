# The fork's own scenarios (docs/V1_FEATURES_PLAN.md). ctest runs them all; one defect's rows run with
# krx_ut "[p0-3]". Under the house policy's checker the warnings and -Werror apply (house_target);
# plain builds compile the same code without them.
add_executable(krx_ut HarnessScenarios.cpp DictionaryScenarios.cpp FieldAccessScenarios.cpp
  TypedHeaderScenarios.cpp PlainAcceptorScenarios.cpp SessionPair.cpp FixInput.cpp)
target_include_directories(krx_ut SYSTEM PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/C++)
target_link_libraries(krx_ut PRIVATE ${PROJECT_NAME} krx_catch)
target_compile_features(krx_ut PRIVATE cxx_std_23)
target_compile_definitions(krx_ut PRIVATE
  KRX_SPEC_DIR="${PROJECT_SOURCE_DIR}/spec"
  KRX_DEFINITIONS_DIR="${PROJECT_SOURCE_DIR}/test/definitions/server")
if(COMMAND house_target)
  house_target(krx_ut)
endif()
add_test(NAME krx_ut COMMAND krx_ut)
