# Add all the warnings to the files
if( NOT KICAD_IPC_API )
    message( FATAL_ERROR "KiChad requires KICAD_IPC_API for native Codex PCB tools" )
endif()

if( COMPILER_SUPPORTS_WARNINGS )
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${WARN_FLAGS_CXX}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${WARN_FLAGS_C}")
endif()

add_compile_definitions( KICAD )
add_subdirectory( pcm )

include_directories( BEFORE ${INC_BEFORE} )
include_directories(
    ${CMAKE_SOURCE_DIR}/common/dialogs
    ${CMAKE_SOURCE_DIR}/pcbnew
    ${CMAKE_SOURCE_DIR}/eeschema
    ${CMAKE_SOURCE_DIR}/kicad/pcm
    ${INC_AFTER}
    )

set( KICAD_SRCS
    codex/board_render_artifact_validator.cpp
    codex/board_ps_artifact_validator.cpp
    codex/codex_app_server_client.cpp
    codex/codex_protocol_log.cpp
    codex/codex_tool_design.cpp
    codex/codex_tool_fabricate.cpp
    codex/codex_tool_inspect.cpp
    codex/codex_tool_layout.cpp
    codex/codex_tool_pcb.cpp
    codex/codex_tool_project.cpp
    codex/codex_tool_verify.cpp
    codex/design_script_board_asset_compiler.cpp
    codex/design_script_board_compiler.cpp
    codex/design_script_board_graphic_compiler.cpp
    codex/design_script_board_graphic_proto_generator.cpp
    codex/design_script_board_outline_compiler.cpp
    codex/design_script_board_table_compiler.cpp
    codex/design_script_board_table_proto_generator.cpp
    codex/design_script_board_text_box_compiler.cpp
    codex/design_script_board_text_box_proto_generator.cpp
    codex/design_script_assembly_compiler.cpp
    codex/design_script_capabilities.cpp
    codex/design_script_compiler.cpp
    codex/design_script_context_builder.cpp
    codex/design_script_electrical_analyzer.cpp
    codex/design_script_electrical_compiler.cpp
    codex/design_script_custom_pad_proto_generator.cpp
    codex/design_script_device_code_compiler.cpp
    codex/design_release_writer.cpp
    codex/design_script_footprint_backdrill_generator.cpp
    codex/design_script_footprint_component_classes_compiler.cpp
    codex/design_script_footprint_component_classes_generator.cpp
    codex/design_script_footprint_compiler.cpp
    codex/design_script_footprint_graphic_compiler.cpp
    codex/design_script_footprint_graphic_generator.cpp
    codex/design_script_footprint_group_compiler.cpp
    codex/design_script_footprint_group_generator.cpp
    codex/design_script_footprint_hole_treatment_compiler.cpp
    codex/design_script_footprint_hole_treatment_generator.cpp
    codex/design_script_footprint_custom_pad_compiler.cpp
    codex/design_script_footprint_custom_pad_generator.cpp
    codex/design_script_footprint_library_generator.cpp
    codex/design_script_footprint_pad_compiler.cpp
    codex/design_script_footprint_pad_generator.cpp
    codex/design_script_footprint_padstack_compiler.cpp
    codex/design_script_footprint_padstack_generator.cpp
    codex/design_script_footprint_property_compiler.cpp
    codex/design_script_footprint_property_generator.cpp
    codex/design_script_footprint_settings_compiler.cpp
    codex/design_script_footprint_settings_generator.cpp
    codex/design_script_footprint_text_compiler.cpp
    codex/design_script_footprint_text_generator.cpp
    codex/design_script_footprint_variant_compiler.cpp
    codex/design_script_footprint_variant_generator.cpp
    codex/design_script_footprint_zone_compiler.cpp
    codex/design_script_footprint_zone_generator.cpp
    codex/design_script_layout_analyzer.cpp
    codex/design_script_layout_compiler.cpp
    codex/design_script_pcb_planner.cpp
    codex/design_script_pcb_reconciler.cpp
    codex/design_script_physical_synthesis_compiler.cpp
    codex/design_script_physical_synthesizer.cpp
    codex/design_script_production_compiler.cpp
    codex/design_script_schematic_planner.cpp
    codex/design_script_schematic_reconciler.cpp
    codex/design_script_simulation_compiler.cpp
    codex/design_script_simulation_runner.cpp
    codex/design_script_symbol_compiler.cpp
    codex/design_script_symbol_field_compiler.cpp
    codex/design_script_symbol_field_generator.cpp
    codex/design_script_symbol_graphics_compiler.cpp
    codex/design_script_symbol_graphics_generator.cpp
    codex/design_script_symbol_library_generator.cpp
    codex/design_script_symbol_text_compiler.cpp
    codex/design_script_symbol_text_generator.cpp
    codex/design_script_symbol_resolver.cpp
    codex/design_script_teardrop_compiler.cpp
    codex/design_script_teardrop_generator.cpp
    codex/design_script_via_backdrill_compiler.cpp
    codex/design_script_via_padstack_compiler.cpp
    codex/design_script_via_protection_compiler.cpp
    codex/fabrication_artifact_validator.cpp
    codex/legacy_bom_artifact_validator.cpp
    codex/mechanical_artifact_validator.cpp
    codex/managed_footprint_library_io.cpp
    codex/managed_symbol_library_io.cpp
    codex/schematic_bom_artifact_validator.cpp
    codex/schematic_dxf_artifact_validator.cpp
    codex/schematic_pdf_artifact_validator.cpp
    codex/schematic_ps_artifact_validator.cpp
    codex/schematic_svg_artifact_validator.cpp
    codex/stepz_artifact_validator.cpp
    codex/three_d_pdf_artifact_validator.cpp
    codex/u3d_artifact_validator.cpp
    codex/kicad_ipc_client.cpp
    codex/kicad_ipc_transaction.cpp
    codex/project_settings_ipc.cpp
    codex/production_package_writer.cpp
    codex/codex_agent_policy.cpp
    codex/codex_panel.cpp
    codex/codex_user_input_dialog.cpp
    codex/codex_paths.cpp
    codex/codex_thread_store.cpp
    codex/codex_tool_registry.cpp
    codex/codex_tool_support.cpp
    codex/kds_editor_panel.cpp
    codex/kds_editor_text.cpp
    codex/lossless_sexpr_document.cpp
    dialogs/dialog_destination.cpp
    dialogs/dialog_destination_base.cpp
    dialogs/dialog_copyfiles_job_settings.cpp
    dialogs/dialog_copyfiles_job_settings_base.cpp
    dialogs/dialog_executecommand_job_settings.cpp
    dialogs/dialog_executecommand_job_settings_base.cpp
    dialogs/dialog_update_notice_base.cpp
    dialogs/dialog_update_notice.cpp
    dialogs/dialog_template_selector_base.cpp
    dialogs/dialog_template_selector.cpp
    dialogs/dialog_edit_cfg.cpp
    dialogs/panel_kicad_launcher_base.cpp
    dialogs/panel_kicad_launcher.cpp
    dialogs/panel_jobset_base.cpp
    dialogs/panel_jobset.cpp
    files-io.cpp
    jobs_runner.cpp
    import_proj.cpp
    import_project.cpp
    kicad_manager_frame.cpp
    local_history_pane.cpp
    menubar.cpp
    project_template.cpp
    project_tree_pane.cpp
    project_tree.cpp
    project_tree_item.cpp
    project_tree_traverser.cpp
    update_manager.cpp
    toolbars_kicad_manager.cpp
    tools/kicad_manager_actions.cpp
    tools/kicad_manager_control.cpp
    )

