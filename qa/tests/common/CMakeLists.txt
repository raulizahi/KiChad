# This program source code file is part of KiCad, a free EDA CAD application.
#
# Copyright (C) 2018, 2023 KiCad Developers, see CHANGELOG.TXT for contributors.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, you may find one here:
# http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
# or you may search the http://www.gnu.org website for the version 2 license,
# or you may write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

find_package( wxWidgets 3.0.0 COMPONENTS gl aui adv html core net base xml stc REQUIRED )

set( QA_COMMON_SRCS
    # This is needed for the global mock objects
    ${CMAKE_SOURCE_DIR}/qa/mocks/kicad/common_mocks.cpp

    # The main test entry points
    test_module.cpp

    wximage_test_utils.cpp

    test_angle_item.cpp

    test_array_axis.cpp
    test_backup_settings.cpp
    test_base_set.cpp
    test_bitmap_base.cpp
    test_board_render_artifact_validator.cpp
    test_board_ps_artifact_validator.cpp
    test_clipboard.cpp
    test_collector.cpp
    test_color4d.cpp
    test_commit.cpp
    test_codex_app_server_client.cpp
    test_codex_agent_policy.cpp
    test_codex_tool_fabricate.cpp
    test_codex_tool_registry.cpp
    test_codex_thread_store.cpp
    test_fabrication_artifact_validator.cpp
    test_legacy_bom_artifact_validator.cpp
    test_mechanical_artifact_validator.cpp
    test_schematic_bom_artifact_validator.cpp
    test_schematic_dxf_artifact_validator.cpp
    test_schematic_pdf_artifact_validator.cpp
    test_schematic_ps_artifact_validator.cpp
    test_schematic_svg_artifact_validator.cpp
    test_stepz_artifact_validator.cpp
    test_three_d_pdf_artifact_validator.cpp
    test_u3d_artifact_validator.cpp
    test_design_script_compiler.cpp
    test_design_script_footprint_authoring.cpp
    test_design_script_pcb_planner.cpp
    test_design_script_pcb_reconciler.cpp
    test_design_script_schematic_planner.cpp
    test_design_script_schematic_reconciler.cpp
    test_design_script_symbol_resolver.cpp
    test_design_script_symbol_fields.cpp
    test_design_script_symbol_graphics.cpp
    test_design_script_symbol_library_generator.cpp
    test_design_script_symbol_metadata.cpp
    test_design_script_symbol_text.cpp
    test_managed_footprint_library_io.cpp
    test_managed_symbol_library_io.cpp
    test_coroutine.cpp
    test_eda_shape.cpp
    test_eda_text.cpp
    test_embedded_file_compress.cpp
    test_expand_text_vars.cpp
    test_file_history.cpp
    test_filename_resolver.cpp
    test_hotkey_store.cpp
    test_increment.cpp
    test_ki_any.cpp
    test_library_tables.cpp
    test_lockfile.cpp
    test_loop_safe_collect.cpp
    test_lossless_sexpr_document.cpp
    test_markup_parser.cpp
    test_kicad_string.cpp
    test_kicad_stroke_font.cpp
    test_kds_editor_text.cpp
    test_outline_font.cpp
    test_dxf_plotter.cpp
    test_gerber_plotter.cpp
    test_pdf_unicode_plot.cpp
    test_kiid.cpp
    test_layer_ids.cpp
    test_lib_id.cpp
    test_layer_range.cpp
    test_lset.cpp
    test_markup_parser.cpp
    test_netclass_serialization.cpp
    test_notifications_manager.cpp
    test_property.cpp
    test_property_holder.cpp
    test_reporting.cpp
    test_refdes_utils.cpp
    test_grid_helper.cpp
    test_string_utils.cpp
    test_richio.cpp
    test_text_attributes.cpp
    git/test_libgit_backend.cpp
    text_eval/test_text_eval_parser.cpp
    text_eval/test_text_eval_parser_core.cpp
    text_eval/test_text_eval_parser_datetime.cpp
    text_eval/test_text_eval_parser_vcs.cpp
    text_eval/test_text_eval_parser_integration.cpp
    text_eval/test_text_eval_numeric_compat.cpp
    text_eval/test_text_eval_render.cpp
    test_title_block.cpp
    test_types.cpp
    test_utf8.cpp
    test_wildcards_and_files_ext.cpp
    test_wx_filename.cpp

    ${CMAKE_SOURCE_DIR}/kicad/codex/lossless_sexpr_document.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_app_server_client.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_paths.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_protocol_log.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_thread_store.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_tool_design.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_tool_fabricate.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_tool_inspect.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_tool_layout.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_tool_pcb.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_tool_project.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_tool_verify.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_board_asset_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_board_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_board_graphic_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_board_graphic_proto_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_board_outline_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_board_table_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_board_table_proto_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_board_text_box_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_board_text_box_proto_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_assembly_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_capabilities.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_context_builder.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_electrical_analyzer.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_electrical_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_custom_pad_proto_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_device_code_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_release_writer.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_backdrill_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_component_classes_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_component_classes_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_graphic_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_graphic_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_group_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_group_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_hole_treatment_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_hole_treatment_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_custom_pad_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_custom_pad_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_library_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_pad_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_pad_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_padstack_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_padstack_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_property_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_property_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_settings_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_settings_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_text_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_text_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_variant_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_variant_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_zone_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_footprint_zone_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_layout_analyzer.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_layout_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_pcb_planner.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_pcb_reconciler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_physical_synthesis_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_physical_synthesizer.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_production_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_schematic_planner.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_schematic_reconciler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_simulation_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_simulation_runner.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_symbol_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_symbol_field_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_symbol_field_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_symbol_graphics_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_symbol_graphics_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_symbol_library_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_symbol_text_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_symbol_text_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_symbol_resolver.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_teardrop_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_teardrop_generator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_via_backdrill_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_via_padstack_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/design_script_via_protection_compiler.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_tool_registry.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_agent_policy.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/codex_tool_support.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/production_package_writer.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/board_render_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/board_ps_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/fabrication_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/legacy_bom_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/mechanical_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/managed_footprint_library_io.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/managed_symbol_library_io.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/schematic_bom_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/schematic_dxf_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/schematic_pdf_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/schematic_ps_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/schematic_svg_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/stepz_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/three_d_pdf_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/u3d_artifact_validator.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/kicad_ipc_transaction.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/kds_editor_text.cpp
    ${CMAKE_SOURCE_DIR}/kicad/codex/project_settings_ipc.cpp

    libeval/test_numeric_evaluator.cpp

    io/test_csv.cpp
    io/altium/test_altium_parser.cpp
    io/altium/test_altium_parser_utils.cpp
    io/cadstar/test_cadstar_parts_parser.cpp

    io/cadstar/test_cadstar_archive_parser.cpp

    io/pads/test_pads_attribute_mapper.cpp
    io/pads/test_pads_common.cpp
    io/pads/test_pads_unit_converter.cpp

    test_kigit_common.cpp
    test_kigit_orphan_registry.cpp
    test_throttle.cpp
    view/test_zoom_controller.cpp

    test_netname_validator.cpp

    # GAL tests
    test_gal_xor_mode.cpp

    # Graphics import tests
    test_graphics_importer_buffer.cpp

    # Project template tests (source included directly since it's from kicad executable)
    test_project_template.cpp
    ${CMAKE_SOURCE_DIR}/kicad/project_template.cpp

    # Project file tests
    test_project_file.cpp
    test_remote_provider_metadata.cpp
    test_remote_provider_client.cpp
    test_remote_provider_settings.cpp
    test_oauth_loopback_server.cpp
    test_oauth_pkce.cpp
    test_secure_token_store.cpp

    # Job tests
    test_job_export_pcb_variants.cpp
    test_job_export_sch_plot.cpp
    test_job_sch_erc.cpp
    test_jobs_runner.cpp
    test_jobs_output_archive.cpp
    test_kicad_ipc_client.cpp

    ${CMAKE_SOURCE_DIR}/kicad/codex/kicad_ipc_client.cpp
)

