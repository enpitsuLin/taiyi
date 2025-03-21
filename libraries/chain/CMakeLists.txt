file(GLOB HEADERS "./*.hpp" "./util/*.hpp" "./asset_objects/*.hpp")

## SORT .cpp by most likely to change / break compile
add_library( taiyi_chain

             # As database takes the longest to compile, start it first
             database.cpp
             index.cpp
             asset_index.cpp
             contract_index.cpp
             actor_index.cpp
             zone_index.cpp

             taiyi_evaluator.cpp

             siming_schedule.cpp
             fork_database.cpp

             shared_authority.cpp
             block_log.cpp

             generic_custom_operation_interpreter.cpp
             
             lua_context.cpp
             contract_evaluator.cpp
             contract_objects.cpp
             contract_handler.cpp
             contract_worker.cpp
             database_contract.cpp

             nfa_evaluator.cpp
             database_nfa.cpp
             contract_nfa_handler.cpp

             actor_evaluator.cpp
             database_actor.cpp
             contract_actor_handler.cpp

             zone_evaluator.cpp
             database_zone.cpp
             contract_zone_handler.cpp
             taiyi_geography.cpp

             database_cultivation.cpp

             util/impacted.cpp
             util/advanced_benchmark_dumper.cpp
             util/name_generator.cpp

             ${HEADERS}
           )

target_link_libraries( taiyi_chain taiyi_protocol fc chainbase taiyi_schema appbase mira lua
                       ${PATCH_MERGE_LIB} )
target_include_directories( taiyi_chain
                            PUBLIC
                            "${CMAKE_SOURCE_DIR}/libraries/lua/lib/inc"
                            "${CMAKE_CURRENT_SOURCE_DIR}"
                            "${CMAKE_SOURCE_DIR}/libraries" 
                            "${CMAKE_CURRENT_BINARY_DIR}"
                            "${CMAKE_BINARY_DIR}/libraries" )

if( CLANG_TIDY_EXE )
   set_target_properties(
      taiyi_chain PROPERTIES
      CXX_CLANG_TIDY "${DO_CLANG_TIDY}"
   )
endif( CLANG_TIDY_EXE )

if(MSVC)
  set_source_files_properties( database.cpp PROPERTIES COMPILE_FLAGS "/bigobj" )
endif(MSVC)

INSTALL( TARGETS
   taiyi_chain

   RUNTIME DESTINATION bin
   LIBRARY DESTINATION lib
   ARCHIVE DESTINATION lib
)
INSTALL( FILES ${HEADERS} DESTINATION "include/taiyi/chain" )