set( KICAD_CLI_SRCS
    cli/command.cpp
    cli/command_jobset_run.cpp
    cli/command_pcb_export_base.cpp
    cli/command_pcb_drc.cpp
    cli/command_pcb_render.cpp
    cli/command_pcb_export_3d.cpp
    cli/command_pcb_export_drill.cpp
    cli/command_pcb_export_dxf.cpp
    cli/command_pcb_export_gerber.cpp
    cli/command_pcb_export_gerbers.cpp
    cli/command_pcb_export_gencad.cpp
    cli/command_pcb_export_hpgl.cpp
    cli/command_pcb_export_ipc2581.cpp
    cli/command_pcb_export_ipcd356.cpp
    cli/command_pcb_export_odb.cpp
    cli/command_pcb_export_pdf.cpp
    cli/command_pcb_export_pos.cpp
    cli/command_pcb_export_ps.cpp
    cli/command_pcb_export_stats.cpp
    cli/command_pcb_export_svg.cpp
    cli/command_pcb_upgrade.cpp
    cli/command_pcb_import.cpp
    cli/command_fp_export_svg.cpp
    cli/command_fp_upgrade.cpp
    cli/command_sch_upgrade.cpp
    cli/command_sch_export_bom.cpp
    cli/command_sch_export_pythonbom.cpp
    cli/command_sch_export_netlist.cpp
    cli/command_sch_export_plot.cpp
    cli/command_sch_erc.cpp
    cli/command_sym_export_svg.cpp
    cli/command_sym_upgrade.cpp
    cli/command_version.cpp

    jobs_runner.cpp
    )