if( KICAD_TEST_DATABASE_LIBRARIES )
    set( QA_COMMON_SRCS ${QA_COMMON_SRCS} test_database.cpp )
endif()

if( WIN32 )
    # We want to declare a resource manifest on Windows to enable UTF8 mode
    # Without UTF8 mode, some random IO tests may fail, we set the active code page on
    # normal kicad to UTF8 as well
    if( MINGW )
        # QA_COMMON_RESOURCES variable is set by the macro.
        mingw_resource_compiler( qa_common )
    else()
        set( QA_COMMON_RESOURCES ${CMAKE_SOURCE_DIR}/resources/msw/qa_common.rc )
    endif()
endif()


# Test executable for non-program-specific code
add_executable( qa_common
    ${QA_COMMON_SRCS}
    ${QA_COMMON_RESOURCES} )

target_link_libraries( qa_common
    common
    libcontext
    gal
    pads_common
    qa_utils
    kinng
    kiapi
    Boost::headers
    Boost::unit_test_framework
    ${wxWidgets_LIBRARIES}
    ZLIB::ZLIB
    )

if( TARGET api_schema_build_copy )
    add_dependencies( qa_common api_schema_build_copy )
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/kicad
    ${CMAKE_SOURCE_DIR}/pcbnew
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/common/io/pads
    ${CMAKE_SOURCE_DIR}/qa/mocks/include
    ${INC_AFTER}
)

set_source_files_properties( test_remote_provider_metadata.cpp test_remote_provider_client.cpp PROPERTIES
        COMPILE_DEFINITIONS "QA_SRC_ROOT=(\"${CMAKE_SOURCE_DIR}\")"
        )

if( KICAD_TEST_DATABASE_LIBRARIES )
    set_source_files_properties( test_database.cpp PROPERTIES
            COMPILE_DEFINITIONS "QA_DATABASE_FILE_LOCATION=(\"${CMAKE_SOURCE_DIR}/qa/data/dblib\")"
            )
endif()

kicad_add_boost_test( qa_common qa_common )

setup_qa_env( qa_common )
