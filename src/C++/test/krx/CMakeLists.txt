# The fork's own scenarios (docs/V1_FEATURES_PLAN.md). ctest runs them all; one defect's rows run with
# krx_ut "[p0-3]". Under the house policy's checker the warnings and -Werror apply (house_target);
# plain builds compile the same code without them.
add_executable(krx_ut HarnessScenarios.cpp DictionaryScenarios.cpp FieldAccessScenarios.cpp
  TypedHeaderScenarios.cpp PlainAcceptorScenarios.cpp InboundFramingScenarios.cpp
  SocketLivenessScenarios.cpp TeardownRaceScenarios.cpp StoreDurabilityScenarios.cpp
  SendTruthScenarios.cpp SessionPair.cpp FixInput.cpp)
target_include_directories(krx_ut SYSTEM PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/C++)
target_link_libraries(krx_ut PRIVATE ${PROJECT_NAME} krx_catch)
target_compile_features(krx_ut PRIVATE cxx_std_23)
target_compile_definitions(krx_ut PRIVATE
  KRX_SPEC_DIR="${PROJECT_SOURCE_DIR}/spec"
  KRX_DEFINITIONS_DIR="${PROJECT_SOURCE_DIR}/test/definitions/server"
  KRX_CORPUS_DIR="${PROJECT_SOURCE_DIR}/test/krx/corpus")
if(COMMAND house_target)
  house_target(krx_ut)
endif()
add_test(NAME krx_ut COMMAND krx_ut)

# The latency and throughput baseline (plan item 1.8): a tool, not a test.
add_executable(krx_bench Bench.cpp)
target_include_directories(krx_bench SYSTEM PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/C++)
target_link_libraries(krx_bench PRIVATE ${PROJECT_NAME})
target_compile_features(krx_bench PRIVATE cxx_std_23)
target_compile_definitions(krx_bench PRIVATE KRX_SPEC_DIR="${PROJECT_SOURCE_DIR}/spec")
if(COMMAND house_target)
  house_target(krx_bench)
endif()

# The fuzz target (plan items 1.7, 8.6): clang with libFuzzer, cmake -DKRX_FUZZ=ON.
option(KRX_FUZZ "Build the libFuzzer target krx_fuzz (clang)" OFF)
if(KRX_FUZZ)
  add_executable(krx_fuzz FuzzEntry.cpp FixInput.cpp)
  target_include_directories(krx_fuzz SYSTEM PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/C++)
  target_link_libraries(krx_fuzz PRIVATE ${PROJECT_NAME})
  target_compile_features(krx_fuzz PRIVATE cxx_std_23)
  target_compile_definitions(krx_fuzz PRIVATE KRX_SPEC_DIR="${PROJECT_SOURCE_DIR}/spec")
  target_compile_options(krx_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(krx_fuzz PRIVATE -fsanitize=fuzzer)
  if(COMMAND house_target)
    house_target(krx_fuzz)
  endif()
endif()