if( WIN32 )
    if( MINGW )
        # KICAD_RESOURCES variable is set by the macro.
        mingw_resource_compiler( kicad )
    else()
        set( KICAD_RESOURCES ${CMAKE_SOURCE_DIR}/resources/msw/kicad.rc )
    endif()
endif()

if( APPLE )
    set( KICAD_RESOURCES kicad.icns kicad_doc.icns )
    set_source_files_properties( "${CMAKE_CURRENT_SOURCE_DIR}/kicad.icns" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
        )
    set_source_files_properties( "${CMAKE_CURRENT_SOURCE_DIR}/kicad_doc.icns"  PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
        )
    set( MACOSX_BUNDLE_ICON_FILE kicad.icns )
    set( MACOSX_BUNDLE_GUI_IDENTIFIER org.kicad.kicad )
    set( MACOSX_BUNDLE_NAME kicad )
endif()

add_executable( kicad WIN32 MACOSX_BUNDLE
    kicad.cpp
    ${KICAD_SRCS}
    ${KICAD_EXTRA_SRCS}
    ${KICAD_RESOURCES}
    )

add_executable( kicad-cli WIN32
    kicad_cli.cpp
    ${KICAD_CLI_SRCS}
    ${KICAD_EXTRA_SRCS}
    ${KICAD_RESOURCES}
    )

if( MSVC )
   # The cli needs subsystem:console or else we can't link wmain/main
   set_target_properties(kicad-cli PROPERTIES COMPILE_DEFINITIONS_DEBUG "_CONSOLE")
   set_target_properties(kicad-cli PROPERTIES COMPILE_DEFINITIONS_RELWITHDEBINFO "_CONSOLE")
   set_target_properties(kicad-cli PROPERTIES LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
   set_target_properties(kicad-cli PROPERTIES LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:CONSOLE")
   set_target_properties(kicad-cli PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE")
   set_target_properties(kicad-cli PROPERTIES LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:CONSOLE")

   # Allow running from build dir
   add_custom_command( TARGET kicad POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:kiapi>" "$<TARGET_FILE_DIR:kicad>"
        )
endif()

if( UNIX )
    # for build directory: create kiface symlinks so kicad (exe) can be run in-situ
    add_custom_target( kiface_sym_links
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_BINARY_DIR}/eeschema/_eeschema.kiface" "${CMAKE_BINARY_DIR}/kicad/_eeschema.kiface"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_BINARY_DIR}/pcbnew/_pcbnew.kiface" "${CMAKE_BINARY_DIR}/kicad/_pcbnew.kiface"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_BINARY_DIR}/cvpcb/_cvpcb.kiface" "${CMAKE_BINARY_DIR}/kicad/_cvpcb.kiface"
        COMMENT "Making <build-dir>/kicad/<kiface.symlinks>"
        )
    add_dependencies( kicad kiface_sym_links )
    add_dependencies( kicad-cli kiface_sym_links )
endif()

if( APPLE )
    set_target_properties( kicad PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${PROJECT_BINARY_DIR}/kicad/Info.plist
        )
    target_link_libraries( kicad
        nlohmann_json
        common
        pads_common
        kicommon
        kinng
        kiapi
        Boost::filesystem
        core
        ${wxWidgets_LIBRARIES}
        )

    set_target_properties( kicad-cli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${OSX_BUNDLE_BUILD_BIN_DIR}
        )

    target_link_libraries( kicad-cli
        nlohmann_json
        common
        pads_common
        ${wxWidgets_LIBRARIES}
        core
        )
else()
    target_link_libraries( kicad
        nlohmann_json
        common
        gal
        common      #repeated due to a circular dependency between gal and common
        pads_common
        kicommon
        kinng
        kiapi
        Boost::filesystem
        core
        ${wxWidgets_LIBRARIES}
        )

    target_link_libraries( kicad-cli
        nlohmann_json
        common
        gal
        common      #repeated due to a circular dependency between gal and common
        pads_common
        kicommon
        core
        ${wxWidgets_LIBRARIES}
        )
endif()

target_link_libraries( kicad ZLIB::ZLIB )
target_link_libraries( kicad pcm )
target_link_libraries( kicad-cli pcm )

if( MSVC )
    # kicommon exports nlohmann's inline basic_json members: KICOMMON_API classes such
    # as PARAM_LAYER_PRESET derive from PARAM_LAMBDA<nlohmann::json>, and exporting a
    # class drags its template base into the export table.  The codex sources use
    # nlohmann directly and emit their own copies, so the link sees each function both
    # imported and defined locally (LNK2005 -> LNK1169).  Both copies come from the same
    # header compiled with the same flags, so letting the linker keep one is safe.
    # Verified still required: removing this reproduces 45 LNK2005 errors.
    # Removing it for real means un-exporting those parameter classes from kicommon.
    target_link_options( kicad PRIVATE /FORCE:MULTIPLE )
endif()

install( TARGETS kicad
    DESTINATION ${KICAD_BIN}
    COMPONENT binary
    )

if( NOT APPLE )
    install( TARGETS kicad-cli
        DESTINATION ${KICAD_BIN}
        COMPONENT binary
        )
endif()

if( KICAD_WIN32_INSTALL_PDBS )
    # Get the PDBs to copy over for MSVC
    install(FILES $<TARGET_PDB_FILE:kicad> DESTINATION ${KICAD_BIN})
endif()

if( APPLE )
    # "install( CODE ... )" will launch its own CMake, so no variables from
    # this CMake instance are accessible... use helper to transfer
    set( SCRIPTING_HELPER "1" )

    set( SPICE_HELPER "1" )

    if( PYTHON_FRAMEWORK )
        set( PYTHON_FRAMEWORK_HELPER "1" )
    else()
        set( PYTHON_FRAMEWORK_HELPER "0" )
    endif()

    set_target_properties( kicad PROPERTIES BUILD_RPATH ${PYTHON_FRAMEWORK} )
    set_target_properties( kicad PROPERTIES INSTALL_RPATH
            "@executable_path/../Frameworks;@executable_path/../Frameworks/Python.framework" )
    set_target_properties( kicad-cli PROPERTIES BUILD_RPATH ${PYTHON_FRAMEWORK} )
    set_target_properties( kicad-cli PROPERTIES INSTALL_RPATH
            "@executable_path/../Frameworks;@executable_path/../Frameworks/Python.framework" )

    install( CODE "
        set( KICAD_CMAKE_MODULE_PATH \"${KICAD_CMAKE_MODULE_PATH}\" )
        set( OSX_BUNDLE_INSTALL_BIN_DIR \"${OSX_BUNDLE_INSTALL_BIN_DIR}\" )
        set( OSX_BUNDLE_INSTALL_LIB_DIR \"${OSX_BUNDLE_INSTALL_LIB_DIR}\" )
        set( PYTHON_FRAMEWORK \"${PYTHON_FRAMEWORK}\" )
    " )

    install( CODE [[
        include( ${KICAD_CMAKE_MODULE_PATH}/InstallSteps/InstallMacOS.cmake )

        # Install the main bundle
        install_runtime_deps( "${OSX_BUNDLE_INSTALL_BIN_DIR}/kicad"
            ""
            ""
            )

        # Now install Python framework properly (GET_RUNTIME_DEPENDENCIES can't do this)
        execute_process( COMMAND rm ${OSX_BUNDLE_INSTALL_LIB_DIR}/Python )
        execute_process( COMMAND cp -RP ${PYTHON_FRAMEWORK} ${OSX_BUNDLE_INSTALL_LIB_DIR}/ )
    ]] )

    # move all individual app bundles into main bundle
    install( CODE "
        # helper function to move a bundle into main bundle
        function( move_to_main_bundle bundle_name symlink_name )
            message( STATUS \"Moving \${bundle_name} into main bundle...\" )
            file( MAKE_DIRECTORY ${OSX_BUNDLE_INSTALL_DIR}/${OSX_BUNDLE_APP_DIR} )
            file( REMOVE_RECURSE ${KICAD_BIN}/\${bundle_name}/${OSX_BUNDLE_LIB_DIR} )
            file( RENAME ${KICAD_BIN}/\${bundle_name} ${OSX_BUNDLE_INSTALL_DIR}/${OSX_BUNDLE_APP_DIR}/\${bundle_name} )
            execute_process( COMMAND ${CMAKE_COMMAND} -E create_symlink \"../../../Frameworks\" \"Frameworks\"
                WORKING_DIRECTORY ${OSX_BUNDLE_INSTALL_DIR}/${OSX_BUNDLE_APP_DIR}/\${bundle_name}/Contents
            )
            # create a top-level link pointing inside main bundle
            execute_process( COMMAND ${CMAKE_COMMAND} -E create_symlink \"${OSX_BUNDLE_MAIN}/${OSX_BUNDLE_APP_DIR}/\${bundle_name}\" \"\${symlink_name}\"
                WORKING_DIRECTORY ${KICAD_BIN}
            )
        endfunction( move_to_main_bundle )

        # move all app bundles
        move_to_main_bundle( \"bitmap2component.app\" \"Image Converter.app\" )
        move_to_main_bundle( \"eeschema.app\" \"Schematic Editor.app\" )
        move_to_main_bundle( \"gerbview.app\" \"GerbView.app\" )
        move_to_main_bundle( \"pcb_calculator.app\" \"PCB Calculator.app\" )
        move_to_main_bundle( \"pcbnew.app\" \"PCB Editor.app\" )
        move_to_main_bundle( \"pl_editor.app\" \"Page Layout Editor.app\" )

        cmake_policy( SET CMP0012 NEW )
        if ( ${PYTHON_FRAMEWORK_HELPER} AND ${KICAD_APPLE_MAKE_RELOCATEABLE_BUNDLE} )
            message( STATUS \"Making the finished bundle relocateable...\" )
            include( ${KICAD_CMAKE_MODULE_PATH}/InstallSteps/RefixupMacOS.cmake )
            refix_kicad_bundle( ${OSX_BUNDLE_INSTALL_DIR} )
        endif( )

        " COMPONENT Runtime
        )
endif()
