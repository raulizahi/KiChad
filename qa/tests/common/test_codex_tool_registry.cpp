/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <kicad/codex/codex_tool_internal.h>
#include <kicad/codex/codex_tool_registry.h>
#include <kicad/codex/design_script_pcb_planner.h>
#include <kicad/codex/kicad_ipc_client.h>
#include <kicad/codex/managed_footprint_library_io.h>
#include <kicad/codex/project_settings_ipc.h>

#include <atomic>
#include <build_version.h>
#include <import_export.h>
#include <api/board/board_commands.pb.h>
#include <api/board/board_types.pb.h>
#include <api/common/envelope.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/common/types/project_settings.pb.h>
#include <google/protobuf/empty.pb.h>
#include <kiid.h>
#include <kinng.h>
#include <filesystem>
#include <limits>
#include <map>
#include <regex>
#include <wx/datetime.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/utils.h>


namespace
{

using JSON = nlohmann::json;


class SCOPED_ENVIRONMENT
{
public:
    SCOPED_ENVIRONMENT( wxString aName, const wxString& aValue ) :
            m_name( std::move( aName ) ),
            m_hadValue( wxGetEnv( m_name, &m_previous ) )
    {
        wxSetEnv( m_name, aValue );
    }

    ~SCOPED_ENVIRONMENT()
    {
        if( m_hadValue )
            wxSetEnv( m_name, m_previous );
        else
            wxUnsetEnv( m_name );
    }

private:
    wxString m_name;
    wxString m_previous;
    bool     m_hadValue;
};


JSON envelope( const JSON& aResult )
{
    return JSON::parse( aResult.at( "contentItems" ).at( 0 ).at( "text" ).get<std::string>() );
}


kiapi::board::BoardStackup mockTwoLayerStackup( const std::string& aFinish )
{
    kiapi::board::BoardStackup stackup;
    stackup.mutable_finish()->set_type_name( aFinish );
    stackup.mutable_impedance()->set_is_controlled( false );
    stackup.mutable_edge()->mutable_plating()->set_has_edge_plating( false );
    auto* copper = stackup.add_layers();
    copper->set_layer( kiapi::board::types::BL_F_Cu );
    copper->set_enabled( true );
    copper->set_type( kiapi::board::BSLT_COPPER );
    copper->set_type_name( "copper" );
    copper->mutable_thickness()->set_value_nm( 35000 );
    auto* dielectric = stackup.add_layers();
    dielectric->set_layer( kiapi::board::types::BL_UNDEFINED );
    dielectric->set_enabled( true );
    dielectric->set_type( kiapi::board::BSLT_DIELECTRIC );
    dielectric->set_type_name( "core" );
    dielectric->set_dielectric_layer_id( 1 );
    dielectric->mutable_thickness()->set_value_nm( 1530000 );
    auto* properties = dielectric->mutable_dielectric()->add_layer();
    properties->mutable_thickness()->set_value_nm( 1530000 );
    properties->set_material_name( "FR4" );
    properties->set_epsilon_r( 4.5 );
    properties->set_loss_tangent( 0.02 );
    auto* bottom = stackup.add_layers();
    bottom->set_layer( kiapi::board::types::BL_B_Cu );
    bottom->set_enabled( true );
    bottom->set_type( kiapi::board::BSLT_COPPER );
    bottom->set_type_name( "copper" );
    bottom->mutable_thickness()->set_value_nm( 35000 );
    return stackup;
}


kiapi::board::BoardDesignRules mockBoardRules( int64_t aMinimumClearance )
{
    kiapi::board::BoardDesignRules rules;
    rules.mutable_minimum_clearance()->set_value_nm( aMinimumClearance );
    rules.mutable_minimum_connection_width()->set_value_nm( 200000 );
    rules.mutable_minimum_track_width()->set_value_nm( 200000 );
    rules.mutable_minimum_via_annular_width()->set_value_nm( 100000 );
    rules.mutable_minimum_via_diameter()->set_value_nm( 600000 );
    rules.mutable_minimum_through_hole_diameter()->set_value_nm( 300000 );
    rules.mutable_minimum_microvia_diameter()->set_value_nm( 300000 );
    rules.mutable_minimum_microvia_drill()->set_value_nm( 100000 );
    rules.mutable_minimum_hole_to_hole()->set_value_nm( 250000 );
    rules.mutable_minimum_copper_to_hole_clearance()->set_value_nm( 250000 );
    rules.mutable_minimum_silkscreen_clearance()->set_value_nm( 0 );
    rules.mutable_minimum_groove_width()->set_value_nm( 250000 );
    rules.set_minimum_resolved_spokes( 2 );
    rules.mutable_minimum_silkscreen_text_height()->set_value_nm( 800000 );
    rules.mutable_minimum_silkscreen_text_thickness()->set_value_nm( 80000 );
    rules.set_copper_edge_clearance_mode( kiapi::board::BCECM_EXPLICIT );
    rules.mutable_minimum_copper_to_edge_clearance()->set_value_nm( 500000 );
    rules.set_use_height_for_length_calculations( true );
    rules.mutable_maximum_error()->set_value_nm( 5000 );
    rules.set_allow_fillets_outside_zone_outline( false );
    return rules;
}


kiapi::common::project::NetClassSettings mockNetClassSettings( int64_t aClearance )
{
    kiapi::common::project::NetClassSettings settings;
    auto* netClass = settings.add_net_classes();
    netClass->set_name( "Default" );
    netClass->set_priority( std::numeric_limits<int32_t>::max() );
    netClass->set_type( kiapi::common::project::NCT_EXPLICIT );
    netClass->mutable_board()->mutable_clearance()->set_value_nm( aClearance );
    return settings;
}


std::string readExactTextFile( const wxFileName& aPath )
{
    wxFile file( aPath.GetFullPath(), wxFile::read );
    BOOST_REQUIRE_MESSAGE( file.IsOpened(), aPath.GetFullPath() );
    const wxFileOffset length = file.Length();
    BOOST_REQUIRE_GE( length, 0 );
    std::string text( static_cast<size_t>( length ), '\0' );

    if( length > 0 )
        BOOST_REQUIRE_EQUAL( file.Read( text.data(), text.size() ), length );

    return text;
}


class TOOL_PROJECT_FIXTURE
{
public:
    TOOL_PROJECT_FIXTURE()
    {
        wxFileName root = wxFileName::DirName( wxFileName::GetTempDir() );
        root.AppendDir( wxS( "kichad-codex-tools-" ) + KIID().AsString() );
        m_root = root.GetFullPath();
        BOOST_REQUIRE( wxFileName::Mkdir( m_root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

        write( wxS( "design.kicad_pro" ), wxS( "{}\n" ) );
        write( wxS( "design.kicad_sch" ),
               wxS( "(kicad_sch (version 20260306)\n"
                    "  (generator \"eeschema\")\n"
                    "  (generator_version \"10.0\")\n"
                    ")\n" ) );
        write( wxS( "design.kicad_pcb" ),
               wxS( "(kicad_pcb (version 20260206)\n"
                    "  (general (thickness 1.6))\n"
                    "  (footprint \"Package_SO:SOIC-8_3.9x4.9mm_P1.27mm\"\n"
                    "    (property \"Reference\" \"U1\")\n"
                    "    (unknown_extension keep-me))\n"
                    "  (segment (start 1 2) (end 3 4) (width 0.25))\n"
                    ")\n" ) );
        write( wxS( "empty.kicad_pcb" ), wxString() );
        write( wxS( "wrong-root.kicad_pcb" ), wxS( "(kicad_sch)\n" ) );

        wxFileName outsideRoot = wxFileName::DirName( m_root );
        wxString   rootName = outsideRoot.GetDirs().Last();
        outsideRoot.RemoveLastDir();
        m_outside = wxFileName( outsideRoot.GetFullPath(),
                                rootName + wxS( "-outside.kicad_pcb" ) )
                            .GetFullPath();
        wxFFile outsideFile( m_outside, wxS( "wb" ) );
        BOOST_REQUIRE( outsideFile.IsOpened() );
        BOOST_REQUIRE( outsideFile.Write( wxS( "(kicad_pcb)\n" ) ) );
    }

    ~TOOL_PROJECT_FIXTURE()
    {
        wxRemoveFile( m_outside );
        wxFileName::Rmdir( m_root, wxPATH_RMDIR_RECURSIVE );
    }

    const wxString& Root() const { return m_root; }

    wxString OutsideRelativePath() const
    {
        return wxS( "../" ) + wxFileName( m_outside ).GetFullName();
    }

    const wxString& OutsidePath() const { return m_outside; }

private:
    void write( const wxString& aName, const wxString& aContent )
    {
        wxFFile file( wxFileName( m_root, aName ).GetFullPath(), wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aContent ) );
    }

    wxString m_root;
    wxString m_outside;
};

} // namespace


BOOST_AUTO_TEST_SUITE( CodexToolRegistry )


BOOST_AUTO_TEST_CASE( AdvertisesOnlyImplementedNativeTools )
{
    CODEX_TOOL_REGISTRY registry( []() { return wxString(); } );
    JSON                specs = registry.Specs();

    BOOST_REQUIRE_EQUAL( specs.size(), 6 );
    BOOST_CHECK_EQUAL( specs[0]["name"].get<std::string>(), "project" );
    BOOST_CHECK_EQUAL( specs[1]["name"].get<std::string>(), "inspect" );
    BOOST_CHECK_EQUAL( specs[2]["name"].get<std::string>(), "design" );
    BOOST_CHECK_EQUAL( specs[3]["name"].get<std::string>(), "pcb" );
    BOOST_CHECK_EQUAL( specs[4]["name"].get<std::string>(), "verify" );
    BOOST_CHECK_EQUAL( specs[5]["name"].get<std::string>(), "fabricate" );
    const JSON& inspectOperations =
            specs[1]["inputSchema"]["properties"]["operation"]["enum"];
    BOOST_REQUIRE_EQUAL( inspectOperations.size(), 3 );
    BOOST_CHECK_EQUAL( inspectOperations[2].get<std::string>(), "render" );
    const JSON& designOperations =
            specs[2]["inputSchema"]["properties"]["operation"]["enum"];
    BOOST_REQUIRE_EQUAL( designOperations.size(), 9 );
    BOOST_CHECK_EQUAL( designOperations[1].get<std::string>(), "context" );
    BOOST_CHECK_EQUAL( designOperations[2].get<std::string>(), "search" );
    BOOST_CHECK_EQUAL( designOperations[3].get<std::string>(), "read" );
    BOOST_CHECK_EQUAL( designOperations[5].get<std::string>(), "preview" );
    BOOST_CHECK_EQUAL( designOperations[7].get<std::string>(), "patch" );
    BOOST_CHECK_EQUAL( designOperations[8].get<std::string>(), "apply" );
    const JSON& verifyOperations =
            specs[4]["inputSchema"]["properties"]["operation"]["enum"];
    BOOST_REQUIRE_EQUAL( verifyOperations.size(), 5 );
    BOOST_CHECK_EQUAL( verifyOperations[0].get<std::string>(), "erc" );
    BOOST_CHECK_EQUAL( verifyOperations[1].get<std::string>(), "electrical" );
    BOOST_CHECK_EQUAL( verifyOperations[2].get<std::string>(), "drc" );
    BOOST_CHECK_EQUAL( verifyOperations[3].get<std::string>(), "layout" );
    BOOST_CHECK_EQUAL( verifyOperations[4].get<std::string>(), "sourcing" );
    const JSON& fabricationOperations =
            specs[5]["inputSchema"]["properties"]["operation"]["enum"];
    BOOST_REQUIRE_EQUAL( fabricationOperations.size(), 2 );
    BOOST_CHECK_EQUAL( fabricationOperations[0].get<std::string>(), "plan" );
    BOOST_CHECK_EQUAL( fabricationOperations[1].get<std::string>(), "export" );
}


BOOST_AUTO_TEST_CASE( AttachesNativePreviewImagesToTheModel )
{
    TOOL_PROJECT_FIXTURE fixture;
    int calls = 0;
    CODEX_TOOL_REGISTRY registry(
            [&fixture]() { return fixture.Root(); }, {}, {}, {}, {}, {}, {}, {},
            [&]( const std::string& aView, const wxFileName& aInput,
                 const std::vector<int>& aPages,
                 const std::vector<wxFileName>& aOutputs, std::string& )
            {
                ++calls;
                BOOST_CHECK_EQUAL( aView, "schematic" );
                BOOST_CHECK_EQUAL( aInput.GetFullName(), wxS( "design.kicad_sch" ) );
                BOOST_REQUIRE_EQUAL( aPages.size(), 1 );
                BOOST_REQUIRE_EQUAL( aOutputs.size(), 1 );
                BOOST_CHECK_EQUAL( aPages.front(), 1 );
                wxFile file( aOutputs.front().GetFullPath(), wxFile::write );
                const unsigned char pngSignature[] = {
                    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
                };
                return file.IsOpened()
                       && file.Write( pngSignature, sizeof( pngSignature ) )
                                  == sizeof( pngSignature );
            } );
    JSON rendered = registry.Handle(
            "inspect", { { "operation", "render" }, { "path", "design.kicad_sch" },
                           { "view", "schematic" } } );
    BOOST_REQUIRE_MESSAGE( rendered.at( "success" ).get<bool>(), rendered.dump() );
    BOOST_REQUIRE_EQUAL( rendered["contentItems"].size(), 2 );
    BOOST_CHECK_EQUAL( rendered["contentItems"][1]["type"].get<std::string>(),
                       "inputImage" );
    BOOST_CHECK( rendered["contentItems"][1]["imageUrl"].get<std::string>()
                         .starts_with( "data:image/png;base64," ) );
    JSON data = envelope( rendered )["data"];
    BOOST_CHECK_EQUAL( data["view"].get<std::string>(), "schematic" );
    BOOST_CHECK_EQUAL( data["previewBytes"].get<int>(), 8 );
    BOOST_CHECK( !data.contains( "previewPath" ) );
    BOOST_CHECK_EQUAL( data["pages"][0]["contentItemIndex"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( calls, 1 );

    JSON mismatch = registry.Handle(
            "inspect", { { "operation", "render" }, { "path", "design.kicad_sch" },
                           { "view", "pcb2d" } } );
    BOOST_CHECK( !mismatch.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( mismatch )["error"]["code"].get<std::string>(),
                       "invalid_path" );
    BOOST_CHECK_EQUAL( calls, 1 );
}


BOOST_AUTO_TEST_CASE( RendersEverySchematicHierarchyPageWhenPageIsOmitted )
{
    TOOL_PROJECT_FIXTURE fixture;
    const auto write = [&]( const wxString& aName, const wxString& aSource )
    {
        wxFFile file( wxFileName( fixture.Root(), aName ).GetFullPath(), wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aSource ) );
    };
    write( wxS( "design.kicad_sch" ),
           wxS( "(kicad_sch (version 20260306)\n"
                "  (generator \"eeschema\")\n"
                "  (sheet\n"
                "    (property \"Sheetname\" \"Power\")\n"
                "    (property \"Sheetfile\" \"power.kicad_sch\")\n"
                "    (instances (project \"design\"\n"
                "      (path \"/power\" (page \"2\")))))\n"
                "  (sheet\n"
                "    (property \"Sheetname\" \"Control\")\n"
                "    (property \"Sheetfile\" \"control.kicad_sch\")\n"
                "    (instances (project \"design\"\n"
                "      (path \"/control\" (page \"3\")))))\n"
                ")\n" ) );
    write( wxS( "power.kicad_sch" ),
           wxS( "(kicad_sch (version 20260306) (generator \"eeschema\"))\n" ) );
    write( wxS( "control.kicad_sch" ),
           wxS( "(kicad_sch (version 20260306) (generator \"eeschema\"))\n" ) );

    int calls = 0;
    CODEX_TOOL_REGISTRY registry(
            [&fixture]() { return fixture.Root(); }, {}, {}, {}, {}, {}, {}, {},
            [&]( const std::string& aView, const wxFileName&,
                 const std::vector<int>& aPages,
                 const std::vector<wxFileName>& aOutputs, std::string& )
            {
                ++calls;
                BOOST_CHECK_EQUAL( aView, "schematic" );
                BOOST_REQUIRE_EQUAL( aPages.size(), 3 );
                BOOST_REQUIRE_EQUAL( aOutputs.size(), 3 );
                BOOST_CHECK_EQUAL( aPages[0], 1 );
                BOOST_CHECK_EQUAL( aPages[1], 2 );
                BOOST_CHECK_EQUAL( aPages[2], 3 );

                const unsigned char pngSignature[] = {
                    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
                };

                for( const wxFileName& output : aOutputs )
                {
                    wxFile file( output.GetFullPath(), wxFile::write );

                    if( !file.IsOpened()
                        || file.Write( pngSignature, sizeof( pngSignature ) )
                                   != sizeof( pngSignature ) )
                    {
                        return false;
                    }
                }

                return true;
            } );
    JSON rendered = registry.Handle(
            "inspect", { { "operation", "render" }, { "path", "design.kicad_sch" },
                           { "view", "schematic" } } );
    BOOST_REQUIRE_MESSAGE( rendered.at( "success" ).get<bool>(), rendered.dump() );
    BOOST_REQUIRE_EQUAL( rendered["contentItems"].size(), 4 );
    const JSON data = envelope( rendered )["data"];
    BOOST_CHECK_EQUAL( data["pageCount"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( data["renderedPages"].get<int>(), 3 );
    BOOST_CHECK( !data["pagesTruncated"].get<bool>() );
    BOOST_CHECK_EQUAL( data["previewBytes"].get<int>(), 24 );
    BOOST_REQUIRE_EQUAL( data["pages"].size(), 3 );
    BOOST_CHECK_EQUAL( data["pages"][0]["sheetPath"].get<std::string>(), "/" );
    BOOST_CHECK_EQUAL( data["pages"][1]["sheetName"].get<std::string>(), "Power" );
    BOOST_CHECK_EQUAL( data["pages"][1]["sourcePath"].get<std::string>(),
                       "power.kicad_sch" );
    BOOST_CHECK_EQUAL( data["pages"][2]["sheetName"].get<std::string>(), "Control" );
    BOOST_CHECK_EQUAL( calls, 1 );
}


BOOST_AUTO_TEST_CASE( InvalidatesSchematicPreviewsWhenAnyHierarchyFileChanges )
{
    TOOL_PROJECT_FIXTURE fixture;
    const auto write = [&]( const wxString& aName, const wxString& aSource )
    {
        wxFFile file( wxFileName( fixture.Root(), aName ).GetFullPath(), wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aSource ) );
    };
    write( wxS( "design.kicad_sch" ),
           wxS( "(kicad_sch (version 20260306)\n"
                "  (generator \"eeschema\")\n"
                "  (symbol (property \"Sheetfile\" \"not-a-child.kicad_sch\"))\n"
                "  (sheet\n"
                "    (property \"Sheetname\" \"Child\")\n"
                "    (property \"Sheetfile\" \"child.kicad_sch\"))\n"
                ")\n" ) );
    write( wxS( "child.kicad_sch" ),
           wxS( "(kicad_sch (version 20260306) (generator \"eeschema\"))\n" ) );

    int calls = 0;
    CODEX_TOOL_REGISTRY registry(
            [&fixture]() { return fixture.Root(); }, {}, {}, {}, {}, {}, {}, {},
            [&]( const std::string&, const wxFileName&, const std::vector<int>& aPages,
                 const std::vector<wxFileName>& aOutputs, std::string& )
            {
                ++calls;
                BOOST_REQUIRE_EQUAL( aPages.size(), 1 );
                BOOST_REQUIRE_EQUAL( aOutputs.size(), 1 );
                BOOST_CHECK_EQUAL( aPages.front(), 2 );
                wxFile file( aOutputs.front().GetFullPath(), wxFile::write );
                const unsigned char pngSignature[] = {
                    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
                };
                return file.IsOpened()
                       && file.Write( pngSignature, sizeof( pngSignature ) )
                                  == sizeof( pngSignature );
            } );
    const JSON request = { { "operation", "render" }, { "path", "design.kicad_sch" },
                           { "view", "schematic" }, { "page", 2 } };
    const JSON first = registry.Handle( "inspect", request );
    BOOST_REQUIRE_MESSAGE( first.at( "success" ).get<bool>(), first.dump() );
    const JSON firstData = envelope( first )["data"];
    BOOST_CHECK_EQUAL( firstData["sourceFiles"].get<int>(), 2 );
    BOOST_CHECK_GT( firstData["sourceBytes"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( firstData["sourceSha256"].get<std::string>().size(), 64 );
    const std::filesystem::path previewDirectory =
            std::filesystem::path( fixture.Root().ToStdString() ) / ".kichad" / "previews";
    std::vector<std::filesystem::path> firstPreviews;

    for( const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator( previewDirectory ) )
    {
        if( entry.is_regular_file() )
            firstPreviews.push_back( entry.path() );
    }

    BOOST_REQUIRE_EQUAL( firstPreviews.size(), 1 );
    const std::filesystem::path firstPreview = firstPreviews.front();

    write( wxS( "child.kicad_sch" ),
           wxS( "(kicad_sch (version 20260306) (generator \"eeschema\")\n"
                "  (text \"changed child revision\"))\n" ) );
    const JSON second = registry.Handle( "inspect", request );
    BOOST_REQUIRE_MESSAGE( second.at( "success" ).get<bool>(), second.dump() );
    const JSON secondData = envelope( second )["data"];
    BOOST_CHECK_NE( firstData["sourceSha256"], secondData["sourceSha256"] );
    BOOST_CHECK_EQUAL( secondData["pages"][0]["contentItemIndex"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( secondData["supersededPreviewsRemoved"].get<int>(), 1 );
    BOOST_CHECK( !std::filesystem::exists( firstPreview ) );
    BOOST_CHECK_EQUAL( calls, 2 );
}


BOOST_AUTO_TEST_CASE( ValidatesAuthoredFootprintModelAssetsBeforePreviewOrApply )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName models = wxFileName::DirName( fixture.Root() );
    models.AppendDir( wxS( "models" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( models.GetFullPath(), 0700 ) );
    wxFFile projectModel( wxFileName( models.GetFullPath(), wxS( "body.step" ) ).GetFullPath(),
                          wxS( "wb" ) );
    BOOST_REQUIRE( projectModel.IsOpened() );
    BOOST_REQUIRE( projectModel.Write( wxS( "ISO-10303-21;\nEND-ISO-10303-21;\n" ) ) );
    projectModel.Close();

    wxFileName stockModels = wxFileName::DirName( fixture.Root() );
    stockModels.AppendDir( wxS( "stock" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( stockModels.GetFullPath(), 0700 ) );
    SCOPED_ENVIRONMENT stockModelEnvironment( wxS( "KICAD10_3DMODEL_DIR" ),
                                               stockModels.GetFullPath() );
    stockModels.AppendDir( wxS( "Resistor_SMD.3dshapes" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( stockModels.GetFullPath(), 0700 ) );
    wxFFile stockModel(
            wxFileName( stockModels.GetFullPath(), wxS( "R_0603_1608Metric.step" ) )
                    .GetFullPath(),
            wxS( "wb" ) );
    BOOST_REQUIRE( stockModel.IsOpened() );
    BOOST_REQUIRE( stockModel.Write( wxS( "ISO-10303-21;\nEND-ISO-10303-21;\n" ) ) );
    stockModel.Close();

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); },
                                  []() { return true; } );
    const std::string validSource = R"KDS((kichad_design
  (version 1) (project model_assets)
  (library footprint Product (table project)
    (uri "${KIPRJMOD}/Product.pretty") (managed true))
  (footprint Product:BODY
    (reference U) (value BODY) (attributes (smd true) (allow_missing_courtyard true))
    (pad p1 (number 1) (type smd) (shape rect) (at 0mm 0mm)
      (size 1mm 1mm) (layers F.Cu F.Mask F.Paste))
    (model "${KIPRJMOD}/models/body.step")
    (model "${KICAD10_3DMODEL_DIR}/Resistor_SMD.3dshapes/R_0603_1608Metric.step"))
))KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "models.kicad_kds" },
                                                { "source", validSource } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON preview = registry.Handle( "design", { { "operation", "preview" },
                                                  { "path", "models.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( preview.at( "success" ).get<bool>(), preview.dump() );
    const JSON modelAssets = envelope( preview )["data"]["footprintModelAssets"];
    BOOST_CHECK_EQUAL( modelAssets["checked"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( modelAssets["project"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( modelAssets["stock"].get<int>(), 1 );

    std::string missingSource = validSource;
    const std::string installed = "R_0603_1608Metric.step";
    const size_t installedPosition = missingSource.find( installed );
    BOOST_REQUIRE_NE( installedPosition, std::string::npos );
    missingSource.replace( installedPosition, installed.size(), "MISSING_MODEL.step" );
    saved = registry.Handle( "design", { { "operation", "save" },
                                           { "path", "models.kicad_kds" },
                                           { "source", missingSource },
                                           { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    JSON missing = registry.Handle( "design", { { "operation", "preview" },
                                                  { "path", "models.kicad_kds" } } );
    BOOST_CHECK( !missing.at( "success" ).get<bool>() );
    const JSON error = envelope( missing )["error"];
    BOOST_CHECK_EQUAL( error["code"].get<std::string>(),
                       "footprint_model_asset_unavailable" );
    BOOST_CHECK_NE( error["message"].get<std::string>().find( "MISSING_MODEL.step" ),
                    std::string::npos );
    BOOST_CHECK_NE( error["message"].get<std::string>().find( "Product:BODY" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( RendersRealNativePreviewsWhenRequested )
{
    wxString enabled;

    if( !wxGetEnv( wxS( "KICHAD_QA_NATIVE_PREVIEW" ), &enabled ) || enabled != wxS( "1" ) )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in native preview integration" );
        return;
    }

    TOOL_PROJECT_FIXTURE fixture;
    const std::filesystem::path sourceRoot =
            std::filesystem::path( __FILE__ ).parent_path().parent_path().parent_path()
            / "data/kichad/fabrication_clean";
    const std::pair<const char*, const char*> files[] = {
        { "fabrication_clean.kicad_pro", "preview.kicad_pro" },
        { "fabrication_clean.kicad_sch", "preview.kicad_sch" },
        { "fabrication_clean.kicad_pcb", "preview.kicad_pcb" }
    };

    for( const auto& [source, destination] : files )
    {
        std::error_code error;
        std::filesystem::copy_file(
                sourceRoot / source,
                std::filesystem::path( fixture.Root().ToStdString() ) / destination,
                std::filesystem::copy_options::overwrite_existing, error );
        BOOST_REQUIRE_MESSAGE( !error, error.message() );
    }

    wxFileName config = wxFileName::DirName( fixture.Root() );
    config.AppendDir( wxS( "config" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( config.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );
    SCOPED_ENVIRONMENT isolatedConfig( wxS( "KICAD_CONFIG_HOME" ),
                                       config.GetFullPath() );
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );

    for( const std::pair<const char*, const char*> request : {
                 std::pair{ "preview.kicad_sch", "schematic" },
                 std::pair{ "preview.kicad_pcb", "pcb2d" } } )
    {
        JSON rendered = registry.Handle(
                "inspect", { { "operation", "render" }, { "path", request.first },
                               { "view", request.second } } );
        BOOST_REQUIRE_MESSAGE( rendered.at( "success" ).get<bool>(), rendered.dump() );
        BOOST_REQUIRE_EQUAL( rendered["contentItems"].size(), 2 );
        BOOST_CHECK_EQUAL( rendered["contentItems"][1]["type"].get<std::string>(),
                           "inputImage" );
        const JSON data = envelope( rendered )["data"];
        BOOST_CHECK_GT( data["previewBytes"].get<int64_t>(), 8 );
        BOOST_CHECK_EQUAL( data["pages"][0]["contentItemIndex"].get<int>(), 1 );
    }
}


BOOST_AUTO_TEST_CASE( RendersExternalNativeSchematicHierarchyWhenRequested )
{
    wxString inputPath;

    if( !wxGetEnv( wxS( "KICHAD_QA_EXTERNAL_SCHEMATIC_PREVIEW" ), &inputPath )
        || inputPath.empty() )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in external hierarchy preview" );
        return;
    }

    wxFileName input( inputPath );
    BOOST_REQUIRE( input.FileExists() );
    TOOL_PROJECT_FIXTURE configFixture;
    wxFileName config = wxFileName::DirName( configFixture.Root() );
    config.AppendDir( wxS( "config" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( config.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );
    SCOPED_ENVIRONMENT isolatedConfig( wxS( "KICAD_CONFIG_HOME" ),
                                       config.GetFullPath() );
    CODEX_TOOL_REGISTRY registry( [input]() { return input.GetPath(); } );
    JSON rendered = registry.Handle(
            "inspect", { { "operation", "render" },
                           { "path", input.GetFullName().ToStdString() },
                           { "view", "schematic" } } );
    BOOST_REQUIRE_MESSAGE( rendered.at( "success" ).get<bool>(), rendered.dump() );
    const JSON data = envelope( rendered )["data"];
    BOOST_REQUIRE_EQUAL( data["pageCount"], data["renderedPages"] );
    BOOST_CHECK( data["hierarchyOverviewComposed"].get<bool>() );
    BOOST_REQUIRE_EQUAL( rendered["contentItems"].size(),
                         data["renderedPages"].get<size_t>() + 1 );
    BOOST_TEST_MESSAGE( data.dump( 2 ) );
}


BOOST_AUTO_TEST_CASE( ReturnsActionableFailureSteering )
{
    CODEX_TOOL_REGISTRY registry( []() { return wxString(); } );
    JSON result = registry.Handle( "unknown", { { "operation", "do_something" } } );

    BOOST_REQUIRE( !result.at( "success" ).get<bool>() );
    JSON error = envelope( result )["error"];
    BOOST_CHECK_EQUAL( error["contractVersion"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( error["code"].get<std::string>(), "unknown_tool" );
    BOOST_CHECK_EQUAL( error["stage"].get<std::string>(), "unknown.do_something" );
    BOOST_CHECK_EQUAL( error["stateChanged"].get<std::string>(), "none" );
    BOOST_CHECK( !error["retryable"].get<bool>() );
    BOOST_CHECK_EQUAL( error["context"]["tool"].get<std::string>(), "unknown" );
    BOOST_CHECK( !error["recovery"]["summary"].get<std::string>().empty() );
    BOOST_CHECK( !error["recovery"]["steps"].empty() );
}


BOOST_AUTO_TEST_CASE( RequestsInteractiveDependenciesAtPointOfUse )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );
    int requests = 0;
    JSON result = registry.HandleWithContext(
            "pcb", { { "operation", "status" }, { "path", "design.kicad_pcb" } },
            fixture.Root(), false, wxString(), false, std::chrono::milliseconds( 1 ),
            [&]( const CODEX_TOOL_REGISTRY::RUNTIME_DEPENDENCY& aDependency,
                 std::string& aError )
            {
                ++requests;
                BOOST_CHECK( aDependency.application
                             == CODEX_TOOL_REGISTRY::RUNTIME_APPLICATION::PCB_EDITOR );
                BOOST_CHECK_EQUAL( aDependency.document.GetFullName(),
                                   wxS( "design.kicad_pcb" ) );
                aError = "injected application broker rejection";
                return false;
            } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "dependency_unavailable" );
    BOOST_CHECK_EQUAL( requests, 1 );

    result = registry.HandleWithContext(
            "pcb", { { "operation", "describe" }, { "path", "design.kicad_pcb" },
                     { "itemType", "trace" } },
            fixture.Root(), false, wxString(), false, std::chrono::milliseconds( 1 ),
            [&]( const CODEX_TOOL_REGISTRY::RUNTIME_DEPENDENCY&, std::string& )
            {
                ++requests;
                return false;
            } );
    BOOST_CHECK( result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( requests, 1 );
}


BOOST_AUTO_TEST_CASE( ReturnsBoundedStructuredNativeErcResults )
{
    TOOL_PROJECT_FIXTURE fixture;
    int                  calls = 0;
    const std::string    nativeVersion( GetMajorMinorPatchVersion().ToUTF8() );
    CODEX_TOOL_REGISTRY  registry(
            [&fixture]() { return fixture.Root(); }, {}, {}, {},
            [&]( const std::string& aCheck, const wxFileName& aPath, std::string& aReport,
                 std::string& )
            {
                ++calls;
                BOOST_CHECK_EQUAL( aCheck, "erc" );
                BOOST_CHECK_EQUAL( aPath.GetFullName(), wxS( "design.kicad_sch" ) );
                const JSON item = { { "description", "R1 pin 1" },
                                    { "pos", { { "x", 10.0 }, { "y", 20.0 } } },
                                    { "uuid", "11111111-1111-4111-8111-111111111111" } };
                const JSON error = { { "type", "pin_not_connected" },
                                     { "description", "Pin is not connected" },
                                     { "severity", "error" },
                                     { "items", JSON::array( { item } ) } };
                const JSON warning = { { "type", "label_dangling" },
                                       { "description", "Label is not connected" },
                                       { "severity", "warning" },
                                       { "items", JSON::array( { item } ) } };
                const JSON excluded = { { "type", "pin_not_connected" },
                                        { "description", "Approved test point" },
                                        { "severity", "error" },
                                        { "items", JSON::array( { item } ) },
                                        { "excluded", true },
                                        { "comment", "TP waiver" } };
                aReport = JSON( { { "$schema", "https://schemas.kicad.org/erc.v1.json" },
                                  { "source", "design.kicad_sch" },
                                  { "date", "volatile and intentionally omitted" },
                                  { "kicad_version", nativeVersion },
                                  { "coordinate_units", "mm" },
                                  { "included_severities",
                                    JSON::array( { "error", "warning", "exclusion" } ) },
                                  { "ignored_checks",
                                    JSON::array( { { { "key", "pin_to_pin" },
                                                      { "description", "Pin conflict" } } } ) },
                                  { "sheets",
                                    JSON::array(
                                            { { { "path", "/" },
                                                { "uuid_path", "/root" },
                                                { "violations",
                                                  JSON::array( { error, warning, excluded } ) } } } ) } } )
                                  .dump();
                return true;
            } );

    JSON first = registry.Handle( "verify", { { "operation", "erc" },
                                                { "path", "design.kicad_sch" },
                                                { "offset", 1 },
                                                { "limit", 1 } } );
    BOOST_REQUIRE_MESSAGE( first.at( "success" ).get<bool>(), first.dump() );
    JSON firstData = envelope( first )["data"];
    BOOST_CHECK_EQUAL( firstData["kicadVersion"].get<std::string>(), nativeVersion );
    BOOST_CHECK( !firstData["clean"].get<bool>() );
    BOOST_CHECK( firstData["waiversPresent"].get<bool>() );
    BOOST_CHECK_EQUAL( firstData["counts"]["total"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( firstData["counts"]["errors"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["counts"]["warnings"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["counts"]["exclusions"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["counts"]["categories"]["erc"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( firstData["ignoredChecksCount"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["returnedViolations"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["violations"][0]["severity"].get<std::string>(), "warning" );
    BOOST_CHECK_EQUAL( firstData["violations"][0]["sheetPath"].get<std::string>(), "/" );
    BOOST_CHECK_EQUAL( firstData["nextOffset"].get<int>(), 2 );
    BOOST_CHECK( firstData["resultTruncated"].get<bool>() );
    BOOST_CHECK( !firstData.contains( "date" ) );

    JSON last = registry.Handle( "verify", { { "operation", "erc" },
                                               { "path", "design.kicad_sch" },
                                               { "offset", 2 },
                                               { "limit", 1 } } );
    BOOST_REQUIRE_MESSAGE( last.at( "success" ).get<bool>(), last.dump() );
    JSON lastData = envelope( last )["data"];
    BOOST_CHECK( lastData["violations"][0]["excluded"].get<bool>() );
    BOOST_CHECK( !lastData["resultTruncated"].get<bool>() );
    BOOST_CHECK( !lastData.contains( "nextOffset" ) );
    BOOST_CHECK_EQUAL( calls, 2 );
}


BOOST_AUTO_TEST_CASE( RejectsNativeVerificationVersionMismatch )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry(
            [&fixture]() { return fixture.Root(); }, {}, {}, {},
            []( const std::string&, const wxFileName&, std::string& aReport, std::string& )
            {
                aReport = JSON( { { "$schema", "https://schemas.kicad.org/drc.v1.json" },
                                  { "source", "design.kicad_pcb" },
                                  { "date", "2026-01-01T00:00:00Z" },
                                  { "kicad_version", "9.0.0" },
                                  { "coordinate_units", "mm" },
                                  { "included_severities",
                                    JSON::array( { "error", "warning", "exclusion" } ) },
                                  { "ignored_checks", JSON::array() },
                                  { "violations", JSON::array() },
                                  { "unconnected_items", JSON::array() },
                                  { "schematic_parity", JSON::array() } } )
                                  .dump();
                return true;
            } );
    JSON result = registry.Handle(
            "verify", { { "operation", "drc" }, { "path", "design.kicad_pcb" } } );
    BOOST_REQUIRE( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "version_mismatch" );
}


BOOST_AUTO_TEST_CASE( VerifiesSingleRepresentationSourcingEvidence )
{
    TOOL_PROJECT_FIXTURE fixture;
    const std::string today( wxDateTime::Today().FormatISODate().ToUTF8() );
    const auto record = [&]( const std::string& aReference, const std::string& aLifecycle,
                             int aAvailable, const std::string& aVerifiedOn )
    {
        return "  (source " + aReference + "\n"
               "    (manufacturer \"Example Components\")\n"
               "    (mpn \"EX-0603-10K\")\n"
               "    (datasheet \"https://manufacturer.example.test/EX-0603-10K.pdf\")\n"
               "    (lifecycle " + aLifecycle + ")\n"
               "    (supplier \"DigiKey\")\n"
               "    (sku \"EX-0603-10K-ND\")\n"
               "    (product_url \"https://distributor.example.test/EX-0603-10K\")\n"
               "    (available " + std::to_string( aAvailable ) + ")\n"
               "    (verified_on " + aVerifiedOn + ")\n"
               "    (quantity 1))\n";
    };
    const auto writeSidecar = [&]( const wxString& aName, const std::string& aSource )
    {
        wxFFile file( wxFileName( fixture.Root(), aName ).GetFullPath(), wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE_EQUAL( file.Write( aSource.data(), aSource.size() ), aSource.size() );
    };
    const std::string prefix =
            "(kichad_design\n"
            "  (version 1)\n"
            "  (project sourcing)\n"
            "  (component R1 (symbol \"Device:R\") (value \"10k\") "
            "(footprint \"Resistor:R_0603\"))\n"
            "  (component PWR1 (symbol \"power:GND\") (value \"GND\") "
            "(footprint none))\n";
    const std::string suffix = "  (check sourcing)\n)\n";
    writeSidecar( wxS( "sourcing.kicad_kds" ), prefix + record( "R1", "active", 500, today )
                                                     + suffix );
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );
    JSON clean = registry.Handle( "verify", { { "operation", "sourcing" },
                                                { "path", "sourcing.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( clean.at( "success" ).get<bool>(), clean.dump() );
    JSON cleanData = envelope( clean )["data"];
    BOOST_CHECK( cleanData["clean"].get<bool>() );
    BOOST_CHECK_EQUAL( cleanData["counts"]["total"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( cleanData["sourcing"]["requiredComponents"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( cleanData["sourcing"]["sourceRecords"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( cleanData["sourcing"]["completeComponents"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( cleanData["maxAgeDays"].get<int>(), 7 );
    BOOST_CHECK_EQUAL( cleanData["verifiedOn"].get<std::string>(), today );

    const std::string missing =
            prefix
            + "  (component R2 (symbol \"Device:R\") (value \"1k\") "
              "(footprint \"Resistor:R_0603\"))\n"
            + record( "R1", "active", 500, today ) + suffix;
    writeSidecar( wxS( "missing-source.kicad_kds" ), missing );
    JSON incomplete = registry.Handle(
            "verify", { { "operation", "sourcing" },
                         { "path", "missing-source.kicad_kds" },
                         { "limit", 1 } } );
    BOOST_REQUIRE_MESSAGE( incomplete.at( "success" ).get<bool>(), incomplete.dump() );
    JSON incompleteData = envelope( incomplete )["data"];
    BOOST_CHECK( !incompleteData["clean"].get<bool>() );
    BOOST_CHECK_EQUAL( incompleteData["counts"]["errors"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( incompleteData["violations"][0]["type"].get<std::string>(),
                       "missing_source" );
    BOOST_CHECK_EQUAL( incompleteData["violations"][0]["component"].get<std::string>(),
                       "R2" );

    writeSidecar( wxS( "stale-source.kicad_kds" ),
                  prefix + record( "R1", "nrnd", 0, "2000-01-01" ) + suffix );
    JSON stale = registry.Handle( "verify", { { "operation", "sourcing" },
                                                { "path", "stale-source.kicad_kds" },
                                                { "maxAgeDays", 30 } } );
    BOOST_REQUIRE_MESSAGE( stale.at( "success" ).get<bool>(), stale.dump() );
    JSON staleData = envelope( stale )["data"];
    BOOST_CHECK( !staleData["clean"].get<bool>() );
    BOOST_CHECK_EQUAL( staleData["counts"]["total"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( staleData["counts"]["errors"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( staleData["counts"]["warnings"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( staleData["sourcing"]["completeComponents"].get<int>(), 0 );

    const std::string tomorrow(
            ( wxDateTime::Today() + wxDateSpan::Day() ).FormatISODate().ToUTF8() );
    writeSidecar( wxS( "future-source.kicad_kds" ),
                  prefix + record( "R1", "active", 500, tomorrow ) + suffix );
    JSON future = registry.Handle( "verify", { { "operation", "sourcing" },
                                                 { "path", "future-source.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( future.at( "success" ).get<bool>(), future.dump() );
    JSON futureData = envelope( future )["data"];
    BOOST_CHECK_EQUAL( futureData["counts"]["errors"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( futureData["violations"][0]["type"].get<std::string>(),
                       "future_evidence" );
}


BOOST_AUTO_TEST_CASE( VerifiesCanonicalPhysicalLayoutContract )
{
    TOOL_PROJECT_FIXTURE fixture;
    const auto writeSidecar = [&]( const char* aMaximumWidth )
    {
        const std::string source =
                "(kichad_design\n"
                "  (version 1)\n"
                "  (project layout_verify)\n"
                "  (component U1 (symbol \"Device:R\") (value \"1k\") "
                "(footprint \"Resistor:R_0603\"))\n"
                "  (board\n"
                "    (outline (rectangle edge (start 0mm 0mm) (end 10mm 8mm) "
                "(stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))\n"
                "    (place U1 (at 5mm 4mm))\n"
                "    (layout (board (maximum_width "
                + std::string( aMaximumWidth )
                + ") (maximum_height 8mm)) "
                  "(routing (defaults (geometry any) (maximum_vias 0)))))\n"
                  "  (check layout))\n";
        wxFFile file( wxFileName( fixture.Root(), wxS( "layout.kicad_kds" ) )
                              .GetFullPath(),
                      wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE_EQUAL( file.Write( source.data(), source.size() ), source.size() );
    };
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );
    writeSidecar( "10mm" );
    JSON clean = registry.Handle(
            "verify", { { "operation", "layout" }, { "path", "layout.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( clean.at( "success" ).get<bool>(), clean.dump() );
    JSON cleanData = envelope( clean )["data"];
    BOOST_CHECK( cleanData["clean"].get<bool>() );
    BOOST_CHECK_EQUAL( cleanData["counts"]["total"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( cleanData["layout"]["board"]["widthNm"].get<int64_t>(),
                       10000000 );

    writeSidecar( "9mm" );
    JSON failed = registry.Handle(
            "verify", { { "operation", "layout" }, { "path", "layout.kicad_kds" },
                         { "limit", 1 } } );
    BOOST_REQUIRE_MESSAGE( failed.at( "success" ).get<bool>(), failed.dump() );
    JSON failedData = envelope( failed )["data"];
    BOOST_CHECK( !failedData["clean"].get<bool>() );
    BOOST_CHECK_EQUAL( failedData["counts"]["errors"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( failedData["violations"][0]["type"].get<std::string>(),
                       "board_width_exceeded" );
}


BOOST_AUTO_TEST_CASE( DescribesExactPcbProtobufJsonFieldsWithoutAnEditor )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );

    JSON root = registry.Handle( "pcb", { { "operation", "describe" },
                                           { "path", "design.kicad_pcb" },
                                           { "itemType", "footprint" } } );
    BOOST_REQUIRE_MESSAGE( root.at( "success" ).get<bool>(), root.dump() );
    JSON rootSchema = envelope( root )["data"]["schema"];
    BOOST_CHECK_EQUAL( rootSchema["messageType"].get<std::string>(),
                       "kiapi.board.types.FootprintInstance" );

    JSON nested = registry.Handle( "pcb", { { "operation", "describe" },
                                             { "path", "design.kicad_pcb" },
                                             { "itemType", "trace" },
                                             { "messagePath", "start" } } );
    BOOST_REQUIRE_MESSAGE( nested.at( "success" ).get<bool>(), nested.dump() );
    JSON nestedSchema = envelope( nested )["data"]["schema"];
    BOOST_CHECK_EQUAL( nestedSchema["messageType"].get<std::string>(),
                       "kiapi.common.types.Vector2" );
    BOOST_CHECK_EQUAL( nestedSchema["fields"][0]["name"].get<std::string>(), "xNm" );
    BOOST_CHECK_EQUAL( nestedSchema["fields"][0]["jsonEncoding"].get<std::string>(),
                       "decimal string" );
}


BOOST_AUTO_TEST_CASE( ReportsProjectAndSnapshotState )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; } );

    JSON result = registry.Handle( "project", { { "operation", "context" } } );
    BOOST_REQUIRE( result.at( "success" ).get<bool>() );

    JSON data = envelope( result ).at( "data" );
    BOOST_CHECK_EQUAL( data.at( "projectPath" ).get<std::string>(),
                       std::string( fixture.Root().ToUTF8() ) );
    BOOST_CHECK( data.at( "mutationAvailable" ).get<bool>() );
    BOOST_CHECK_GE( data.at( "files" ).size(), 2 );
}


BOOST_AUTO_TEST_CASE( CompilesAndAtomicallySavesReusableDesignSidecars )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project reusable)
  (sheet root (parent none) (file "reusable.kicad_sch") (title "Reusable"))
  (component R1
    (symbol "Device:R")
    (value "10k")
    (footprint "Resistor_SMD:R_0603_1608Metric")
    (unit 1 (sheet root) (at 30mm 30mm) (rotation 0deg) (mirror none)))
  (component LED1
    (symbol "Device:LED")
    (value "GREEN")
    (footprint "LED_SMD:LED_0603_1608Metric")
    (unit 1 (sheet root) (at 50mm 30mm) (rotation 0deg) (mirror none)))
  (net LED_A (pin R1 1 1) (pin LED1 1 1))
  (board
    (outline
      (rectangle board-edge (start 0mm 0mm) (end 40mm 30mm)
        (radius 0mm) (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))
    (route LED_A (id led-a-trace) (from 10mm 10mm) (to 20mm 10mm)
      (width 0.25mm) (layer F.Cu)))
  (check erc)
  (check drc)
  (output gerbers))
)KDS";

    JSON compiled = registry.Handle( "design", { { "operation", "compile" },
                                                  { "source", source } } );
    BOOST_REQUIRE_MESSAGE( compiled.at( "success" ).get<bool>(), compiled.dump() );
    JSON compileData = envelope( compiled )["data"];
    BOOST_CHECK( compileData["valid"].get<bool>() );
    BOOST_CHECK( !compileData.contains( "ir" ) );
    const std::string firstHash = compileData["sourceSha256"].get<std::string>();

    int nativeValidationCalls = 0;
    CODEX_TOOL_REGISTRY readOnlyRegistry(
            [&fixture]() { return fixture.Root(); }, {}, {},
            [&]( const wxFileName& aRoot, const JSON& aCompilerIr,
                 const JSON& aResolvedSymbols, std::string& )
            {
                ++nativeValidationCalls;
                BOOST_CHECK( aRoot.FileExists() );
                BOOST_CHECK_NE( aRoot.GetPath(), fixture.Root() );
                BOOST_CHECK( aCompilerIr.contains( "schematic" ) );
                BOOST_CHECK( aResolvedSymbols.is_object() );
                return true;
            } );
    JSON firstPreview = readOnlyRegistry.Handle(
            "design", { { "operation", "preview" }, { "source", source } } );
    JSON secondPreview = readOnlyRegistry.Handle(
            "design", { { "operation", "preview" }, { "source", source } } );
    BOOST_REQUIRE_MESSAGE( firstPreview.at( "success" ).get<bool>(), firstPreview.dump() );
    BOOST_REQUIRE_MESSAGE( secondPreview.at( "success" ).get<bool>(), secondPreview.dump() );
    const std::string hierarchySource = R"KDS((kichad_design
  (version 1)
  (project validated_preview)
  (sheet root (parent none) (file "validated_preview.kicad_sch") (title "Main"))
  (sheet child (parent root) (file "child.kicad_sch") (title "Child")
    (at 20mm 20mm) (size 40mm 20mm))
))KDS";
    JSON validatedPreview = readOnlyRegistry.Handle(
            "design", { { "operation", "preview" }, { "source", hierarchySource } } );
    BOOST_REQUIRE_MESSAGE( validatedPreview.at( "success" ).get<bool>(),
                           validatedPreview.dump() );
    BOOST_CHECK_EQUAL( nativeValidationCalls, 1 );
    JSON firstBoardPlan = envelope( firstPreview )["data"]["boardPlan"];
    JSON secondBoardPlan = envelope( secondPreview )["data"]["boardPlan"];
    BOOST_CHECK( firstBoardPlan["fullyLowered"].get<bool>() );
    BOOST_CHECK( !firstBoardPlan["itemsTruncated"].get<bool>() );
    BOOST_REQUIRE_EQUAL( firstBoardPlan["items"].size(), 2 );
    BOOST_CHECK_EQUAL( firstBoardPlan["items"][0]["targetId"].get<std::string>(),
                       secondBoardPlan["items"][0]["targetId"].get<std::string>() );
    BOOST_CHECK_EQUAL( firstBoardPlan["items"][1]["targetId"].get<std::string>(),
                       secondBoardPlan["items"][1]["targetId"].get<std::string>() );

    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                               { "path", "reusable.kicad_kds" },
                                               { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    BOOST_CHECK_EQUAL( envelope( saved )["data"]["sourceSha256"].get<std::string>(),
                       firstHash );

    JSON read = registry.Handle( "design", { { "operation", "read" },
                                               { "path", "reusable.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( read.at( "success" ).get<bool>(), read.dump() );
    JSON readData = envelope( read )["data"];
    BOOST_CHECK_EQUAL( readData["source"].get<std::string>(), source );
    BOOST_CHECK_EQUAL( readData["bytes"].get<size_t>(), source.size() );
    BOOST_CHECK_EQUAL( readData["sourceSha256"].get<std::string>(), firstHash );
    BOOST_CHECK( readData["valid"].get<bool>() );
    BOOST_CHECK( !readData.contains( "ir" ) );
    BOOST_CHECK( !readData.contains( "plan" ) );

    JSON context = registry.Handle( "design", { { "operation", "context" },
                                                   { "path", "reusable.kicad_kds" },
                                                   { "domain", "schematic" },
                                                   { "limit", 1 } } );
    BOOST_REQUIRE_MESSAGE( context.at( "success" ).get<bool>(), context.dump() );
    JSON contextData = envelope( context )["data"];
    BOOST_CHECK( contextData["valid"].get<bool>() );
    BOOST_CHECK_EQUAL( contextData["sourceSha256"].get<std::string>(), firstHash );
    BOOST_CHECK_EQUAL( contextData["context"]["schema"].get<std::string>(),
                       "kichad.design-context.v1" );
    BOOST_CHECK_EQUAL( contextData["context"]["items"].size(), 1 );
    BOOST_CHECK( contextData["context"]["nextOffset"].is_number_unsigned() );

    JSON focusedContext = registry.Handle( "design", { { "operation", "context" },
                                                          { "path", "reusable.kicad_kds" },
                                                          { "domain", "schematic" },
                                                          { "query", "LED1" },
                                                          { "limit", 10 } } );
    BOOST_REQUIRE_MESSAGE( focusedContext.at( "success" ).get<bool>(),
                           focusedContext.dump() );
    BOOST_CHECK_GT( envelope( focusedContext )["data"]["context"]["total"].get<size_t>(),
                    0 );

    JSON loaded = registry.Handle( "design", { { "operation", "compile" },
                                                { "path", "reusable.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( loaded.at( "success" ).get<bool>(), loaded.dump() );
    BOOST_CHECK( envelope( loaded )["data"]["valid"].get<bool>() );
    BOOST_CHECK_EQUAL( envelope( loaded )["data"]["sourceSha256"].get<std::string>(),
                       firstHash );
    BOOST_CHECK( !envelope( loaded )["data"].contains( "ir" ) );

    JSON staleApply = registry.Handle(
            "design", { { "operation", "apply" }, { "path", "reusable.kicad_kds" },
                        { "boardPath", "design.kicad_pcb" },
                        { "expectedSha256", std::string( 64, '0' ) } } );
    BOOST_REQUIRE( !staleApply.at( "success" ).get<bool>() );
    JSON staleApplyError = envelope( staleApply )["error"];
    BOOST_CHECK_EQUAL( staleApplyError["code"].get<std::string>(), "stale_source" );
    BOOST_CHECK_EQUAL( staleApplyError["stage"].get<std::string>(), "design.apply" );
    BOOST_CHECK_EQUAL( staleApplyError["details"]["actualSha256"].get<std::string>(), firstHash );
    BOOST_REQUIRE_EQUAL( staleApplyError["recovery"]["steps"].size(), 2 );
    BOOST_CHECK_EQUAL( staleApplyError["recovery"]["steps"][0]["tool"].get<std::string>(),
                       "design" );
    BOOST_CHECK_EQUAL(
            staleApplyError["recovery"]["steps"][1]["arguments"]["expectedSha256"]
                    .get<std::string>(),
            "$previous.data.sourceSha256" );

    const std::string updated = source + "; reusable sidecar comment\n";
    JSON noHash = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "reusable.kicad_kds" },
                                                { "source", updated } } );
    BOOST_CHECK( !noHash.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( noHash )["error"]["code"].get<std::string>(), "stale_source" );
    BOOST_CHECK_EQUAL( envelope( noHash )["error"]["stage"].get<std::string>(),
                       "design.save" );
    BOOST_CHECK( envelope( noHash )["error"]["retryable"].get<bool>() );
    BOOST_CHECK( !envelope( noHash )["error"]["recovery"]["steps"].empty() );

    JSON stale = registry.Handle( "design", { { "operation", "save" },
                                               { "path", "reusable.kicad_kds" },
                                               { "source", updated },
                                               { "expectedSha256", std::string( 64, '0' ) } } );
    BOOST_CHECK( !stale.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( stale )["error"]["code"].get<std::string>(), "stale_source" );
    BOOST_CHECK_EQUAL(
            envelope( stale )["error"]["details"]["expectedSha256"].get<std::string>(),
            std::string( 64, '0' ) );
    BOOST_CHECK_EQUAL( envelope( stale )["error"]["details"]["actualSha256"].get<std::string>(),
                       firstHash );

    JSON replaced = registry.Handle( "design", { { "operation", "save" },
                                                  { "path", "reusable.kicad_kds" },
                                                  { "source", updated },
                                                  { "expectedSha256", firstHash } } );
    BOOST_REQUIRE_MESSAGE( replaced.at( "success" ).get<bool>(), replaced.dump() );

    wxFile installed( wxFileName( fixture.Root(), wxS( "reusable.kicad_kds" ) ).GetFullPath(),
                      wxFile::read );
    BOOST_REQUIRE( installed.IsOpened() );
    std::string installedSource( static_cast<size_t>( installed.Length() ), '\0' );
    BOOST_REQUIRE_EQUAL( installed.Read( installedSource.data(), installedSource.size() ),
                         installedSource.size() );
    BOOST_CHECK_EQUAL( installedSource, updated );

    JSON reread = registry.Handle( "design", { { "operation", "read" },
                                                 { "path", "reusable.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( reread.at( "success" ).get<bool>(), reread.dump() );
    BOOST_CHECK_EQUAL( envelope( reread )["data"]["source"].get<std::string>(), updated );

    JSON escaped = registry.Handle( "design", { { "operation", "save" },
                                                 { "path", "../escape.kicad_kds" },
                                                 { "source", source } } );
    BOOST_CHECK( !escaped.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( escaped )["error"]["code"].get<std::string>(), "invalid_path" );
}


BOOST_AUTO_TEST_CASE( SearchesReadsAndAtomicallyPatchesDesignSidecars )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project patchable)
  (component R1
    (symbol "Device:R")
    (value "10k")
    (footprint "Resistor_SMD:R_0603_1608Metric"))
  (component R2
    (symbol "Device:R")
    (value "10k")
    (footprint "Resistor_SMD:R_0603_1608Metric"))
  (net LINK (pin R1 1 1) (pin R2 1 1))
  (check erc))
)KDS";

    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                               { "path", "patchable.kicad_kds" },
                                               { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    const std::string firstHash =
            envelope( saved )["data"]["sourceSha256"].get<std::string>();

    JSON searched = registry.Handle( "design", { { "operation", "search" },
                                                  { "path", "patchable.kicad_kds" },
                                                  { "query", "VALUE \"10K\"" },
                                                  { "contextBytes", 48 } } );
    BOOST_REQUIRE_MESSAGE( searched.at( "success" ).get<bool>(), searched.dump() );
    JSON searchData = envelope( searched )["data"];
    BOOST_CHECK_EQUAL( searchData["sourceSha256"].get<std::string>(), firstHash );
    BOOST_CHECK_EQUAL( searchData["totalMatches"].get<size_t>(), 2 );
    BOOST_REQUIRE_EQUAL( searchData["matches"].size(), 2 );
    BOOST_CHECK_EQUAL( searchData["matches"][0]["matchText"].get<std::string>(),
                       "value \"10k\"" );
    BOOST_CHECK_NE( searchData["matches"][0]["source"].get<std::string>().find( "R1" ),
                    std::string::npos );

    JSON boundedRead = registry.Handle( "design", { { "operation", "read" },
                                                     { "path", "patchable.kicad_kds" },
                                                     { "startLine", 4 },
                                                     { "lineCount", 4 } } );
    BOOST_REQUIRE_MESSAGE( boundedRead.at( "success" ).get<bool>(), boundedRead.dump() );
    JSON boundedData = envelope( boundedRead )["data"];
    BOOST_CHECK_EQUAL( boundedData["range"]["startLine"].get<size_t>(), 4 );
    BOOST_CHECK_EQUAL( boundedData["range"]["endLine"].get<size_t>(), 7 );
    BOOST_CHECK( !boundedData["range"]["complete"].get<bool>() );
    BOOST_CHECK_NE( boundedData["source"].get<std::string>().find( "component R1" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( boundedData["source"].get<std::string>().find( "component R2" ),
                       std::string::npos );
    BOOST_CHECK_LT( boundedData["bytes"].get<size_t>(),
                    boundedData["sourceBytes"].get<size_t>() );

    JSON patched = registry.Handle(
            "design", { { "operation", "patch" }, { "path", "patchable.kicad_kds" },
                        { "expectedSha256", firstHash },
                        { "edits",
                          JSON::array(
                                  { { { "oldText", "(project patchable)" },
                                      { "newText", "(project precisely-patched)" } },
                                    { { "oldText",
                                        "(component R1\n    (symbol \"Device:R\")\n    (value \"10k\")" },
                                      { "newText",
                                        "(component R1\n    (symbol \"Device:R\")\n    (value \"22k\")" } } } ) } } );
    BOOST_REQUIRE_MESSAGE( patched.at( "success" ).get<bool>(), patched.dump() );
    JSON patchData = envelope( patched )["data"];
    const std::string patchedHash = patchData["sourceSha256"].get<std::string>();
    BOOST_CHECK_EQUAL( patchData["operation"].get<std::string>(), "patch" );
    BOOST_CHECK_EQUAL( patchData["previousSha256"].get<std::string>(), firstHash );
    BOOST_CHECK_EQUAL( patchData["editsApplied"].get<size_t>(), 2 );
    BOOST_CHECK_NE( patchedHash, firstHash );

    JSON read = registry.Handle( "design", { { "operation", "read" },
                                              { "path", "patchable.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( read.at( "success" ).get<bool>(), read.dump() );
    const std::string patchedSource = envelope( read )["data"]["source"].get<std::string>();
    BOOST_CHECK_NE( patchedSource.find( "(project precisely-patched)" ), std::string::npos );
    BOOST_CHECK_NE( patchedSource.find( "(value \"22k\")" ), std::string::npos );

    JSON stale = registry.Handle(
            "design", { { "operation", "patch" }, { "path", "patchable.kicad_kds" },
                        { "expectedSha256", firstHash },
                        { "edits", JSON::array( { { { "oldText", "22k" },
                                                    { "newText", "33k" } } } ) } } );
    BOOST_REQUIRE( !stale.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( stale )["error"]["code"].get<std::string>(),
                       "stale_source" );

    JSON missing = registry.Handle(
            "design", { { "operation", "patch" }, { "path", "patchable.kicad_kds" },
                        { "expectedSha256", patchedHash },
                        { "edits", JSON::array( { { { "oldText", "not in this KDS" },
                                                    { "newText", "replacement" } } } ) } } );
    BOOST_REQUIRE( !missing.at( "success" ).get<bool>() );
    JSON missingError = envelope( missing )["error"];
    BOOST_CHECK_EQUAL( missingError["code"].get<std::string>(),
                       "patch_context_not_found" );
    BOOST_CHECK_EQUAL( missingError["stateChanged"].get<std::string>(), "none" );
    BOOST_CHECK( missingError["retryable"].get<bool>() );

    JSON ambiguous = registry.Handle(
            "design", { { "operation", "patch" }, { "path", "patchable.kicad_kds" },
                        { "expectedSha256", patchedHash },
                        { "edits", JSON::array( { { { "oldText", "(symbol \"Device:R\")" },
                                                    { "newText", "(symbol \"Device:R_US\")" } } } ) } } );
    BOOST_REQUIRE( !ambiguous.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( ambiguous )["error"]["code"].get<std::string>(),
                       "patch_context_ambiguous" );
    BOOST_CHECK_EQUAL(
            envelope( ambiguous )["error"]["details"]["matchesAtLeast"].get<size_t>(),
                       2 );

    JSON invalid = registry.Handle(
            "design", { { "operation", "patch" }, { "path", "patchable.kicad_kds" },
                        { "expectedSha256", patchedHash },
                        { "edits", JSON::array( { { { "oldText", "(check erc)" },
                                                    { "newText", "(check unsupported)" } } } ) } } );
    BOOST_REQUIRE( !invalid.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( invalid )["error"]["code"].get<std::string>(),
                       "compile_failed" );

    JSON unchanged = registry.Handle( "design", { { "operation", "read" },
                                                   { "path", "patchable.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( unchanged.at( "success" ).get<bool>(), unchanged.dump() );
    BOOST_CHECK_EQUAL( envelope( unchanged )["data"]["sourceSha256"].get<std::string>(),
                       patchedHash );
    BOOST_CHECK_EQUAL( envelope( unchanged )["data"]["source"].get<std::string>(),
                       patchedSource );
}


BOOST_AUTO_TEST_CASE( SummarizesAndFindsBoundedExpressions )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );

    JSON summary = registry.Handle(
            "inspect", { { "operation", "summary" }, { "path", "design.kicad_pcb" } } );
    BOOST_REQUIRE_MESSAGE( summary.at( "success" ).get<bool>(), summary.dump() );
    JSON summaryData = envelope( summary ).at( "data" );
    BOOST_CHECK_EQUAL( summaryData.at( "rootHead" ).get<std::string>(), "kicad_pcb" );
    BOOST_CHECK_GT( summaryData.at( "nodes" ).get<size_t>(), 0 );

    JSON found = registry.Handle( "inspect", { { "operation", "find" },
                                                { "path", "design.kicad_pcb" },
                                                { "head", "footprint" }, { "limit", 1 } } );
    BOOST_REQUIRE_MESSAGE( found.at( "success" ).get<bool>(), found.dump() );
    JSON foundData = envelope( found ).at( "data" );
    BOOST_CHECK_EQUAL( foundData.at( "totalMatches" ).get<size_t>(), 1 );
    BOOST_REQUIRE_EQUAL( foundData.at( "expressions" ).size(), 1 );
    BOOST_CHECK_NE( foundData["expressions"][0]["text"].get<std::string>().find( "keep-me" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( ConfinesProjectFootprintSourcesUsedByPlacement )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );
    wxFileName library = wxFileName::DirName( fixture.Root() );
    library.AppendDir( wxS( "Local.pretty" ) );
    BOOST_REQUIRE( wxFileName::Mkdir(
            library.GetFullPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project footprint_inventory)
  (library symbol Device (table global))
  (library footprint Local (table project)
    (uri "${KIPRJMOD}/Local.pretty"))
  (sheet root (parent none) (file "footprint_inventory.kicad_sch") (title "Root"))
  (component R1 (symbol "Device:R") (value "1k") (footprint "Local:R")
    (unit 1 (sheet root) (at 10mm 10mm) (rotation 0deg) (mirror none)))
  (board (place R1 (at 20mm 20mm) (rotation 0deg) (side front) (locked false))))
)KDS";

    JSON missing = registry.Handle(
            "design", { { "operation", "preview" }, { "source", source } } );
    BOOST_REQUIRE_MESSAGE( !missing.at( "success" ).get<bool>(), missing.dump() );
    BOOST_CHECK_EQUAL( envelope( missing )["error"]["code"].get<std::string>(),
                       "footprint_inventory_failed" );

    wxFileName footprint( library.GetFullPath(), wxS( "R.kicad_mod" ) );
    wxFFile file( footprint.GetFullPath(), wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE( file.Write( wxS( "(footprint \"R\" (version 20260206)\n)\n" ) ) );
    BOOST_REQUIRE( file.Close() );

    JSON inventoried = registry.Handle(
            "design", { { "operation", "preview" }, { "source", source } } );
    BOOST_REQUIRE_MESSAGE( inventoried.at( "success" ).get<bool>(), inventoried.dump() );

#ifndef __WXMSW__
    BOOST_REQUIRE( wxRemoveFile( footprint.GetFullPath() ) );
    std::error_code linkError;
    std::filesystem::create_symlink(
            std::filesystem::path( fixture.OutsidePath().ToStdString() ),
            std::filesystem::path( footprint.GetFullPath().ToStdString() ), linkError );
    BOOST_REQUIRE_MESSAGE( !linkError, linkError.message() );
    JSON linked = registry.Handle(
            "design", { { "operation", "preview" }, { "source", source } } );
    BOOST_REQUIRE_MESSAGE( !linked.at( "success" ).get<bool>(), linked.dump() );
    BOOST_CHECK_EQUAL( envelope( linked )["error"]["code"].get<std::string>(),
                       "footprint_inventory_failed" );
#endif
}


BOOST_AUTO_TEST_CASE( RejectsPathsOutsideTheProject )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );

    JSON escaped = registry.Handle(
            "inspect", { { "operation", "summary" },
                         { "path", std::string( fixture.OutsideRelativePath().ToUTF8() ) } } );
    BOOST_REQUIRE_MESSAGE( !escaped.at( "success" ).get<bool>(), escaped.dump() );
    BOOST_CHECK_EQUAL( envelope( escaped )["error"]["code"].get<std::string>(), "invalid_path" );

    JSON absolute = registry.Handle(
            "inspect", { { "operation", "summary" },
                         { "path", std::string( wxFileName( fixture.Root(),
                                                             wxS( "design.kicad_pcb" ) )
                                                        .GetFullPath()
                                                        .ToUTF8() ) } } );
    BOOST_CHECK( !absolute.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( absolute )["error"]["code"].get<std::string>(), "invalid_path" );

#ifndef __WXMSW__
    wxFileName link( fixture.Root(), wxS( "linked.kicad_pcb" ) );
    std::error_code linkError;
    std::filesystem::create_symlink( std::filesystem::path( fixture.OutsidePath().ToStdString() ),
                                     std::filesystem::path( link.GetFullPath().ToStdString() ),
                                     linkError );
    BOOST_REQUIRE_MESSAGE( !linkError, linkError.message() );

    JSON linked = registry.Handle(
            "inspect", { { "operation", "summary" }, { "path", "linked.kicad_pcb" } } );
    BOOST_REQUIRE_MESSAGE( !linked.at( "success" ).get<bool>(), linked.dump() );
    BOOST_CHECK_EQUAL( envelope( linked )["error"]["code"].get<std::string>(), "invalid_path" );

    CODEX_TOOL_REGISTRY writableRegistry( [&fixture]() { return fixture.Root(); },
                                          []() { return true; } );
    const std::string librarySource = R"KDS((kichad_design
  (version 1)
  (project protected_tables)
  (library symbol LocalSymbols (table project)
    (uri "${KIPRJMOD}/libraries/local.kicad_sym"))
))KDS";
    JSON saved = writableRegistry.Handle( "design", { { "operation", "save" },
                                                        { "path", "tables.kicad_kds" },
                                                        { "source", librarySource } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    wxFileName tableLink( fixture.Root(), wxS( "sym-lib-table" ) );
    linkError.clear();
    std::filesystem::create_symlink(
            std::filesystem::path( fixture.OutsidePath().ToStdString() ),
            std::filesystem::path( tableLink.GetFullPath().ToStdString() ), linkError );
    BOOST_REQUIRE_MESSAGE( !linkError, linkError.message() );
    JSON linkedTable = writableRegistry.Handle(
            "design", { { "operation", "apply" },
                         { "path", "tables.kicad_kds" },
                         { "boardPath", "design.kicad_pcb" },
                         { "expectedSha256",
                           envelope( saved )["data"]["sourceSha256"] } } );
    BOOST_REQUIRE_MESSAGE( !linkedTable.at( "success" ).get<bool>(), linkedTable.dump() );
    BOOST_CHECK_EQUAL( envelope( linkedTable )["error"]["code"].get<std::string>(),
                       "library_table_inventory_failed" );
    std::filesystem::remove(
            std::filesystem::path( tableLink.GetFullPath().ToStdString() ) );
    std::filesystem::remove( std::filesystem::path( link.GetFullPath().ToStdString() ) );
#endif
}


BOOST_AUTO_TEST_CASE( RejectsMalformedArgumentsAndDocuments )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );

    JSON result;
    BOOST_REQUIRE_NO_THROW( result = registry.Handle(
                                    "inspect", { { "operation", 7 },
                                                 { "path", "design.kicad_pcb" } } ) );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );

    BOOST_REQUIRE_NO_THROW( result = registry.Handle(
                                    "inspect", { { "operation", "find" },
                                                 { "path", "design.kicad_pcb" },
                                                 { "head", JSON::array() } } ) );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );

    result = registry.Handle(
            "inspect", { { "operation", "summary" }, { "path", "empty.kicad_pcb" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(), "parse_failed" );

    result = registry.Handle(
            "inspect", { { "operation", "summary" }, { "path", "wrong-root.kicad_pcb" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "format_mismatch" );

    result = registry.Handle( "design", { { "operation", "read" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "invalid_arguments" );

    const std::string malformedSource = "(kichad_design";
    wxFFile malformedFile( wxFileName( fixture.Root(), wxS( "malformed.kicad_kds" ) )
                                   .GetFullPath(),
                           wxS( "wb" ) );
    BOOST_REQUIRE( malformedFile.IsOpened() );
    BOOST_REQUIRE_EQUAL( malformedFile.Write( malformedSource.data(), malformedSource.size() ),
                         malformedSource.size() );
    malformedFile.Close();

    result = registry.Handle( "design", { { "operation", "read" },
                                            { "path", "malformed.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( result.at( "success" ).get<bool>(), result.dump() );
    BOOST_CHECK_EQUAL( envelope( result )["data"]["source"].get<std::string>(),
                       malformedSource );
    BOOST_CHECK( !envelope( result )["data"]["valid"].get<bool>() );

    const std::string invalidUtf8 = "(kichad_design \xFF)";
    wxFFile invalidFile( wxFileName( fixture.Root(), wxS( "invalid-utf8.kicad_kds" ) )
                                 .GetFullPath(),
                         wxS( "wb" ) );
    BOOST_REQUIRE( invalidFile.IsOpened() );
    BOOST_REQUIRE_EQUAL( invalidFile.Write( invalidUtf8.data(), invalidUtf8.size() ),
                         invalidUtf8.size() );
    invalidFile.Close();

    result = registry.Handle( "design", { { "operation", "read" },
                                            { "path", "invalid-utf8.kicad_kds" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "invalid_source" );

    result = registry.Handle( "pcb", { { "operation", "status" },
                                        { "path", "design.kicad_pro" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(), "invalid_path" );

    result = registry.Handle( "pcb", { { "operation", "mutate" },
                                        { "path", "design.kicad_pcb" },
                                        { "itemType", "trace" },
                                        { "action", "delete" },
                                        { "ids", JSON::array( { KIID().AsStdString() } ) } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "snapshot_required" );

    result = registry.Handle(
            "verify", { { "operation", "erc" }, { "path", "design.kicad_pcb" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(), "invalid_path" );

    result = registry.Handle( "verify", { { "operation", "drc" },
                                            { "path", "design.kicad_pcb" },
                                            { "offset", "zero" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "invalid_arguments" );

    result = registry.Handle( "verify", { { "operation", "drc" },
                                            { "path", "design.kicad_pcb" },
                                            { "maxAgeDays", 7 } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "invalid_arguments" );

    result = registry.Handle( "verify", { { "operation", "sourcing" },
                                            { "path", "malformed.kicad_kds" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "compile_failed" );

    result = registry.Handle( "verify", { { "operation", "sourcing" },
                                            { "path", "design.kicad_pcb" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "invalid_path" );

    result = registry.Handle( "verify", { { "operation", "sourcing" },
                                            { "path", "malformed.kicad_kds" },
                                            { "maxAgeDays", 0 } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "invalid_arguments" );
}


BOOST_AUTO_TEST_CASE( CreatesPcbItemsInsideAnIpcTransaction )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName socketPath( fixture.Root(), wxS( "api-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-tool-token";
    const std::string commitId = KIID().AsStdString();
    const std::string createdId = KIID().AsStdString();
    std::atomic<int> beginCount( 0 );
    std::atomic<int> commitCount( 0 );

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    if( ++beginCount == 1 )
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                        response.mutable_status()->set_error_message(
                                "an earlier commit response was lost" );
                    }
                    else
                    {
                        kiapi::common::commands::BeginCommitResponse begin;
                        begin.mutable_id()->set_value( commitId );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( begin );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::CreateItems>() )
                {
                    kiapi::common::commands::CreateItems create;
                    kiapi::board::types::Track track;

                    if( request.header().kicad_token() == token
                        && request.message().UnpackTo( &create ) && create.items_size() == 1
                        && create.items( 0 ).UnpackTo( &track ) )
                    {
                        track.mutable_id()->set_value( createdId );
                        kiapi::common::commands::CreateItemsResponse created;
                        created.mutable_header()->CopyFrom( create.header() );
                        created.set_status( kiapi::common::types::IRS_OK );
                        auto* item = created.add_created_items();
                        item->mutable_status()->set_code( kiapi::common::commands::ISC_OK );
                        item->mutable_item()->PackFrom( track );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( created );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::UpdateItems>() )
                {
                    kiapi::common::commands::UpdateItems update;
                    kiapi::board::types::Track track;

                    if( request.message().UnpackTo( &update ) && update.items_size() == 1
                        && update.items( 0 ).UnpackTo( &track )
                        && track.id().value() == createdId
                        && update.header().field_mask().paths_size() == 1 )
                    {
                        kiapi::common::commands::UpdateItemsResponse updated;
                        updated.mutable_header()->CopyFrom( update.header() );
                        updated.set_status( kiapi::common::types::IRS_OK );
                        auto* item = updated.add_updated_items();
                        item->mutable_status()->set_code( kiapi::common::commands::ISC_OK );
                        item->mutable_item()->PackFrom( track );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( updated );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::DeleteItems>() )
                {
                    kiapi::common::commands::DeleteItems remove;

                    if( request.message().UnpackTo( &remove ) && remove.item_ids_size() == 1
                        && remove.item_ids( 0 ).value() == createdId )
                    {
                        kiapi::common::commands::DeleteItemsResponse deleted;
                        deleted.mutable_header()->CopyFrom( remove.header() );
                        deleted.set_status( kiapi::common::types::IRS_OK );
                        auto* item = deleted.add_deleted_items();
                        item->mutable_id()->set_value( createdId );
                        item->set_status( kiapi::common::commands::IDS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( deleted );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;

                    if( request.message().UnpackTo( &end ) && end.id().value() == commitId
                        && end.action() == kiapi::common::commands::CMA_COMMIT )
                    {
                        ++commitCount;
                    }

                    kiapi::common::commands::EndCommitResponse ended;
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( ended );
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; },
                                  [&fixture]() { return fixture.Root(); } );
    JSON result = registry.Handle(
            "pcb", { { "operation", "mutate" }, { "path", "design.kicad_pcb" },
                     { "itemType", "trace" }, { "action", "create" },
                     { "commitMessage", "Route QA trace" },
                     { "items",
                       JSON::array( { { { "start", { { "xNm", "1000000" },
                                                       { "yNm", "2000000" } } },
                                        { "end", { { "xNm", "3000000" },
                                                     { "yNm", "4000000" } } },
                                        { "width", { { "valueNm", "250000" } } },
                                        { "layer", "BL_F_Cu" },
                                        { "net", { { "name", "GND" } } } } } ) } } );

    BOOST_REQUIRE_MESSAGE( result.at( "success" ).get<bool>(), result.dump() );
    JSON data = envelope( result ).at( "data" );
    BOOST_CHECK_EQUAL( data.at( "transaction" ).get<std::string>(), "committed" );
    BOOST_REQUIRE_EQUAL( data.at( "itemIds" ).size(), 1 );
    BOOST_CHECK_EQUAL( data["itemIds"][0].get<std::string>(), createdId );
    BOOST_CHECK_EQUAL( beginCount.load(), 2 );
    BOOST_CHECK_EQUAL( commitCount.load(), 1 );

    result = registry.Handle(
            "pcb", { { "operation", "mutate" }, { "path", "design.kicad_pcb" },
                     { "itemType", "trace" }, { "action", "update" },
                     { "fieldMask", JSON::array( { "width" } ) },
                     { "items",
                       JSON::array( { { { "id", { { "value", createdId } } },
                                        { "width", { { "valueNm", "300000" } } } } } ) } } );
    BOOST_REQUIRE_MESSAGE( result.at( "success" ).get<bool>(), result.dump() );
    BOOST_CHECK_EQUAL( envelope( result )["data"]["transaction"].get<std::string>(),
                       "committed" );

    result = registry.Handle(
            "pcb", { { "operation", "mutate" }, { "path", "design.kicad_pcb" },
                     { "itemType", "trace" }, { "action", "delete" },
                     { "ids", JSON::array( { createdId } ) } } );
    BOOST_REQUIRE_MESSAGE( result.at( "success" ).get<bool>(), result.dump() );
    BOOST_CHECK_EQUAL( envelope( result )["data"]["affectedItems"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( beginCount.load(), 4 );
    BOOST_CHECK_EQUAL( commitCount.load(), 3 );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( AppliesReusableDesignsIdempotentlyWithManagedState )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName socketPath( fixture.Root(), wxS( "api-design-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-design-token";
    std::map<std::string, google::protobuf::Any> liveItems;
    std::map<std::string, google::protobuf::Any> transactionBefore;
    int commitCount = 0;
    int stackupUpdateCount = 0;
    int rulesUpdateCount = 0;
    bool rejectNextCreate = false;
    bool rejectNextCommitResponse = false;
    bool rejectNextRulesResponse = false;
    bool rejectNextStackupResponse = false;
    bool transactionActive = false;
    kiapi::board::BoardStackup currentStackup = mockTwoLayerStackup( "None" );
    kiapi::board::BoardDesignRules currentRules = mockBoardRules( 100000 );

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::GetItemsById>() )
                {
                    kiapi::common::commands::GetItemsById get;
                    kiapi::common::commands::GetItemsResponse items;

                    if( request.message().UnpackTo( &get ) )
                    {
                        for( const auto& id : get.items() )
                        {
                            auto item = liveItems.find( id.value() );

                            if( item != liveItems.end() )
                                items.add_items()->CopyFrom( item->second );
                        }

                        items.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( items );
                    }
                }
                else if( request.message().Is<kiapi::board::commands::GetBoardStackup>() )
                {
                    kiapi::board::commands::BoardStackupResponse stackup;
                    stackup.mutable_stackup()->CopyFrom( currentStackup );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( stackup );
                }
                else if( request.message().Is<kiapi::board::commands::UpdateBoardStackup>() )
                {
                    kiapi::board::commands::UpdateBoardStackup update;

                    if( request.message().UnpackTo( &update ) && update.has_stackup() )
                    {
                        currentStackup.CopyFrom( update.stackup() );
                        ++stackupUpdateCount;

                        if( rejectNextStackupResponse )
                        {
                            rejectNextStackupResponse = false;
                            response.mutable_status()->set_status( kiapi::common::AS_TIMEOUT );
                            response.mutable_status()->set_error_message(
                                    "injected lost stackup acknowledgement" );
                        }
                        else
                        {
                            kiapi::board::commands::BoardStackupResponse stackup;
                            stackup.mutable_stackup()->CopyFrom( currentStackup );
                            response.mutable_status()->set_status( kiapi::common::AS_OK );
                            response.mutable_message()->PackFrom( stackup );
                        }
                    }
                }
                else if( request.message().Is<kiapi::board::commands::GetBoardDesignRules>() )
                {
                    kiapi::board::commands::BoardDesignRulesResponse rules;
                    rules.mutable_rules()->CopyFrom( currentRules );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( rules );
                }
                else if( request.message().Is<kiapi::board::commands::UpdateBoardDesignRules>() )
                {
                    kiapi::board::commands::UpdateBoardDesignRules update;

                    if( request.message().UnpackTo( &update ) && update.has_rules() )
                    {
                        currentRules.CopyFrom( update.rules() );
                        ++rulesUpdateCount;

                        if( rejectNextRulesResponse )
                        {
                            rejectNextRulesResponse = false;
                            response.mutable_status()->set_status( kiapi::common::AS_TIMEOUT );
                            response.mutable_status()->set_error_message(
                                    "injected lost board-rules acknowledgement" );
                        }
                        else
                        {
                            kiapi::board::commands::BoardDesignRulesResponse rules;
                            rules.mutable_rules()->CopyFrom( currentRules );
                            response.mutable_status()->set_status( kiapi::common::AS_OK );
                            response.mutable_message()->PackFrom( rules );
                        }
                    }
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    if( transactionActive )
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                        response.mutable_status()->set_error_message( "transaction already active" );
                    }
                    else
                    {
                        transactionActive = true;
                        transactionBefore = liveItems;
                        kiapi::common::commands::BeginCommitResponse begin;
                        begin.mutable_id()->set_value( KIID().AsStdString() );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( begin );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::CreateItems>() )
                {
                    kiapi::common::commands::CreateItems create;
                    kiapi::common::commands::CreateItemsResponse created;

                    if( request.message().UnpackTo( &create ) )
                    {
                        for( const google::protobuf::Any& packed : create.items() )
                        {
                            std::string id;
                            kiapi::board::types::BoardGraphicShape shape;
                            kiapi::board::types::Track track;

                            if( packed.UnpackTo( &shape ) )
                                id = shape.id().value();
                            else if( packed.UnpackTo( &track ) )
                                id = track.id().value();

                            auto* result = created.add_created_items();

                            if( rejectNextCreate )
                            {
                                rejectNextCreate = false;
                                result->mutable_status()->set_code(
                                        kiapi::common::commands::ISC_INVALID_DATA );
                                result->mutable_status()->set_error_message( "injected failure" );
                            }
                            else if( id.empty() || liveItems.contains( id ) )
                            {
                                result->mutable_status()->set_code(
                                        kiapi::common::commands::ISC_EXISTING );
                            }
                            else
                            {
                                liveItems[id] = packed;
                                result->mutable_status()->set_code(
                                        kiapi::common::commands::ISC_OK );
                                result->mutable_item()->CopyFrom( packed );
                            }
                        }

                        created.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( created );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::UpdateItems>() )
                {
                    kiapi::common::commands::UpdateItems update;
                    kiapi::common::commands::UpdateItemsResponse updated;

                    if( request.message().UnpackTo( &update ) )
                    {
                        for( const google::protobuf::Any& packed : update.items() )
                        {
                            std::string id;
                            kiapi::board::types::BoardGraphicShape shape;
                            kiapi::board::types::Track track;

                            if( packed.UnpackTo( &shape ) )
                                id = shape.id().value();
                            else if( packed.UnpackTo( &track ) )
                                id = track.id().value();

                            auto* result = updated.add_updated_items();

                            if( id.empty() || !liveItems.contains( id ) )
                            {
                                result->mutable_status()->set_code(
                                        kiapi::common::commands::ISC_NONEXISTENT );
                            }
                            else
                            {
                                liveItems[id] = packed;
                                result->mutable_status()->set_code(
                                        kiapi::common::commands::ISC_OK );
                                result->mutable_item()->CopyFrom( packed );
                            }
                        }

                        updated.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( updated );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::DeleteItems>() )
                {
                    kiapi::common::commands::DeleteItems remove;
                    kiapi::common::commands::DeleteItemsResponse deleted;

                    if( request.message().UnpackTo( &remove ) )
                    {
                        for( const auto& id : remove.item_ids() )
                        {
                            auto* result = deleted.add_deleted_items();
                            result->mutable_id()->CopyFrom( id );
                            result->set_status( liveItems.erase( id.value() ) == 1
                                                        ? kiapi::common::commands::IDS_OK
                                                        : kiapi::common::commands::IDS_NONEXISTENT );
                        }

                        deleted.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( deleted );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;
                    request.message().UnpackTo( &end );
                    bool rejectResponse = false;

                    if( end.action() == kiapi::common::commands::CMA_COMMIT )
                    {
                        if( transactionActive )
                        {
                            transactionActive = false;
                            ++commitCount;
                            rejectResponse = rejectNextCommitResponse;
                            rejectNextCommitResponse = false;
                        }
                        else
                        {
                            response.mutable_status()->set_status(
                                    kiapi::common::AS_BAD_REQUEST );
                            response.mutable_status()->set_error_message(
                                    "no transaction is active" );
                        }
                    }
                    else if( end.action() == kiapi::common::commands::CMA_DROP )
                    {
                        if( transactionActive )
                        {
                            transactionActive = false;
                            liveItems = transactionBefore;
                        }
                        else
                        {
                            response.mutable_status()->set_status(
                                    kiapi::common::AS_BAD_REQUEST );
                            response.mutable_status()->set_error_message(
                                    "no transaction is active" );
                        }
                    }

                    kiapi::common::commands::EndCommitResponse ended;

                    if( rejectResponse )
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                        response.mutable_status()->set_error_message( "injected lost acknowledgement" );
                    }
                    else if( response.status().status() == kiapi::common::AS_UNKNOWN )
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( ended );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::SaveDocument>() )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; },
                                  [&fixture]() { return fixture.Root(); } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project managed_design)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net SIGNAL (pin R1 1 1) (pin R2 1 1))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled false)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.53mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked false))
        (copper B.Cu (thickness 35um))))
    (outline
      (rectangle edge (start 0mm 0mm) (end 20mm 10mm)
        (radius 0mm) (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))
    (route SIGNAL (id trace-a) (from 1mm 2mm) (to 3mm 4mm)
      (width 0.25mm) (layer F.Cu)))
  (rules
    (minimum_clearance 0.2mm) (minimum_connection_width 0.2mm)
    (minimum_track_width 0.2mm) (minimum_via_annular_width 0.1mm)
    (minimum_via_diameter 0.6mm) (minimum_through_hole_diameter 0.3mm)
    (minimum_microvia_diameter 0.3mm) (minimum_microvia_drill 0.1mm)
    (minimum_hole_to_hole 0.25mm) (minimum_copper_to_hole_clearance 0.25mm)
    (minimum_silkscreen_clearance 0mm) (minimum_groove_width 0.25mm)
    (minimum_resolved_spokes 2) (minimum_silkscreen_text_height 0.8mm)
    (minimum_silkscreen_text_thickness 0.08mm)
    (minimum_copper_to_edge_clearance 0.5mm)
    (use_height_for_length_calculations true)
    (maximum_error 0.005mm)
    (allow_fillets_outside_zone_outline false)))
)KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                               { "path", "managed.kicad_kds" },
                                               { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                 { "path", "managed.kicad_kds" },
                                                 { "boardPath", "design.kicad_pcb" },
                                                 { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    BOOST_CHECK_EQUAL( envelope( applied )["data"]["counts"]["create"].get<int>(), 2 );
    BOOST_CHECK( envelope( applied )["data"]["rulesApplied"].get<bool>() );
    BOOST_CHECK_EQUAL( liveItems.size(), 2 );
    BOOST_CHECK_EQUAL( commitCount, 1 );
    BOOST_CHECK_EQUAL( currentStackup.finish().type_name(), "ENIG" );
    BOOST_CHECK_EQUAL( stackupUpdateCount, 1 );
    BOOST_CHECK_EQUAL( currentRules.minimum_clearance().value_nm(), 200000 );
    BOOST_CHECK_EQUAL( rulesUpdateCount, 1 );

    JSON repeated = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", "managed.kicad_kds" },
                                                  { "boardPath", "design.kicad_pcb" },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( repeated.at( "success" ).get<bool>(), repeated.dump() );
    BOOST_CHECK_EQUAL( envelope( repeated )["data"]["counts"]["update"].get<int>(), 2 );
    BOOST_CHECK( envelope( repeated )["data"]["rulesApplied"].get<bool>() );
    BOOST_CHECK_EQUAL( liveItems.size(), 2 );
    BOOST_CHECK_EQUAL( currentStackup.finish().type_name(), "ENIG" );
    BOOST_CHECK_EQUAL( stackupUpdateCount, 2 );
    BOOST_CHECK_EQUAL( currentRules.minimum_clearance().value_nm(), 200000 );
    BOOST_CHECK_EQUAL( rulesUpdateCount, 2 );

    const std::string reduced = R"KDS((kichad_design
  (version 1)
  (project managed_design)
  (board (outline
    (rectangle edge (start 0mm 0mm) (end 25mm 10mm)
      (radius 0mm) (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))))
)KDS";
    JSON replaced = registry.Handle( "design", { { "operation", "save" },
                                                  { "path", "managed.kicad_kds" },
                                                  { "source", reduced },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( replaced.at( "success" ).get<bool>(), replaced.dump() );
    hash = envelope( replaced )["data"]["sourceSha256"].get<std::string>();
    JSON reducedApply = registry.Handle( "design", { { "operation", "apply" },
                                                      { "path", "managed.kicad_kds" },
                                                      { "boardPath", "design.kicad_pcb" },
                                                      { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( reducedApply.at( "success" ).get<bool>(), reducedApply.dump() );
    BOOST_CHECK_EQUAL( envelope( reducedApply )["data"]["counts"]["delete"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( liveItems.size(), 1 );
    BOOST_CHECK( wxFileExists( wxFileName( fixture.Root(),
                                           wxS( "managed.kicad_kds_state" ) ).GetFullPath() ) );
    BOOST_CHECK( !wxFileExists( wxFileName( fixture.Root(),
                                            wxS( "managed.kicad_kds_journal" ) ).GetFullPath() ) );

    const std::string recoveredSource = R"KDS((kichad_design
  (version 1)
  (project managed_design)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net SIGNAL (pin R1 1 1) (pin R2 1 1))
  (board
    (stackup
      (finish "HASL") (impedance_controlled true)
      (edge_connector yes) (edge_plating true)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.53mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (outline
      (rectangle edge (start 0mm 0mm) (end 30mm 10mm)
        (radius 0mm) (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))
    (route SIGNAL (id recovered-trace) (from 1mm 2mm) (to 4mm 4mm)
      (width 0.3mm) (layer F.Cu)))
  (rules
    (minimum_clearance 0.3mm) (minimum_connection_width 0.2mm)
    (minimum_track_width 0.2mm) (minimum_via_annular_width 0.1mm)
    (minimum_via_diameter 0.6mm) (minimum_through_hole_diameter 0.3mm)
    (minimum_microvia_diameter 0.3mm) (minimum_microvia_drill 0.1mm)
    (minimum_hole_to_hole 0.25mm) (minimum_copper_to_hole_clearance 0.25mm)
    (minimum_silkscreen_clearance 0mm) (minimum_groove_width 0.25mm)
    (minimum_resolved_spokes 2) (minimum_silkscreen_text_height 0.8mm)
    (minimum_silkscreen_text_thickness 0.08mm)
    (minimum_copper_to_edge_clearance legacy)
    (use_height_for_length_calculations false)
    (maximum_error 0.01mm)
    (allow_fillets_outside_zone_outline true)))
)KDS";
    replaced = registry.Handle( "design", { { "operation", "save" },
                                             { "path", "managed.kicad_kds" },
                                             { "source", recoveredSource },
                                             { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( replaced.at( "success" ).get<bool>(), replaced.dump() );
    hash = envelope( replaced )["data"]["sourceSha256"].get<std::string>();

    rejectNextStackupResponse = true;
    JSON lostStackup = registry.Handle( "design", { { "operation", "apply" },
                                                     { "path", "managed.kicad_kds" },
                                                     { "boardPath", "design.kicad_pcb" },
                                                     { "expectedSha256", hash } } );
    BOOST_CHECK( !lostStackup.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( lostStackup )["error"]["code"].get<std::string>(),
                       "stackup_apply_failed" );
    BOOST_CHECK_EQUAL( currentStackup.finish().type_name(), "ENIG" );
    BOOST_CHECK_EQUAL( stackupUpdateCount, 4 );
    BOOST_CHECK_EQUAL( currentRules.minimum_clearance().value_nm(), 200000 );
    BOOST_CHECK_EQUAL( rulesUpdateCount, 2 );

    rejectNextRulesResponse = true;
    JSON lostRules = registry.Handle( "design", { { "operation", "apply" },
                                                   { "path", "managed.kicad_kds" },
                                                   { "boardPath", "design.kicad_pcb" },
                                                   { "expectedSha256", hash } } );
    BOOST_CHECK( !lostRules.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( lostRules )["error"]["code"].get<std::string>(),
                       "rules_apply_failed" );
    BOOST_CHECK_EQUAL( currentStackup.finish().type_name(), "ENIG" );
    BOOST_CHECK_EQUAL( stackupUpdateCount, 6 );
    BOOST_CHECK_EQUAL( currentRules.minimum_clearance().value_nm(), 200000 );
    BOOST_CHECK_EQUAL( rulesUpdateCount, 4 );

    rejectNextCreate = true;
    JSON failed = registry.Handle( "design", { { "operation", "apply" },
                                                { "path", "managed.kicad_kds" },
                                                { "boardPath", "design.kicad_pcb" },
                                                { "expectedSha256", hash } } );
    BOOST_CHECK( !failed.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( failed )["error"]["code"].get<std::string>(), "apply_failed" );
    BOOST_CHECK_EQUAL( liveItems.size(), 1 );
    BOOST_CHECK_EQUAL( currentStackup.finish().type_name(), "ENIG" );
    BOOST_CHECK_EQUAL( stackupUpdateCount, 8 );
    BOOST_CHECK_EQUAL( currentRules.minimum_clearance().value_nm(), 200000 );
    BOOST_CHECK_EQUAL( rulesUpdateCount, 6 );
    BOOST_CHECK( wxFileExists( wxFileName( fixture.Root(),
                                           wxS( "managed.kicad_kds_journal" ) ).GetFullPath() ) );

    rejectNextCommitResponse = true;
    JSON ambiguous = registry.Handle( "design", { { "operation", "apply" },
                                                    { "path", "managed.kicad_kds" },
                                                    { "boardPath", "design.kicad_pcb" },
                                                    { "expectedSha256", hash } } );
    BOOST_CHECK( !ambiguous.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( ambiguous )["error"]["code"].get<std::string>(),
                       "transaction_failed" );
    BOOST_CHECK_EQUAL( liveItems.size(), 2 );
    BOOST_CHECK_EQUAL( currentStackup.finish().type_name(), "ENIG" );
    BOOST_CHECK_EQUAL( stackupUpdateCount, 10 );
    BOOST_CHECK_EQUAL( currentRules.minimum_clearance().value_nm(), 200000 );
    BOOST_CHECK_EQUAL( rulesUpdateCount, 8 );
    BOOST_CHECK( wxFileExists( wxFileName( fixture.Root(),
                                           wxS( "managed.kicad_kds_journal" ) ).GetFullPath() ) );

    JSON recovered = registry.Handle( "design", { { "operation", "apply" },
                                                    { "path", "managed.kicad_kds" },
                                                    { "boardPath", "design.kicad_pcb" },
                                                    { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( recovered.at( "success" ).get<bool>(), recovered.dump() );
    BOOST_CHECK_EQUAL( liveItems.size(), 2 );
    BOOST_CHECK_EQUAL( envelope( recovered )["data"]["counts"]["update"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( currentStackup.finish().type_name(), "HASL" );
    BOOST_CHECK_EQUAL( stackupUpdateCount, 11 );
    BOOST_CHECK_EQUAL( currentRules.minimum_clearance().value_nm(), 300000 );
    BOOST_CHECK_EQUAL( currentRules.copper_edge_clearance_mode(),
                       kiapi::board::BCECM_LEGACY );
    BOOST_CHECK( !currentRules.use_height_for_length_calculations() );
    BOOST_CHECK_EQUAL( currentRules.maximum_error().value_nm(), 10000 );
    BOOST_CHECK( currentRules.allow_fillets_outside_zone_outline() );
    BOOST_CHECK_EQUAL( rulesUpdateCount, 9 );
    BOOST_CHECK( !wxFileExists( wxFileName( fixture.Root(),
                                            wxS( "managed.kicad_kds_journal" ) ).GetFullPath() ) );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( AppliesAndRollsBackCanonicalNetclassSettingsAtomically )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName socketPath( fixture.Root(), wxS( "api-netclasses-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-netclasses-token";
    int updateCount = 0;
    bool rejectNextUpdateResponse = false;
    kiapi::common::project::NetClassSettings currentSettings =
            mockNetClassSettings( 100000 );

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::GetNetClassSettings>() )
                {
                    kiapi::common::commands::NetClassSettingsResponse settings;
                    settings.mutable_settings()->CopyFrom( currentSettings );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( settings );
                }
                else if( request.message().Is<kiapi::common::commands::UpdateNetClassSettings>() )
                {
                    kiapi::common::commands::UpdateNetClassSettings update;

                    if( request.message().UnpackTo( &update ) && update.has_settings() )
                    {
                        currentSettings.CopyFrom( update.settings() );
                        ++updateCount;

                        if( rejectNextUpdateResponse )
                        {
                            rejectNextUpdateResponse = false;
                            response.mutable_status()->set_status( kiapi::common::AS_TIMEOUT );
                            response.mutable_status()->set_error_message(
                                    "injected lost netclass acknowledgement" );
                        }
                        else
                        {
                            kiapi::common::commands::NetClassSettingsResponse settings;
                            settings.mutable_settings()->CopyFrom( currentSettings );
                            response.mutable_status()->set_status( kiapi::common::AS_OK );
                            response.mutable_message()->PackFrom( settings );
                        }
                    }
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; },
                                  [&fixture]() { return fixture.Root(); } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project netclass_design)
  (net_classes
    (class Default
      (clearance 0.2mm) (track_width 0.2mm)
      (via_diameter 0.6mm) (via_drill 0.3mm)
      (microvia_diameter 0.3mm) (microvia_drill 0.1mm)
      (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
      (tuning_profile none) (pcb_color default)
      (wire_width 0.15mm) (bus_width 0.3mm)
      (schematic_color default) (line_style solid))
    (class USB_HS
      (clearance inherit) (track_width 0.15mm)
      (via_diameter inherit) (via_drill inherit)
      (microvia_diameter inherit) (microvia_drill inherit)
      (diff_pair_width 0.15mm) (diff_pair_gap 0.18mm) (diff_pair_via_gap inherit)
      (tuning_profile usb_hs) (pcb_color "#112233")
      (wire_width inherit) (bus_width inherit)
      (schematic_color "#AABBCC") (line_style dash_dot))
    (assign (pattern "USB_[PN]") (classes USB_HS))))
)KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "netclasses.kicad_kds" },
                                                { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON preview = registry.Handle( "design", { { "operation", "preview" },
                                                  { "path", "netclasses.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( preview.at( "success" ).get<bool>(), preview.dump() );
    const JSON previewData = envelope( preview );
    const JSON& previewItems = previewData["data"]["boardPlan"]["items"];
    BOOST_REQUIRE_EQUAL( previewItems.size(), 1 );
    BOOST_CHECK_EQUAL( previewItems[0]["action"].get<std::string>(),
                       "configure_net_classes" );
    BOOST_CHECK_EQUAL( previewItems[0]["classes"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( previewItems[0]["assignments"].get<int>(), 1 );

    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", "netclasses.kicad_kds" },
                                                  { "boardPath", "design.kicad_pcb" },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    BOOST_CHECK( envelope( applied )["data"]["netClassesApplied"].get<bool>() );
    BOOST_REQUIRE_EQUAL( currentSettings.net_classes_size(), 2 );
    BOOST_CHECK_EQUAL( currentSettings.net_classes( 0 ).board().clearance().value_nm(),
                       200000 );
    BOOST_CHECK_EQUAL( currentSettings.assignments_size(), 1 );
    BOOST_CHECK_EQUAL( updateCount, 1 );

    const std::string changed = std::regex_replace(
            source, std::regex( "\\(clearance 0\\.2mm\\)" ), "(clearance 0.3mm)" );
    saved = registry.Handle( "design", { { "operation", "save" },
                                          { "path", "netclasses.kicad_kds" },
                                          { "source", changed },
                                          { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    rejectNextUpdateResponse = true;
    JSON lost = registry.Handle( "design", { { "operation", "apply" },
                                               { "path", "netclasses.kicad_kds" },
                                               { "boardPath", "design.kicad_pcb" },
                                               { "expectedSha256", hash } } );
    BOOST_CHECK( !lost.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( lost )["error"]["code"].get<std::string>(),
                       "netclass_apply_failed" );
    BOOST_CHECK_EQUAL( currentSettings.net_classes( 0 ).board().clearance().value_nm(),
                       200000 );
    BOOST_CHECK_EQUAL( updateCount, 3 );
    wxFileName statePath( fixture.Root(), wxS( "netclasses.kicad_kds_state" ) );
    BOOST_REQUIRE( wxRemoveFile( statePath.GetFullPath() ) );
    BOOST_REQUIRE( wxFileName::Mkdir(
            statePath.GetFullPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    JSON stateFailure = registry.Handle( "design", { { "operation", "apply" },
                                                       { "path", "netclasses.kicad_kds" },
                                                       { "boardPath", "design.kicad_pcb" },
                                                       { "expectedSha256", hash } } );
    BOOST_CHECK( !stateFailure.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( stateFailure )["error"]["code"].get<std::string>(),
                       "state_write_failed" );
    BOOST_CHECK_EQUAL( currentSettings.net_classes( 0 ).board().clearance().value_nm(),
                       200000 );
    BOOST_CHECK_EQUAL( updateCount, 5 );
    BOOST_CHECK( wxFileExists( wxFileName( fixture.Root(),
                                           wxS( "netclasses.kicad_kds_journal" ) )
                                      .GetFullPath() ) );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( AppliesAndRollsBackGeneratedCustomRulesAtomically )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName socketPath( fixture.Root(), wxS( "api-custom-rules-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-custom-rules-token";
    int updateCount = 0;
    bool rejectNextUpdateResponse = false;
    bool currentPresent = false;
    std::string currentSource;

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::board::commands::GetBoardCustomRules>() )
                {
                    kiapi::board::commands::BoardCustomRulesResponse rules;
                    rules.set_present( currentPresent );
                    rules.set_source( currentSource );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( rules );
                }
                else if( request.message().Is<kiapi::board::commands::UpdateBoardCustomRules>() )
                {
                    kiapi::board::commands::UpdateBoardCustomRules update;

                    if( request.message().UnpackTo( &update ) )
                    {
                        currentPresent = update.present();
                        currentSource = update.source();
                        ++updateCount;

                        if( rejectNextUpdateResponse )
                        {
                            rejectNextUpdateResponse = false;
                            response.mutable_status()->set_status( kiapi::common::AS_TIMEOUT );
                            response.mutable_status()->set_error_message(
                                    "injected lost custom-rules acknowledgement" );
                        }
                        else
                        {
                            kiapi::board::commands::BoardCustomRulesResponse rules;
                            rules.set_present( currentPresent );
                            rules.set_source( currentSource );
                            response.mutable_status()->set_status( kiapi::common::AS_OK );
                            response.mutable_message()->PackFrom( rules );
                        }
                    }
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; },
                                  [&fixture]() { return fixture.Root(); } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project custom_rules_design)
  (library symbol Device (table global))
  (library symbol LocalSymbols (table project)
    (uri "${KIPRJMOD}/libraries/local-one.kicad_sym"))
  (library footprint Resistor_SMD (table global))
  (library footprint LocalFootprints (table project)
    (uri "${KIPRJMOD}/libraries/LocalOne.pretty"))
  (custom_rules
    (rule signal_clearance
      (condition "A.NetName == 'SIGNAL'")
      (layer F.Cu)
      (severity error)
      (constraint clearance (min 0.2mm))
      (constraint track_width (min 0.15mm) (opt 0.2mm) (max 0.4mm)))))
)KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "custom_rules.kicad_kds" },
                                                { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON preview = registry.Handle( "design", { { "operation", "preview" },
                                                  { "path", "custom_rules.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( preview.at( "success" ).get<bool>(), preview.dump() );
    const JSON previewData = envelope( preview );
    const JSON& items = previewData["data"]["boardPlan"]["items"];
    BOOST_REQUIRE_EQUAL( items.size(), 3 );
    BOOST_CHECK_EQUAL( items[0]["action"].get<std::string>(),
                       "configure_project_library_table" );
    BOOST_CHECK_EQUAL( items[0]["kind"].get<std::string>(), "symbol" );
    BOOST_CHECK_EQUAL( items[0]["libraries"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( items[1]["action"].get<std::string>(),
                       "configure_project_library_table" );
    BOOST_CHECK_EQUAL( items[1]["kind"].get<std::string>(), "footprint" );
    BOOST_CHECK_EQUAL( items[1]["libraries"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( items[2]["action"].get<std::string>(),
                       "configure_custom_board_rules" );
    BOOST_CHECK_EQUAL( items[2]["rules"].get<int>(), 1 );

    wxFileName statePath( fixture.Root(), wxS( "custom_rules.kicad_kds_state" ) );
    BOOST_REQUIRE( wxFileName::Mkdir(
            statePath.GetFullPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    JSON initialStateFailure = registry.Handle(
            "design", { { "operation", "apply" },
                         { "path", "custom_rules.kicad_kds" },
                         { "boardPath", "design.kicad_pcb" },
                         { "expectedSha256", hash } } );
    BOOST_CHECK( !initialStateFailure.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( initialStateFailure )["error"]["code"].get<std::string>(),
                       "state_write_failed" );
    BOOST_CHECK( !currentPresent );
    BOOST_CHECK_EQUAL( updateCount, 2 );
    BOOST_CHECK( !wxFileExists(
            wxFileName( fixture.Root(), wxS( "sym-lib-table" ) ).GetFullPath() ) );
    BOOST_CHECK( !wxFileExists(
            wxFileName( fixture.Root(), wxS( "fp-lib-table" ) ).GetFullPath() ) );
    BOOST_REQUIRE( wxFileName::Rmdir( statePath.GetFullPath() ) );

    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", "custom_rules.kicad_kds" },
                                                  { "boardPath", "design.kicad_pcb" },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    BOOST_CHECK( envelope( applied )["data"]["customRulesApplied"].get<bool>() );
    BOOST_CHECK_EQUAL( envelope( applied )["data"]["libraryTablesApplied"].get<int>(), 2 );
    BOOST_CHECK( currentPresent );
    BOOST_CHECK_NE( currentSource.find( "(constraint clearance (min 0.2mm))" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( updateCount, 3 );
    const std::string installedSource = currentSource;
    const wxFileName symbolTable( fixture.Root(), wxS( "sym-lib-table" ) );
    const wxFileName footprintTable( fixture.Root(), wxS( "fp-lib-table" ) );
    const std::string installedSymbolTable = readExactTextFile( symbolTable );
    const std::string installedFootprintTable = readExactTextFile( footprintTable );
    BOOST_CHECK_NE( installedSymbolTable.find( "local-one.kicad_sym" ), std::string::npos );
    BOOST_CHECK_NE( installedFootprintTable.find( "LocalOne.pretty" ), std::string::npos );

    std::string changed = std::regex_replace(
            source, std::regex( "clearance \\(min 0\\.2mm\\)" ),
            "clearance (min 0.3mm)" );
    changed = std::regex_replace( changed, std::regex( "local-one\\.kicad_sym" ),
                                  "local-two.kicad_sym" );
    changed = std::regex_replace( changed, std::regex( "LocalOne\\.pretty" ),
                                  "LocalTwo.pretty" );
    saved = registry.Handle( "design", { { "operation", "save" },
                                          { "path", "custom_rules.kicad_kds" },
                                          { "source", changed },
                                          { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    rejectNextUpdateResponse = true;
    JSON lost = registry.Handle( "design", { { "operation", "apply" },
                                               { "path", "custom_rules.kicad_kds" },
                                               { "boardPath", "design.kicad_pcb" },
                                               { "expectedSha256", hash } } );
    BOOST_CHECK( !lost.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( lost )["error"]["code"].get<std::string>(),
                       "custom_rules_apply_failed" );
    BOOST_CHECK_EQUAL( currentSource, installedSource );
    BOOST_CHECK_EQUAL( updateCount, 5 );
    BOOST_CHECK_EQUAL( readExactTextFile( symbolTable ), installedSymbolTable );
    BOOST_CHECK_EQUAL( readExactTextFile( footprintTable ), installedFootprintTable );

    BOOST_REQUIRE( wxRemoveFile( statePath.GetFullPath() ) );
    BOOST_REQUIRE( wxFileName::Mkdir(
            statePath.GetFullPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    JSON stateFailure = registry.Handle( "design", { { "operation", "apply" },
                                                       { "path", "custom_rules.kicad_kds" },
                                                       { "boardPath", "design.kicad_pcb" },
                                                       { "expectedSha256", hash } } );
    BOOST_CHECK( !stateFailure.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( stateFailure )["error"]["code"].get<std::string>(),
                       "state_write_failed" );
    BOOST_CHECK_EQUAL( currentSource, installedSource );
    BOOST_CHECK_EQUAL( updateCount, 7 );
    BOOST_CHECK_EQUAL( readExactTextFile( symbolTable ), installedSymbolTable );
    BOOST_CHECK_EQUAL( readExactTextFile( footprintTable ), installedFootprintTable );
    BOOST_CHECK( wxFileExists( wxFileName( fixture.Root(),
                                           wxS( "custom_rules.kicad_kds_journal" ) )
                                      .GetFullPath() ) );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( AppliesAndRollsBackKdsOwnedNativeSymbolLibrary )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName libraries = wxFileName::DirName( fixture.Root() );
    libraries.AppendDir( wxS( "libraries" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( libraries.GetFullPath(), 0700 ) );
    wxFileName footprints = libraries;
    footprints.AppendDir( wxS( "Empty.pretty" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( footprints.GetFullPath(), 0700 ) );
    {
        wxFFile schematic( wxFileName( fixture.Root(), wxS( "design.kicad_sch" ) )
                                   .GetFullPath(), wxS( "wb" ) );
        BOOST_REQUIRE( schematic.IsOpened() );
        BOOST_REQUIRE( schematic.Write(
                wxS( "(kicad_sch\n"
                     "  (version 20260306)\n"
                     "  (generator \"eeschema\")\n"
                     "  (generator_version \"10.0\")\n"
                     "  (uuid \"11111111-2222-4333-8444-555555555555\")\n"
                     "  (paper \"A4\")\n"
                     "  (lib_symbols)\n"
                     "  (sheet_instances (path \"/\" (page \"1\")))\n"
                     "  (embedded_fonts no)\n"
                     ")\n" ) ) );
    }
    wxFileName socketPath( fixture.Root(), wxS( "api-managed-symbol-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-managed-symbol-token";

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    bool rejectSymbol = false;
    int symbolValidationCalls = 0;
    CODEX_TOOL_REGISTRY registry(
            [&fixture]() { return fixture.Root(); }, []() { return true; },
            [&fixture]() { return fixture.Root(); },
            []( const wxFileName&, const JSON&, const JSON&, std::string& )
            {
                return true;
            }, {}, {},
            [&]( const wxFileName& path, std::string& error )
            {
                ++symbolValidationCalls;
                BOOST_CHECK_EQUAL( path.GetFullName(), wxS( "Product.kicad_sym" ) );

                if( rejectSymbol )
                {
                    error = "injected native symbol rejection";
                    return false;
                }

                return true;
            } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project design)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/libraries/Product.kicad_sym") (managed true))
  (library footprint Empty (table project)
    (uri "${KIPRJMOD}/libraries/Empty.pretty"))
  (symbol Product:R
    (reference R) (value R) (description "Managed resistor")
    (unit common
      (rectangle body (from -1.016mm -2.54mm) (to 1.016mm 2.54mm)
        (stroke 0.254mm default) (fill none)))
    (unit 1
      (pin 1 (at 0mm 3.81mm) (orientation up) (length 1.27mm))
      (pin 2 (at 0mm -3.81mm) (orientation down) (length 1.27mm))))
  (sheet root (parent none) (file "design.kicad_sch") (title "Managed"))
  (component R1 (symbol "Product:R") (value "10k") (footprint none)
    (unit 1 (sheet root) (at 20mm 20mm) (rotation 0deg) (mirror none))))
)KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "managed-symbol.kicad_kds" },
                                                { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON preview = registry.Handle( "design", { { "operation", "preview" },
                                                  { "path", "managed-symbol.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( preview.at( "success" ).get<bool>(), preview.dump() );
    BOOST_CHECK_EQUAL( envelope( preview )["data"]["managedSymbolLibraries"]["counts"]
                                         ["libraries"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( envelope( preview )["data"]["managedSymbolLibraries"]["counts"]
                                         ["pins"].get<int>(), 2 );

    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", "managed-symbol.kicad_kds" },
                                                  { "boardPath", "design.kicad_pcb" },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    BOOST_CHECK_EQUAL( envelope( applied )["data"]["managedSymbolLibrariesApplied"].get<int>(),
                       1 );
    BOOST_CHECK_EQUAL( symbolValidationCalls, 1 );
    const wxFileName symbolPath( libraries.GetFullPath(), wxS( "Product.kicad_sym" ) );
    BOOST_REQUIRE( symbolPath.FileExists() );
    const std::string installed = readExactTextFile( symbolPath );
    BOOST_CHECK_NE( installed.find( "Managed resistor" ), std::string::npos );

    std::string changed = std::regex_replace( source, std::regex( "Managed resistor" ),
                                               "Rejected resistor" );
    saved = registry.Handle( "design", { { "operation", "save" },
                                           { "path", "managed-symbol.kicad_kds" },
                                           { "source", changed },
                                           { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    rejectSymbol = true;
    JSON rejected = registry.Handle( "design", { { "operation", "apply" },
                                                   { "path", "managed-symbol.kicad_kds" },
                                                   { "boardPath", "design.kicad_pcb" },
                                                   { "expectedSha256", hash } } );
    BOOST_CHECK( !rejected.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( rejected )["error"]["code"].get<std::string>(),
                       "managed_symbol_library_validation_failed" );
    BOOST_CHECK_EQUAL( readExactTextFile( symbolPath ), installed );
    BOOST_CHECK_EQUAL( symbolValidationCalls, 2 );
    const wxFileName journalPath( fixture.Root(),
                                  wxS( "managed-symbol.kicad_kds_journal" ) );
    BOOST_REQUIRE( journalPath.FileExists() );
    const JSON journal = JSON::parse( readExactTextFile( journalPath ) );
    BOOST_REQUIRE_EQUAL( journal["previousManagedSymbolLibraries"].size(), 1 );
    BOOST_CHECK_EQUAL( journal["previousManagedSymbolLibraries"][0]["nickname"], "Product" );
    BOOST_CHECK_EQUAL( journal["previousManagedSymbolLibraries"][0]["path"],
                       "libraries/Product.kicad_sym" );
    BOOST_CHECK( journal["previousManagedSymbolLibraries"][0]["present"].get<bool>() );
    BOOST_CHECK( !journal["previousManagedSymbolLibraries"][0]["sourceBase64"]
                          .get<std::string>().empty() );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( AppliesAndRollsBackKdsOwnedNativeFootprintLibrary )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName libraries = wxFileName::DirName( fixture.Root() );
    libraries.AppendDir( wxS( "libraries" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( libraries.GetFullPath(), 0700 ) );
    wxFileName socketPath( fixture.Root(), wxS( "api-managed-footprint-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-managed-footprint-token";

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    bool rejectFootprint = false;
    int footprintValidationCalls = 0;
    CODEX_TOOL_REGISTRY registry(
            [&fixture]() { return fixture.Root(); }, []() { return true; },
            [&fixture]() { return fixture.Root(); }, {}, {}, {}, {},
            [&]( const wxFileName& path, std::string& error )
            {
                ++footprintValidationCalls;
                BOOST_CHECK_EQUAL( path.GetDirs().Last(), wxS( "Product.pretty" ) );

                if( rejectFootprint )
                {
                    error = "injected native footprint rejection";
                    return false;
                }

                return true;
            } );
    const std::string source = R"KDS((kichad_design
  (version 1) (project design)
  (library symbol Device (table global))
  (library footprint Product (table project)
    (uri "${KIPRJMOD}/libraries/Product.pretty") (managed true))
  (footprint Product:SENSOR_2P
    (reference U) (value SENSOR_2P) (description "Managed sensor footprint")
    (attributes (smd true) (allow_missing_courtyard true))
    (pad p1 (number 1) (type smd) (shape roundrect) (at -0.8mm 0mm)
      (size 0.8mm 0.8mm) (layers F.Cu F.Mask F.Paste)
      (roundrect_radius 0.2mm))
    (pad p2 (number 2) (type smd) (shape rect) (at 0.8mm 0mm)
      (size 0.8mm 0.8mm) (layers F.Cu F.Mask F.Paste)))
  (footprint Product:MOUNTING_HOLE
    (reference H) (value MOUNTING_HOLE)
    (pad h1 (number "") (type np_thru_hole) (shape circle) (at 0mm 0mm)
      (size 3mm 3mm) (layers all_copper all_mask) (drill round 3mm)))
))KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "managed-footprint.kicad_kds" },
                                                { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON preview = registry.Handle( "design", { { "operation", "preview" },
                                                  { "path", "managed-footprint.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( preview.at( "success" ).get<bool>(), preview.dump() );
    BOOST_CHECK_EQUAL( envelope( preview )["data"]["managedFootprintLibraries"]["counts"]
                                         ["libraries"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( envelope( preview )["data"]["managedFootprintLibraries"]["counts"]
                                         ["footprints"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( envelope( preview )["data"]["managedFootprintLibraries"]["counts"]
                                         ["pads"].get<int>(), 3 );

    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", "managed-footprint.kicad_kds" },
                                                  { "boardPath", "design.kicad_pcb" },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    BOOST_CHECK_EQUAL(
            envelope( applied )["data"]["managedFootprintLibrariesApplied"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( footprintValidationCalls, 1 );
    wxFileName footprintPath = libraries;
    footprintPath.AppendDir( wxS( "Product.pretty" ) );
    BOOST_REQUIRE( footprintPath.DirExists() );
    bool priorPresent = false;
    KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::FILES priorFiles;
    std::string ioError;
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::ReadOptional(
                                   footprintPath, priorPresent, priorFiles, ioError ), ioError );
    BOOST_CHECK( priorPresent );
    BOOST_REQUIRE_EQUAL( priorFiles.size(), 2 );
    BOOST_CHECK_NE( priorFiles["SENSOR_2P.kicad_mod"].find( "Managed sensor footprint" ),
                    std::string::npos );

    std::string changed = std::regex_replace( source,
                                               std::regex( "Managed sensor footprint" ),
                                               "Rejected sensor footprint" );
    saved = registry.Handle( "design", { { "operation", "save" },
                                           { "path", "managed-footprint.kicad_kds" },
                                           { "source", changed },
                                           { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    rejectFootprint = true;
    JSON rejected = registry.Handle( "design", { { "operation", "apply" },
                                                   { "path", "managed-footprint.kicad_kds" },
                                                   { "boardPath", "design.kicad_pcb" },
                                                   { "expectedSha256", hash } } );
    BOOST_CHECK( !rejected.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( rejected )["error"]["code"].get<std::string>(),
                       "managed_footprint_library_validation_failed" );
    bool restoredPresent = false;
    KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::FILES restoredFiles;
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::ReadOptional(
                                   footprintPath, restoredPresent, restoredFiles, ioError ),
                           ioError );
    BOOST_CHECK( restoredPresent );
    BOOST_CHECK( restoredFiles == priorFiles );
    BOOST_CHECK_EQUAL( footprintValidationCalls, 2 );
    const wxFileName journalPath( fixture.Root(),
                                  wxS( "managed-footprint.kicad_kds_journal" ) );
    BOOST_REQUIRE( journalPath.FileExists() );
    const JSON journal = JSON::parse( readExactTextFile( journalPath ) );
    BOOST_REQUIRE_EQUAL( journal["previousManagedFootprintLibraries"].size(), 1 );
    BOOST_CHECK_EQUAL( journal["previousManagedFootprintLibraries"][0]["nickname"],
                       "Product" );
    BOOST_CHECK_EQUAL( journal["previousManagedFootprintLibraries"][0]["path"],
                       "libraries/Product.pretty" );
    BOOST_CHECK( journal["previousManagedFootprintLibraries"][0]["present"].get<bool>() );
    BOOST_REQUIRE_EQUAL( journal["previousManagedFootprintLibraries"][0]["files"].size(), 2 );
    BOOST_CHECK( !journal["previousManagedFootprintLibraries"][0]["files"][0]
                          ["sourceBase64"].get<std::string>().empty() );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( AppliesKdsPlacementAsNarrowSchematicFootprintUpdate )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName socketPath( fixture.Root(), wxS( "api-placement-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-placement-token";
    const std::string commitId = "33333333-3333-8333-8333-333333333333";
    const std::string footprintId = "11111111-1111-8111-8111-111111111111";
    const std::string rootSheetId = "44444444-4444-8444-8444-444444444444";
    const std::string symbolId = "55555555-5555-8555-8555-555555555555";
    kiapi::board::types::FootprintInstance liveFootprint;
    liveFootprint.mutable_id()->set_value( footprintId );
    liveFootprint.mutable_position()->set_x_nm( 1000000 );
    liveFootprint.mutable_position()->set_y_nm( 2000000 );
    liveFootprint.mutable_orientation()->set_value_degrees( 0.0 );
    liveFootprint.set_layer( kiapi::board::types::BL_F_Cu );
    liveFootprint.set_locked( kiapi::common::types::LS_UNLOCKED );
    liveFootprint.mutable_reference_field()->mutable_text()->mutable_text()->set_text( "R1" );
    liveFootprint.mutable_definition()->mutable_id()->set_library_nickname( "R" );
    liveFootprint.mutable_definition()->mutable_id()->set_entry_name( "R" );
    liveFootprint.mutable_attributes()->set_not_in_schematic( false );
    liveFootprint.mutable_symbol_path()->add_path()->set_value( rootSheetId );
    liveFootprint.mutable_symbol_path()->add_path()->set_value( symbolId );
    bool sawFootprintInventory = false;
    bool sawNarrowUpdate = false;
    int  commitCount = 0;
    int  saveCount = 0;

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::GetItems>() )
                {
                    kiapi::common::commands::GetItems get;
                    kiapi::common::commands::GetItemsResponse items;

                    if( request.message().UnpackTo( &get ) && get.types_size() == 1
                        && get.types( 0 ) == kiapi::common::types::KOT_PCB_FOOTPRINT )
                    {
                        sawFootprintInventory = true;
                        items.add_items()->PackFrom( liveFootprint );
                        items.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( items );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::GetItemsById>() )
                {
                    kiapi::common::commands::GetItemsById get;
                    kiapi::common::commands::GetItemsResponse items;

                    if( request.message().UnpackTo( &get ) && get.items_size() == 1 )
                    {
                        // R1 is an existing user-owned footprint with a non-KDS UUID.  The exact
                        // deterministic ownership probe must therefore return no item before the
                        // reference inventory below resolves the user footprint.
                        items.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( items );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    kiapi::common::commands::BeginCommitResponse begin;
                    begin.mutable_id()->set_value( commitId );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( begin );
                }
                else if( request.message().Is<kiapi::common::commands::UpdateItems>() )
                {
                    kiapi::common::commands::UpdateItems update;
                    kiapi::board::types::FootprintInstance placement;
                    const std::set<std::string> expectedMask = {
                        "position", "orientation", "layer", "locked"
                    };
                    std::set<std::string> actualMask;

                    if( request.message().UnpackTo( &update ) )
                    {
                        actualMask.insert( update.header().field_mask().paths().begin(),
                                           update.header().field_mask().paths().end() );
                    }

                    if( update.items_size() == 1 && update.items( 0 ).UnpackTo( &placement )
                        && placement.id().value() == footprintId
                        && placement.position().x_nm() == 12500000
                        && placement.position().y_nm() == 7500000
                        && placement.orientation().value_degrees() == 37.5
                        && placement.layer() == kiapi::board::types::BL_B_Cu
                        && placement.locked() == kiapi::common::types::LS_LOCKED
                        && placement.symbol_path().path_size() == 0
                        && placement.definition().items_size() == 0
                        && actualMask == expectedMask )
                    {
                        sawNarrowUpdate = true;
                        kiapi::common::commands::UpdateItemsResponse updated;
                        updated.mutable_header()->CopyFrom( update.header() );
                        updated.set_status( kiapi::common::types::IRS_OK );
                        auto* item = updated.add_updated_items();
                        item->mutable_status()->set_code( kiapi::common::commands::ISC_OK );
                        item->mutable_item()->PackFrom( liveFootprint );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( updated );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;
                    request.message().UnpackTo( &end );

                    if( end.id().value() == commitId
                        && end.action() == kiapi::common::commands::CMA_COMMIT )
                    {
                        ++commitCount;
                    }

                    kiapi::common::commands::EndCommitResponse ended;
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( ended );
                }
                else if( request.message().Is<kiapi::common::commands::SaveDocument>() )
                {
                    ++saveCount;
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; },
                                  [&fixture]() { return fixture.Root(); } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project placed_design)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (board (place R1 (at 12.5mm 7.5mm) (rotation 37.5deg) (side back) (locked true))))
)KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "placed.kicad_kds" },
                                                { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    const std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", "placed.kicad_kds" },
                                                  { "boardPath", "design.kicad_pcb" },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    JSON data = envelope( applied )["data"];
    BOOST_CHECK_EQUAL( data["counts"]["placement"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( data["managedItems"].get<int>(), 0 );
    BOOST_CHECK( sawFootprintInventory );
    BOOST_CHECK( sawNarrowUpdate );
    BOOST_CHECK_EQUAL( commitCount, 1 );
    BOOST_CHECK_EQUAL( saveCount, 1 );
    BOOST_CHECK( data["boardSaved"].get<bool>() );
    BOOST_CHECK_EQUAL( data["verification"]["status"].get<std::string>(), "not_run" );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( RefillsManagedKdsZonesAndRecoversARejectedRefill )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName socketPath( fixture.Root(), wxS( "api-zone-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-zone-token";
    const std::string commitId = "aaaaaaaa-aaaa-8aaa-8aaa-aaaaaaaaaaaa";
    kiapi::board::types::Zone liveZone;
    bool rejectNextRefill = true;
    bool sawRefill = false;
    int commitCount = 0;

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::GetItemsById>() )
                {
                    kiapi::common::commands::GetItemsById get;
                    kiapi::common::commands::GetItemsResponse items;
                    request.message().UnpackTo( &get );

                    for( const auto& id : get.items() )
                    {
                        if( !liveZone.id().value().empty() && id.value() == liveZone.id().value() )
                            items.add_items()->PackFrom( liveZone );
                    }

                    items.set_status( kiapi::common::types::IRS_OK );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( items );
                }
                else if( request.message().Is<kiapi::common::commands::GetItems>() )
                {
                    kiapi::common::commands::GetItems get;
                    kiapi::common::commands::GetItemsResponse items;
                    request.message().UnpackTo( &get );

                    if( get.types_size() == 1
                        && get.types( 0 ) == kiapi::common::types::KOT_PCB_ZONE )
                    {
                        if( !liveZone.id().value().empty() )
                            items.add_items()->PackFrom( liveZone );

                        items.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( items );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    kiapi::common::commands::BeginCommitResponse begin;
                    begin.mutable_id()->set_value( commitId );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( begin );
                }
                else if( request.message().Is<kiapi::common::commands::CreateItems>() )
                {
                    kiapi::common::commands::CreateItems create;
                    kiapi::common::commands::CreateItemsResponse created;
                    request.message().UnpackTo( &create );
                    auto* result = created.add_created_items();

                    if( create.items_size() == 1 && create.items( 0 ).UnpackTo( &liveZone ) )
                    {
                        result->mutable_status()->set_code( kiapi::common::commands::ISC_OK );
                        result->mutable_item()->PackFrom( liveZone );
                    }
                    else
                    {
                        result->mutable_status()->set_code(
                                kiapi::common::commands::ISC_INVALID_DATA );
                    }

                    created.set_status( kiapi::common::types::IRS_OK );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( created );
                }
                else if( request.message().Is<kiapi::common::commands::UpdateItems>() )
                {
                    kiapi::common::commands::UpdateItems update;
                    kiapi::common::commands::UpdateItemsResponse updated;
                    request.message().UnpackTo( &update );
                    auto* result = updated.add_updated_items();

                    if( update.items_size() == 1 && update.items( 0 ).UnpackTo( &liveZone ) )
                    {
                        result->mutable_status()->set_code( kiapi::common::commands::ISC_OK );
                        result->mutable_item()->PackFrom( liveZone );
                    }
                    else
                    {
                        result->mutable_status()->set_code(
                                kiapi::common::commands::ISC_INVALID_DATA );
                    }

                    updated.set_status( kiapi::common::types::IRS_OK );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( updated );
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;
                    request.message().UnpackTo( &end );

                    if( end.action() == kiapi::common::commands::CMA_COMMIT )
                        ++commitCount;

                    kiapi::common::commands::EndCommitResponse ended;
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( ended );
                }
                else if( request.message().Is<kiapi::board::commands::RefillZones>() )
                {
                    if( rejectNextRefill )
                    {
                        rejectNextRefill = false;
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                        response.mutable_status()->set_error_message( "injected refill failure" );
                    }
                    else
                    {
                        sawRefill = true;
                        liveZone.set_filled( true );
                        google::protobuf::Empty empty;
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( empty );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::SaveDocument>() )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; },
                                  [&fixture]() { return fixture.Root(); } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project managed_zone)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net GND (pin R1 1 1) (pin R2 1 1))
  (board
    (zone GND
      (id ground-plane)
      (layers F.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 20mm 0mm) (point 20mm 10mm) (point 0mm 10mm)))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill solid))))
)KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "zone.kicad_kds" },
                                                { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    const std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON failed = registry.Handle( "design", { { "operation", "apply" },
                                                { "path", "zone.kicad_kds" },
                                                { "boardPath", "design.kicad_pcb" },
                                                { "expectedSha256", hash } } );
    BOOST_REQUIRE( !failed.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( failed )["error"]["code"].get<std::string>(),
                       "zone_refill_failed" );
    BOOST_CHECK_EQUAL( commitCount, 1 );
    BOOST_CHECK( !liveZone.id().value().empty() );
    BOOST_CHECK( !liveZone.filled() );
    BOOST_CHECK( wxFileExists( wxFileName( fixture.Root(),
                                           wxS( "zone.kicad_kds_journal" ) ).GetFullPath() ) );

    JSON recovered = registry.Handle( "design", { { "operation", "apply" },
                                                   { "path", "zone.kicad_kds" },
                                                   { "boardPath", "design.kicad_pcb" },
                                                   { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( recovered.at( "success" ).get<bool>(), recovered.dump() );
    JSON data = envelope( recovered )["data"];
    BOOST_CHECK_EQUAL( data["counts"]["update"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( data["zonesRefilled"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( commitCount, 2 );
    BOOST_CHECK( sawRefill );
    BOOST_CHECK( liveZone.filled() );
    BOOST_CHECK( !wxFileExists( wxFileName( fixture.Root(),
                                            wxS( "zone.kicad_kds_journal" ) ).GetFullPath() ) );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( AppliesReusableDesignAgainstLivePcbEditorWhenRequested )
{
    wxString projectPath;
    wxString boardPath;
    wxString sourcePath;

    if( !wxGetEnv( wxS( "KICHAD_QA_LIVE_PROJECT" ), &projectPath )
        || !wxGetEnv( wxS( "KICHAD_QA_LIVE_BOARD" ), &boardPath )
        || !wxGetEnv( wxS( "KICHAD_QA_LIVE_KDS" ), &sourcePath ) )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in live KDS apply test" );
        return;
    }

    wxFileName project = wxFileName::DirName( projectPath );
    project.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    wxFileName board( boardPath );
    board.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    wxFileName source( sourcePath );
    source.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    BOOST_REQUIRE( project.DirExists() );
    BOOST_REQUIRE( board.FileExists() );
    BOOST_REQUIRE( source.FileExists() );
    BOOST_REQUIRE_EQUAL( board.GetPath(), project.GetPath() );
    BOOST_REQUIRE_EQUAL( source.GetPath(), project.GetPath() );

    // This is a GUI-level smoke test, so migration dialogs are failures rather than harmless
    // parser compatibility.  Keep every native fixture at the exact format emitted by KiCad 10.
    BOOST_REQUIRE_NE( readExactTextFile( board ).find( "(version 20260206)" ),
                      std::string::npos );
    BOOST_REQUIRE_NE(
            readExactTextFile( wxFileName( project.GetFullPath(),
                                           wxS( "live_apply.kicad_sch" ) ) )
                    .find( "(version 20260306)" ),
            std::string::npos );
    BOOST_REQUIRE_NE(
            readExactTextFile( wxFileName( project.GetFullPath(), wxS( "Device.kicad_sym" ) ) )
                    .find( "(version 20251024)" ),
            std::string::npos );
    wxFileName footprintLibrary = wxFileName::DirName( project.GetFullPath() );
    footprintLibrary.AppendDir( wxS( "Resistor_SMD.pretty" ) );
    BOOST_REQUIRE_NE(
            readExactTextFile( wxFileName( footprintLibrary.GetFullPath(),
                                           wxS( "R_0603_1608Metric.kicad_mod" ) ) )
                    .find( "(version 20260206)" ),
            std::string::npos );

    wxString socketDirectory;
    wxGetEnv( wxS( "KICHAD_QA_LIVE_SOCKET_DIR" ), &socketDirectory );
    CODEX_TOOL_REGISTRY registry( [project]() { return project.GetFullPath(); },
                                  []() { return true; },
                                  [socketDirectory]() { return socketDirectory; },
                                  []( const wxFileName&, const JSON&, const JSON&,
                                      std::string& )
                                  {
                                      return true;
                                  } );
    const std::string sourceName = source.GetFullName().ToStdString();
    const std::string boardName = board.GetFullName().ToStdString();
    JSON nativeErc = registry.Handle(
            "verify", { { "operation", "erc" }, { "path", "live_apply.kicad_sch" } } );
    BOOST_REQUIRE_MESSAGE( nativeErc.at( "success" ).get<bool>(), nativeErc.dump() );
    JSON nativeErcData = envelope( nativeErc )["data"];
    BOOST_CHECK( nativeErcData["clean"].get<bool>() );
    BOOST_CHECK_EQUAL( nativeErcData["counts"]["total"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( nativeErcData["kicadVersion"].get<std::string>(),
                       std::string( GetMajorMinorPatchVersion().ToUTF8() ) );

    JSON nativeDrc = registry.Handle(
            "verify", { { "operation", "drc" }, { "path", boardName } } );
    BOOST_REQUIRE_MESSAGE( nativeDrc.at( "success" ).get<bool>(), nativeDrc.dump() );
    JSON nativeDrcData = envelope( nativeDrc )["data"];
    BOOST_CHECK( !nativeDrcData["clean"].get<bool>() );
    BOOST_CHECK_EQUAL( nativeDrcData["counts"]["total"].get<int>(), 5 );
    BOOST_CHECK_EQUAL( nativeDrcData["counts"]["categories"]["drc"].get<int>(), 4 );
    BOOST_CHECK_EQUAL(
            nativeDrcData["counts"]["categories"]["schematicParity"].get<int>(), 1 );

    JSON compiled = registry.Handle( "design", { { "operation", "compile" },
                                                  { "path", sourceName } } );
    BOOST_REQUIRE_MESSAGE( compiled.at( "success" ).get<bool>(), compiled.dump() );
    JSON compileData = envelope( compiled )["data"];
    BOOST_REQUIRE_MESSAGE( compileData["valid"].get<bool>(), compileData.dump( 2 ) );
    const std::string hash = compileData["sourceSha256"].get<std::string>();
    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                 { "path", sourceName },
                                                 { "boardPath", boardName },
                                                 { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    JSON firstData = envelope( applied )["data"];
    BOOST_CHECK_EQUAL( firstData["managedItems"].get<int>(), 13 );
    BOOST_CHECK_EQUAL( firstData["counts"]["placement"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( firstData["counts"]["footprintCreate"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["zonesRefilled"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["transaction"].get<std::string>(), "committed" );
    BOOST_CHECK( firstData["titleBlockApplied"].get<bool>() );
    BOOST_CHECK( firstData["stackupApplied"].get<bool>() );
    BOOST_CHECK( firstData["rulesApplied"].get<bool>() );
    BOOST_CHECK( firstData["netClassesApplied"].get<bool>() );
    BOOST_CHECK( firstData["textVariablesApplied"].get<bool>() );
    BOOST_CHECK( firstData["fieldTemplatesApplied"].get<bool>() );
    BOOST_CHECK( firstData["customRulesApplied"].get<bool>() );
    BOOST_CHECK_EQUAL( firstData["libraryTablesApplied"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( firstData["schematicFilesApplied"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( firstData["schematicCounts"]["filesCreated"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["schematicCounts"]["filesUpdated"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["schematicCounts"]["itemsUpserted"].get<int>(), 40 );
    const wxFileName projectFile( project.GetFullPath(), wxS( "live_apply.kicad_pro" ) );
    const JSON projectSettings = JSON::parse( readExactTextFile( projectFile ) );
    BOOST_CHECK_EQUAL( projectSettings["text_variables"]["PRODUCT_NAME"],
                       "KiChad Live Apply" );
    BOOST_CHECK_EQUAL( projectSettings["text_variables"]["DESIGN_OWNER"], "Codex" );
    BOOST_REQUIRE_EQUAL(
            projectSettings["schematic"]["drawing"]["field_names"].size(), 2 );
    BOOST_CHECK_EQUAL(
            projectSettings["schematic"]["drawing"]["field_names"][0]["name"],
            "Manufacturer Part" );
    BOOST_CHECK(
            projectSettings["schematic"]["drawing"]["field_names"][0]["visible"]
                    .get<bool>() );
    BOOST_CHECK_EQUAL(
            projectSettings["schematic"]["drawing"]["field_names"][1]["name"],
            "Compliance URL" );
    BOOST_CHECK(
            projectSettings["schematic"]["drawing"]["field_names"][1]["url"]
                    .get<bool>() );

    KICHAD_IPC_CLIENT projectSettingsClient(
            "org.kichad.codex.qa.project-settings-validation", socketDirectory );
    KICHAD_IPC_TARGET projectSettingsTarget;
    std::string projectSettingsError;
    BOOST_REQUIRE_MESSAGE(
            projectSettingsClient.FindOpenPcb( project.GetFullPath(), board.GetFullPath(),
                                               projectSettingsTarget,
                                               projectSettingsError ),
            projectSettingsError );

    kiapi::common::commands::SetTextVariables invalidTextVariables;
    invalidTextVariables.mutable_document()->set_type(
            kiapi::common::types::DOCTYPE_PROJECT );
    invalidTextVariables.mutable_document()->mutable_project()->CopyFrom(
            projectSettingsTarget.document.project() );
    ( *invalidTextVariables.mutable_variables()->mutable_variables() )["BAD.NAME"] =
            "must be rejected";
    invalidTextVariables.set_merge_mode( kiapi::common::types::MMM_REPLACE );
    kiapi::common::ApiResponse invalidTextVariablesResponse;
    BOOST_CHECK( !projectSettingsClient.Call(
            projectSettingsTarget, invalidTextVariables, invalidTextVariablesResponse,
            projectSettingsError ) );
    BOOST_CHECK( !projectSettingsError.empty() );

    kiapi::common::project::TextVariables unchangedTextVariables;
    projectSettingsError.clear();
    BOOST_REQUIRE_MESSAGE(
            KICHAD::PROJECT_SETTINGS_IPC::QueryTextVariables(
                    projectSettingsClient, projectSettingsTarget, unchangedTextVariables,
                    projectSettingsError ),
            projectSettingsError );
    BOOST_REQUIRE_EQUAL( unchangedTextVariables.variables_size(), 2 );
    BOOST_CHECK_EQUAL( unchangedTextVariables.variables().at( "PRODUCT_NAME" ),
                       "KiChad Live Apply" );

    kiapi::common::commands::SetSchematicFieldTemplates invalidFieldTemplates;
    invalidFieldTemplates.mutable_document()->set_type(
            kiapi::common::types::DOCTYPE_PROJECT );
    invalidFieldTemplates.mutable_document()->mutable_project()->CopyFrom(
            projectSettingsTarget.document.project() );
    auto* duplicateTemplate =
            invalidFieldTemplates.mutable_templates()->add_fields();
    duplicateTemplate->set_name( "MPN" );
    duplicateTemplate->set_visible( true );
    duplicateTemplate = invalidFieldTemplates.mutable_templates()->add_fields();
    duplicateTemplate->set_name( "mpn" );
    duplicateTemplate->set_url( true );
    kiapi::common::ApiResponse invalidFieldTemplatesResponse;
    projectSettingsError.clear();
    BOOST_CHECK( !projectSettingsClient.Call(
            projectSettingsTarget, invalidFieldTemplates, invalidFieldTemplatesResponse,
            projectSettingsError ) );
    BOOST_CHECK( !projectSettingsError.empty() );

    kiapi::common::project::SchematicFieldTemplates unchangedFieldTemplates;
    projectSettingsError.clear();
    BOOST_REQUIRE_MESSAGE(
            KICHAD::PROJECT_SETTINGS_IPC::QuerySchematicFieldTemplates(
                    projectSettingsClient, projectSettingsTarget, unchangedFieldTemplates,
                    projectSettingsError ),
            projectSettingsError );
    BOOST_REQUIRE_EQUAL( unchangedFieldTemplates.fields_size(), 2 );
    BOOST_CHECK_EQUAL( unchangedFieldTemplates.fields( 0 ).name(),
                       "Manufacturer Part" );
    BOOST_CHECK_EQUAL( unchangedFieldTemplates.fields( 1 ).name(),
                       "Compliance URL" );

    const std::string projectBeforeRejectedSave = readExactTextFile( projectFile );
    const std::filesystem::path projectSettingsPath(
            projectFile.GetFullPath().ToStdString() );
    std::error_code permissionError;
    const std::filesystem::perms originalPermissions =
            std::filesystem::status( projectSettingsPath, permissionError ).permissions();
    BOOST_REQUIRE( !permissionError );
    std::filesystem::permissions(
            projectSettingsPath,
            std::filesystem::perms::owner_read | std::filesystem::perms::group_read
                    | std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace, permissionError );
    BOOST_REQUIRE( !permissionError );

    kiapi::common::project::TextVariables rejectedSaveVariables =
            unchangedTextVariables;
    ( *rejectedSaveVariables.mutable_variables() )["PRODUCT_NAME"] =
            "must not survive a rejected save";
    projectSettingsError.clear();
    const bool rejectedSaveAccepted =
            KICHAD::PROJECT_SETTINGS_IPC::ReplaceTextVariables(
                    projectSettingsClient, projectSettingsTarget,
                    rejectedSaveVariables, projectSettingsError );
    std::error_code restorePermissionError;
    std::filesystem::permissions(
            projectSettingsPath, originalPermissions,
            std::filesystem::perm_options::replace, restorePermissionError );
    BOOST_REQUIRE( !restorePermissionError );
    BOOST_CHECK( !rejectedSaveAccepted );
    BOOST_CHECK( !projectSettingsError.empty() );
    BOOST_CHECK_EQUAL( readExactTextFile( projectFile ),
                       projectBeforeRejectedSave );

    kiapi::common::project::TextVariables rolledBackAfterSaveFailure;
    projectSettingsError.clear();
    BOOST_REQUIRE_MESSAGE(
            KICHAD::PROJECT_SETTINGS_IPC::QueryTextVariables(
                    projectSettingsClient, projectSettingsTarget,
                    rolledBackAfterSaveFailure, projectSettingsError ),
            projectSettingsError );
    BOOST_CHECK_EQUAL(
            rolledBackAfterSaveFailure.variables().at( "PRODUCT_NAME" ),
            "KiChad Live Apply" );

    kiapi::common::project::TextVariables noTextVariables;
    projectSettingsError.clear();
    BOOST_REQUIRE_MESSAGE(
            KICHAD::PROJECT_SETTINGS_IPC::ReplaceTextVariables(
                    projectSettingsClient, projectSettingsTarget, noTextVariables,
                    projectSettingsError ),
            projectSettingsError );
    kiapi::common::project::TextVariables clearedTextVariables;
    BOOST_REQUIRE_MESSAGE(
            KICHAD::PROJECT_SETTINGS_IPC::QueryTextVariables(
                    projectSettingsClient, projectSettingsTarget, clearedTextVariables,
                    projectSettingsError ),
            projectSettingsError );
    BOOST_CHECK( clearedTextVariables.variables().empty() );

    kiapi::common::project::SchematicFieldTemplates noFieldTemplates;
    projectSettingsError.clear();
    BOOST_REQUIRE_MESSAGE(
            KICHAD::PROJECT_SETTINGS_IPC::ReplaceSchematicFieldTemplates(
                    projectSettingsClient, projectSettingsTarget, noFieldTemplates,
                    projectSettingsError ),
            projectSettingsError );
    kiapi::common::project::SchematicFieldTemplates clearedFieldTemplates;
    BOOST_REQUIRE_MESSAGE(
            KICHAD::PROJECT_SETTINGS_IPC::QuerySchematicFieldTemplates(
                    projectSettingsClient, projectSettingsTarget, clearedFieldTemplates,
                    projectSettingsError ),
            projectSettingsError );
    BOOST_CHECK( clearedFieldTemplates.fields().empty() );
    BOOST_CHECK( wxFileExists( wxFileName( project.GetFullPath(),
                                           wxS( "power.kicad_sch" ) ).GetFullPath() ) );
    BOOST_CHECK_EQUAL( readExactTextFile( wxFileName( project.GetFullPath(),
                                                      wxS( "sym-lib-table" ) ) ),
                       "(sym_lib_table\n"
                       "  (version 7)\n"
                       "  (lib (name \"Device\")(type \"KiCad\")"
                       "(uri \"${KIPRJMOD}/Device.kicad_sym\")(options \"\")"
                       "(descr \"\"))\n"
                       ")\n" );
    BOOST_CHECK_EQUAL( readExactTextFile( wxFileName( project.GetFullPath(),
                                                      wxS( "fp-lib-table" ) ) ),
                       "(fp_lib_table\n"
                       "  (version 7)\n"
                       "  (lib (name \"Resistor_SMD\")(type \"KiCad\")"
                       "(uri \"${KIPRJMOD}/Resistor_SMD.pretty\")(options \"\")"
                       "(descr \"\"))\n"
                       ")\n" );

    JSON repeated = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", sourceName },
                                                  { "boardPath", boardName },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( repeated.at( "success" ).get<bool>(), repeated.dump() );
    JSON repeatedData = envelope( repeated )["data"];
    BOOST_CHECK_EQUAL( repeatedData["managedItems"].get<int>(), 13 );
    BOOST_CHECK_EQUAL( repeatedData["counts"]["create"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( repeatedData["counts"]["update"].get<int>(), 12 );
    BOOST_CHECK_EQUAL( repeatedData["counts"]["delete"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( repeatedData["counts"]["placement"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( repeatedData["counts"]["footprintCreate"].get<int>(), 0 );
    BOOST_CHECK( repeatedData["stackupApplied"].get<bool>() );
    BOOST_CHECK( repeatedData["titleBlockApplied"].get<bool>() );
    BOOST_CHECK( repeatedData["rulesApplied"].get<bool>() );
    BOOST_CHECK( repeatedData["netClassesApplied"].get<bool>() );
    BOOST_CHECK( repeatedData["textVariablesApplied"].get<bool>() );
    BOOST_CHECK( repeatedData["fieldTemplatesApplied"].get<bool>() );
    BOOST_CHECK( repeatedData["customRulesApplied"].get<bool>() );
    BOOST_CHECK_EQUAL( repeatedData["libraryTablesApplied"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( repeatedData["schematicFilesApplied"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( repeatedData["schematicCounts"]["filesUnchanged"].get<int>(), 2 );

    const wxFileName rootSchematic( project.GetFullPath(), wxS( "live_apply.kicad_sch" ) );
    const std::string rootBeforeRejectedApply = readExactTextFile( rootSchematic );
    const std::string projectBeforeRejectedApply = readExactTextFile( projectFile );
    const std::string originalSource = readExactTextFile( source );
    std::string rejectedSource = originalSource;
    const std::string originalTitle = "KiChad AI-native release";
    const size_t titlePosition = rejectedSource.find( originalTitle );
    BOOST_REQUIRE_NE( titlePosition, std::string::npos );
    rejectedSource.replace( titlePosition, originalTitle.size(), "Rejected native title" );
    const std::string originalTextVariable = "KiChad Live Apply";
    const size_t textVariablePosition = rejectedSource.find( originalTextVariable );
    BOOST_REQUIRE_NE( textVariablePosition, std::string::npos );
    rejectedSource.replace( textVariablePosition, originalTextVariable.size(),
                            "Rejected Project Variable" );
    const std::string originalTemplate = "Compliance URL";
    const size_t templatePosition = rejectedSource.find( originalTemplate );
    BOOST_REQUIRE_NE( templatePosition, std::string::npos );
    rejectedSource.replace( templatePosition, originalTemplate.size(),
                            "Rejected URL Template" );
    JSON rejectedSaved = registry.Handle(
            "design", { { "operation", "save" },
                         { "path", sourceName },
                         { "source", rejectedSource },
                         { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( rejectedSaved.at( "success" ).get<bool>(), rejectedSaved.dump() );
    const std::string rejectedHash =
            envelope( rejectedSaved )["data"]["sourceSha256"].get<std::string>();
    CODEX_TOOL_REGISTRY rejectingRegistry(
            [project]() { return project.GetFullPath(); }, []() { return true; },
            [socketDirectory]() { return socketDirectory; },
            []( const wxFileName&, const JSON&, const JSON&, std::string& aError )
            {
                aError = "injected native schematic rejection";
                return false;
            } );
    JSON rejectedApply = rejectingRegistry.Handle(
            "design", { { "operation", "apply" },
                         { "path", sourceName },
                         { "boardPath", boardName },
                         { "expectedSha256", rejectedHash } } );
    BOOST_CHECK( !rejectedApply.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( rejectedApply )["error"]["code"].get<std::string>(),
                       "schematic_validation_failed" );
    BOOST_CHECK_EQUAL( readExactTextFile( rootSchematic ), rootBeforeRejectedApply );
    BOOST_CHECK_EQUAL( readExactTextFile( projectFile ), projectBeforeRejectedApply );
    KICHAD_IPC_CLIENT rollbackClient( "org.kichad.codex.qa.title-rollback",
                                      socketDirectory );
    KICHAD_IPC_TARGET rollbackTarget;
    std::string rollbackIpcError;
    BOOST_REQUIRE_MESSAGE(
            rollbackClient.FindOpenPcb( project.GetFullPath(), board.GetFullPath(),
                                        rollbackTarget, rollbackIpcError ),
            rollbackIpcError );
    kiapi::common::commands::GetTitleBlockInfo rollbackTitleRequest;
    rollbackTitleRequest.mutable_document()->CopyFrom( rollbackTarget.document );
    kiapi::common::ApiResponse rollbackTitleEnvelope;
    BOOST_REQUIRE_MESSAGE(
            rollbackClient.Call( rollbackTarget, rollbackTitleRequest,
                                 rollbackTitleEnvelope, rollbackIpcError ),
            rollbackIpcError );
    kiapi::common::types::TitleBlockInfo rolledBackTitleBlock;
    BOOST_REQUIRE( rollbackTitleEnvelope.message().UnpackTo( &rolledBackTitleBlock ) );
    BOOST_CHECK_EQUAL( rolledBackTitleBlock.title(), "KiChad AI-native release" );
    kiapi::common::project::TextVariables rolledBackTextVariables;
    BOOST_REQUIRE_MESSAGE(
            KICHAD::PROJECT_SETTINGS_IPC::QueryTextVariables(
                    rollbackClient, rollbackTarget, rolledBackTextVariables, rollbackIpcError ),
            rollbackIpcError );
    BOOST_CHECK_EQUAL( rolledBackTextVariables.variables().at( "PRODUCT_NAME" ),
                       "KiChad Live Apply" );
    kiapi::common::project::SchematicFieldTemplates rolledBackFieldTemplates;
    BOOST_REQUIRE_MESSAGE(
            KICHAD::PROJECT_SETTINGS_IPC::QuerySchematicFieldTemplates(
                    rollbackClient, rollbackTarget, rolledBackFieldTemplates,
                    rollbackIpcError ),
            rollbackIpcError );
    BOOST_REQUIRE_EQUAL( rolledBackFieldTemplates.fields_size(), 2 );
    BOOST_CHECK_EQUAL( rolledBackFieldTemplates.fields( 1 ).name(), "Compliance URL" );
    BOOST_CHECK( rolledBackFieldTemplates.fields( 1 ).url() );

    JSON restoredSaved = registry.Handle(
            "design", { { "operation", "save" },
                         { "path", sourceName },
                         { "source", originalSource },
                         { "expectedSha256", rejectedHash } } );
    BOOST_REQUIRE_MESSAGE( restoredSaved.at( "success" ).get<bool>(), restoredSaved.dump() );
    const std::string restoredHash =
            envelope( restoredSaved )["data"]["sourceSha256"].get<std::string>();
    JSON recoveredApply = registry.Handle(
            "design", { { "operation", "apply" },
                         { "path", sourceName },
                         { "boardPath", boardName },
                         { "expectedSha256", restoredHash } } );
    BOOST_REQUIRE_MESSAGE( recoveredApply.at( "success" ).get<bool>(),
                           recoveredApply.dump() );
    BOOST_CHECK_EQUAL( readExactTextFile( rootSchematic ), rootBeforeRejectedApply );

    KICHAD_IPC_CLIENT ipcClient( "org.kichad.codex.qa.stackup", socketDirectory );
    KICHAD_IPC_TARGET ipcTarget;
    std::string       ipcError;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.FindOpenPcb( project.GetFullPath(), board.GetFullPath(),
                                   ipcTarget, ipcError ),
            ipcError );
    kiapi::common::commands::GetTitleBlockInfo titleRequest;
    titleRequest.mutable_document()->CopyFrom( ipcTarget.document );
    kiapi::common::ApiResponse titleEnvelope;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.Call( ipcTarget, titleRequest, titleEnvelope, ipcError ), ipcError );
    kiapi::common::types::TitleBlockInfo titleBlock;
    BOOST_REQUIRE( titleEnvelope.message().UnpackTo( &titleBlock ) );
    BOOST_CHECK_EQUAL( titleBlock.title(), "KiChad AI-native release" );
    BOOST_CHECK_EQUAL( titleBlock.date(), "2026-07-19" );
    BOOST_CHECK_EQUAL( titleBlock.revision(), "KDS-1" );
    BOOST_CHECK_EQUAL( titleBlock.company(), "KiChad QA" );
    BOOST_CHECK_EQUAL( titleBlock.comment1(), "Controlled by the KDS sidecar" );
    BOOST_CHECK_EQUAL( titleBlock.comment2(), "" );
    BOOST_CHECK_EQUAL( titleBlock.comment9(), "Complete indexed title-block proof" );
    kiapi::board::commands::GetBoardStackup stackupRequest;
    stackupRequest.mutable_board()->CopyFrom( ipcTarget.document );
    kiapi::common::ApiResponse stackupEnvelope;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.Call( ipcTarget, stackupRequest, stackupEnvelope, ipcError ), ipcError );
    kiapi::board::commands::BoardStackupResponse stackupResponse;
    BOOST_REQUIRE( stackupEnvelope.message().UnpackTo( &stackupResponse ) );
    const kiapi::board::BoardStackup& stackup = stackupResponse.stackup();
    BOOST_REQUIRE_EQUAL( stackup.layers_size(), 9 );
    BOOST_CHECK_EQUAL( stackup.finish().type_name(), "ENIG" );
    BOOST_CHECK( stackup.impedance().is_controlled() );
    BOOST_CHECK( stackup.edge().connector().bevelled() );
    BOOST_CHECK( stackup.edge().plating().has_edge_plating() );
    BOOST_CHECK_EQUAL( stackup.layers( 0 ).color_name(), "White" );
    BOOST_CHECK_EQUAL( stackup.layers( 3 ).thickness().value_nm(), 35000 );
    BOOST_CHECK_EQUAL( stackup.layers( 4 ).dielectric_layer_id(), 1 );
    BOOST_CHECK_EQUAL(
            stackup.layers( 4 ).dielectric().layer( 0 ).thickness().value_nm(), 1510000 );
    BOOST_CHECK( stackup.layers( 4 ).dielectric().layer( 0 ).thickness_locked() );
    BOOST_CHECK_EQUAL( stackup.layers( 6 ).material_name(), "LPI" );

    kiapi::board::commands::GetBoardDesignRules rulesRequest;
    rulesRequest.mutable_board()->CopyFrom( ipcTarget.document );
    kiapi::common::ApiResponse rulesEnvelope;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.Call( ipcTarget, rulesRequest, rulesEnvelope, ipcError ), ipcError );
    kiapi::board::commands::BoardDesignRulesResponse rulesResponse;
    BOOST_REQUIRE( rulesEnvelope.message().UnpackTo( &rulesResponse ) );
    const kiapi::board::BoardDesignRules& rules = rulesResponse.rules();
    BOOST_CHECK_EQUAL( rules.minimum_clearance().value_nm(), 200000 );
    BOOST_CHECK_EQUAL( rules.minimum_connection_width().value_nm(), 150000 );
    BOOST_CHECK_EQUAL( rules.minimum_track_width().value_nm(), 180000 );
    BOOST_CHECK_EQUAL( rules.minimum_via_annular_width().value_nm(), 100000 );
    BOOST_CHECK_EQUAL( rules.minimum_via_diameter().value_nm(), 600000 );
    BOOST_CHECK_EQUAL( rules.minimum_silkscreen_clearance().value_nm(), -10000 );
    BOOST_CHECK_EQUAL( rules.minimum_resolved_spokes(), 3 );
    BOOST_CHECK_EQUAL( rules.copper_edge_clearance_mode(), kiapi::board::BCECM_LEGACY );
    BOOST_CHECK_EQUAL( rules.minimum_copper_to_edge_clearance().value_nm(), 0 );
    BOOST_CHECK( rules.use_height_for_length_calculations() );
    BOOST_CHECK_EQUAL( rules.maximum_error().value_nm(), 5000 );
    BOOST_CHECK( !rules.allow_fillets_outside_zone_outline() );

    kiapi::board::commands::UpdateBoardDesignRules invalidRulesRequest;
    invalidRulesRequest.mutable_board()->CopyFrom( ipcTarget.document );
    invalidRulesRequest.mutable_rules()->CopyFrom( rules );
    invalidRulesRequest.mutable_rules()->mutable_minimum_via_diameter()->set_value_nm( 400000 );
    kiapi::common::ApiResponse invalidRulesEnvelope;
    BOOST_CHECK( !ipcClient.Call( ipcTarget, invalidRulesRequest, invalidRulesEnvelope, ipcError ) );
    BOOST_CHECK_NE( ipcError.find( "cannot satisfy" ), std::string::npos );
    ipcError.clear();
    kiapi::common::ApiResponse unchangedRulesEnvelope;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.Call( ipcTarget, rulesRequest, unchangedRulesEnvelope, ipcError ), ipcError );
    kiapi::board::commands::BoardDesignRulesResponse unchangedRulesResponse;
    BOOST_REQUIRE( unchangedRulesEnvelope.message().UnpackTo( &unchangedRulesResponse ) );
    BOOST_CHECK_EQUAL( unchangedRulesResponse.rules().minimum_via_diameter().value_nm(), 600000 );

    kiapi::common::commands::GetNetClassSettings netClassesRequest;
    kiapi::common::ApiResponse netClassesEnvelope;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.Call( ipcTarget, netClassesRequest, netClassesEnvelope, ipcError ),
            ipcError );
    kiapi::common::commands::NetClassSettingsResponse netClassesResponse;
    BOOST_REQUIRE( netClassesEnvelope.message().UnpackTo( &netClassesResponse ) );
    const kiapi::common::project::NetClassSettings& netClasses =
            netClassesResponse.settings();
    BOOST_REQUIRE_EQUAL( netClasses.net_classes_size(), 2 );
    BOOST_REQUIRE_EQUAL( netClasses.assignments_size(), 1 );
    const kiapi::common::project::NetClass& defaultClass = netClasses.net_classes( 0 );
    BOOST_CHECK_EQUAL( defaultClass.name(), "Default" );
    BOOST_CHECK_EQUAL( defaultClass.priority(), std::numeric_limits<int32_t>::max() );
    BOOST_CHECK_EQUAL( defaultClass.type(), kiapi::common::project::NCT_EXPLICIT );
    BOOST_CHECK_EQUAL( defaultClass.constituents_size(), 0 );
    BOOST_CHECK_EQUAL( defaultClass.board().clearance().value_nm(), 200000 );
    BOOST_CHECK_EQUAL( defaultClass.board().track_width().value_nm(), 200000 );
    BOOST_CHECK_EQUAL(
            defaultClass.board().via_stack().copper_layers( 0 ).size().x_nm(), 600000 );
    BOOST_CHECK_EQUAL(
            defaultClass.board().via_stack().drill().diameter().x_nm(), 300000 );
    BOOST_CHECK_EQUAL(
            defaultClass.board().microvia_stack().copper_layers( 0 ).size().x_nm(), 300000 );
    BOOST_CHECK_EQUAL(
            defaultClass.board().microvia_stack().drill().diameter().x_nm(), 100000 );
    BOOST_CHECK_EQUAL( defaultClass.schematic().wire_width().value_nm(), 150000 );
    BOOST_CHECK_EQUAL( defaultClass.schematic().bus_width().value_nm(), 300000 );
    BOOST_CHECK_EQUAL( defaultClass.schematic().line_style(),
                       kiapi::common::types::SLS_SOLID );
    const kiapi::common::project::NetClass& signalClass = netClasses.net_classes( 1 );
    BOOST_CHECK_EQUAL( signalClass.name(), "SIGNAL" );
    BOOST_CHECK_EQUAL( signalClass.priority(), 0 );
    BOOST_CHECK_EQUAL( signalClass.board().clearance().value_nm(), 250000 );
    BOOST_CHECK_EQUAL( signalClass.board().track_width().value_nm(), 250000 );
    BOOST_CHECK( !signalClass.board().has_via_stack() );
    BOOST_CHECK_EQUAL( signalClass.board().tuning_profile(), "signal_quality" );
    BOOST_CHECK_CLOSE( signalClass.board().color().r(), 26.0 / 255.0, 0.0001 );
    BOOST_CHECK_CLOSE( signalClass.board().color().a(), 221.0 / 255.0, 0.0001 );
    BOOST_CHECK( !signalClass.schematic().has_wire_width() );
    BOOST_CHECK_EQUAL( signalClass.schematic().line_style(),
                       kiapi::common::types::SLS_DASHDOT );
    BOOST_CHECK_EQUAL( netClasses.assignments( 0 ).pattern(), "Net1" );
    BOOST_CHECK_EQUAL( netClasses.assignments( 0 ).net_class(), "SIGNAL" );

    kiapi::common::commands::UpdateNetClassSettings invalidNetClassesRequest;
    invalidNetClassesRequest.mutable_settings()->CopyFrom( netClasses );
    auto* invalidVia = invalidNetClassesRequest.mutable_settings()
                               ->mutable_net_classes( 0 )
                               ->mutable_board()
                               ->mutable_via_stack()
                               ->mutable_copper_layers( 0 )
                               ->mutable_size();
    invalidVia->set_x_nm( 200000 );
    invalidVia->set_y_nm( 200000 );
    kiapi::common::ApiResponse invalidNetClassesEnvelope;
    BOOST_CHECK( !ipcClient.Call( ipcTarget, invalidNetClassesRequest,
                                  invalidNetClassesEnvelope, ipcError ) );
    BOOST_CHECK_NE( ipcError.find( "cannot be smaller" ), std::string::npos );
    ipcError.clear();
    kiapi::common::ApiResponse unchangedNetClassesEnvelope;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.Call( ipcTarget, netClassesRequest, unchangedNetClassesEnvelope,
                            ipcError ),
            ipcError );
    kiapi::common::commands::NetClassSettingsResponse unchangedNetClassesResponse;
    BOOST_REQUIRE( unchangedNetClassesEnvelope.message().UnpackTo(
            &unchangedNetClassesResponse ) );
    BOOST_CHECK_EQUAL(
            unchangedNetClassesResponse.settings()
                    .net_classes( 0 )
                    .board()
                    .via_stack()
                    .copper_layers( 0 )
                    .size()
                    .x_nm(),
            600000 );

    const std::string expectedCustomRules = R"DRU((version 1)

(rule "signal_policy"
  (condition "A.NetName == 'Net1'")
  (layer F.Cu)
  (severity error)
    (constraint clearance (min 0.25mm))
    (constraint track_width (min 0.2mm) (opt 0.3mm) (max 0.6mm))
)

(rule "dangling_vias"
  (severity warning)
    (constraint via_dangling)
)
)DRU";
    kiapi::board::commands::GetBoardCustomRules customRulesRequest;
    customRulesRequest.mutable_board()->CopyFrom( ipcTarget.document );
    kiapi::common::ApiResponse customRulesEnvelope;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.Call( ipcTarget, customRulesRequest, customRulesEnvelope, ipcError ),
            ipcError );
    kiapi::board::commands::BoardCustomRulesResponse customRulesResponse;
    BOOST_REQUIRE( customRulesEnvelope.message().UnpackTo( &customRulesResponse ) );
    BOOST_CHECK( customRulesResponse.present() );
    BOOST_CHECK_EQUAL( customRulesResponse.source(), expectedCustomRules );

    kiapi::board::commands::UpdateBoardCustomRules invalidCustomRulesRequest;
    invalidCustomRulesRequest.mutable_board()->CopyFrom( ipcTarget.document );
    invalidCustomRulesRequest.set_present( true );
    invalidCustomRulesRequest.set_source(
            "(version 1)\n(rule \"invalid\" (constraint track_width (min 100nm)))\n" );
    kiapi::common::ApiResponse invalidCustomRulesEnvelope;
    BOOST_CHECK( !ipcClient.Call( ipcTarget, invalidCustomRulesRequest,
                                  invalidCustomRulesEnvelope, ipcError ) );
    BOOST_CHECK_NE( ipcError.find( "Syntax error" ), std::string::npos );
    ipcError.clear();
    kiapi::common::ApiResponse unchangedCustomRulesEnvelope;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.Call( ipcTarget, customRulesRequest, unchangedCustomRulesEnvelope,
                            ipcError ),
            ipcError );
    kiapi::board::commands::BoardCustomRulesResponse unchangedCustomRulesResponse;
    BOOST_REQUIRE( unchangedCustomRulesEnvelope.message().UnpackTo(
            &unchangedCustomRulesResponse ) );
    BOOST_CHECK( unchangedCustomRulesResponse.present() );
    BOOST_CHECK_EQUAL( unchangedCustomRulesResponse.source(), expectedCustomRules );

    auto getItems = [&]( const std::string& aItemType, size_t aExpectedCount )
    {
        JSON response = registry.Handle( "pcb", { { "operation", "get" },
                                                   { "path", boardName },
                                                   { "itemType", aItemType },
                                                   { "limit", 10 } } );
        BOOST_REQUIRE_MESSAGE( response.at( "success" ).get<bool>(), response.dump() );
        JSON data = envelope( response )["data"];
        BOOST_REQUIRE_EQUAL( data["totalItems"].get<size_t>(), aExpectedCount );
        BOOST_REQUIRE_EQUAL( data["items"].size(), aExpectedCount );
        return data["items"];
    };

    auto getOne = [&]( const std::string& aItemType )
    {
        return getItems( aItemType, 1 )[0];
    };

    const auto stableUuid = []( const std::string& aType, const std::string& aLogicalId )
    {
        return KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                "live_apply", aType, aLogicalId );
    };

    JSON shape = getOne( "shape" );
    BOOST_CHECK_EQUAL( shape["id"]["value"].get<std::string>(),
                       stableUuid( "shape", "board-edge" ) );
    BOOST_CHECK_EQUAL( shape["layer"].get<std::string>(), "BL_Edge_Cuts" );
    BOOST_CHECK_EQUAL( shape["shape"]["rectangle"]["topLeft"]["xNm"].get<std::string>(),
                       "20000000" );
    BOOST_CHECK_EQUAL( shape["shape"]["rectangle"]["bottomRight"]["yNm"].get<std::string>(),
                       "50000000" );

    JSON trace = getOne( "trace" );
    BOOST_CHECK_EQUAL( trace["id"]["value"].get<std::string>(),
                       stableUuid( "trace", "first-trace" ) );
    BOOST_CHECK_EQUAL( trace["start"]["xNm"].get<std::string>(), "25000000" );
    BOOST_CHECK_EQUAL( trace["end"]["xNm"].get<std::string>(), "35000000" );
    BOOST_CHECK_EQUAL( trace["width"]["valueNm"].get<std::string>(), "250000" );
    BOOST_CHECK_EQUAL( trace["net"]["name"].get<std::string>(), "Net1" );

    JSON arc = getOne( "arc" );
    BOOST_CHECK_EQUAL( arc["id"]["value"].get<std::string>(),
                       stableUuid( "arc", "first-arc" ) );
    BOOST_CHECK_EQUAL( arc["mid"]["xNm"].get<std::string>(), "40000000" );
    BOOST_CHECK_EQUAL( arc["mid"]["yNm"].get<std::string>(), "30000000" );

    JSON via = getOne( "via" );
    BOOST_CHECK_EQUAL( via["id"]["value"].get<std::string>(),
                       stableUuid( "via", "first-via" ) );
    BOOST_CHECK_EQUAL( via["position"]["xNm"].get<std::string>(), "45000000" );
    BOOST_CHECK_EQUAL( via["padStack"]["drill"]["diameter"]["xNm"].get<std::string>(),
                       "400000" );
    BOOST_CHECK_EQUAL( via["net"]["name"].get<std::string>(), "Net1" );

    JSON zone = getOne( "zone" );
    BOOST_CHECK_EQUAL( zone["id"]["value"].get<std::string>(),
                       stableUuid( "zone", "ground-plane" ) );
    BOOST_CHECK_EQUAL( zone["type"].get<std::string>(), "ZT_COPPER" );
    BOOST_REQUIRE_EQUAL( zone["layers"].size(), 1 );
    BOOST_CHECK_EQUAL( zone["layers"][0].get<std::string>(), "BL_F_Cu" );
    BOOST_CHECK( zone["filled"].get<bool>() );
    BOOST_CHECK_EQUAL( zone["name"].get<std::string>(), "Net1 live fill proof" );
    BOOST_CHECK_EQUAL( zone["copperSettings"]["net"]["name"].get<std::string>(), "Net1" );
    BOOST_CHECK_EQUAL(
            zone["copperSettings"]["connection"]["thermalSpokes"]["width"]["valueNm"]
                    .get<std::string>(),
            "350000" );
    BOOST_CHECK_EQUAL( zone["copperSettings"]["minIslandArea"].get<std::string>(),
                       "1000000000000" );
    BOOST_REQUIRE_EQUAL( zone["outline"]["polygons"].size(), 1 );
    BOOST_REQUIRE_EQUAL( zone["outline"]["polygons"][0]["outline"]["nodes"].size(), 4 );

    JSON keepout = getOne( "rule_area" );
    BOOST_CHECK_EQUAL( keepout["id"]["value"].get<std::string>(),
                       stableUuid( "rule_area", "no-routing" ) );
    BOOST_CHECK_EQUAL( keepout["type"].get<std::string>(), "ZT_RULE_AREA" );
    BOOST_REQUIRE_EQUAL( keepout["layers"].size(), 1 );
    BOOST_CHECK_EQUAL( keepout["layers"][0].get<std::string>(), "BL_B_Cu" );
    BOOST_CHECK( keepout["ruleAreaSettings"]["keepoutCopper"].get<bool>() );
    BOOST_CHECK( keepout["ruleAreaSettings"]["keepoutVias"].get<bool>() );
    BOOST_CHECK( keepout["ruleAreaSettings"]["keepoutTracks"].get<bool>() );
    BOOST_CHECK( !keepout["ruleAreaSettings"].value( "keepoutPads", false ) );
    BOOST_CHECK( keepout["ruleAreaSettings"]["keepoutFootprints"].get<bool>() );
    BOOST_CHECK( !keepout["ruleAreaSettings"].value( "placementEnabled", false ) );
    BOOST_CHECK_EQUAL( keepout["border"]["pitch"]["valueNm"].get<std::string>(), "400000" );
    BOOST_CHECK_EQUAL( keepout["locked"].get<std::string>(), "LS_LOCKED" );
    BOOST_CHECK( !keepout.value( "filled", false ) );
    BOOST_REQUIRE_EQUAL( keepout["outline"]["polygons"][0]["outline"]["nodes"].size(), 4 );

    JSON text = getOne( "text" );
    BOOST_CHECK_EQUAL( text["id"]["value"].get<std::string>(),
                       stableUuid( "text", "managed-note" ) );
    BOOST_CHECK_EQUAL( text["layer"].get<std::string>(), "BL_F_SilkS" );
    BOOST_CHECK_EQUAL( text["text"]["text"].get<std::string>(), "KiChad\nmanaged" );
    BOOST_CHECK_EQUAL( text["text"]["position"]["xNm"].get<std::string>(), "23000000" );
    BOOST_CHECK_EQUAL( text["text"]["attributes"]["horizontalAlignment"].get<std::string>(),
                       "HA_LEFT" );
    BOOST_CHECK_EQUAL( text["text"]["attributes"]["verticalAlignment"].get<std::string>(),
                       "VA_TOP" );
    BOOST_CHECK_EQUAL( text["text"]["attributes"]["strokeWidth"]["valueNm"].get<std::string>(),
                       "200000" );
    BOOST_CHECK( text["text"]["attributes"]["multiline"].get<bool>() );
    BOOST_CHECK( text["text"]["attributes"]["bold"].get<bool>() );
    BOOST_CHECK( text["text"]["attributes"]["italic"].get<bool>() );
    BOOST_CHECK( text["text"]["attributes"]["keepUpright"].get<bool>() );
    BOOST_CHECK_EQUAL( text["text"]["hyperlink"].get<std::string>(),
                       "https://github.com/10-X-eng/KiChad" );
    BOOST_CHECK_EQUAL( text["locked"].get<std::string>(), "LS_LOCKED" );

    JSON dimensions = getItems( "dimension", 5 );
    const auto findDimension = [&]( const std::string& aLogicalId,
                                    const std::string& aStyle )
    {
        const std::string expectedId = stableUuid( "dimension", aLogicalId );
        auto found = std::find_if(
                dimensions.begin(), dimensions.end(),
                [&]( const JSON& candidate )
                {
                    return candidate["id"]["value"].get<std::string>() == expectedId
                           && candidate.contains( aStyle );
                } );
        BOOST_REQUIRE( found != dimensions.end() );
        return *found;
    };
    JSON dimension = findDimension( "board-width", "aligned" );
    BOOST_CHECK_EQUAL( dimension["id"]["value"].get<std::string>(),
                       stableUuid( "dimension", "board-width" ) );
    BOOST_CHECK_EQUAL( dimension["layer"].get<std::string>(), "BL_Dwgs_User" );
    BOOST_CHECK_EQUAL( dimension["aligned"]["start"]["xNm"].get<std::string>(),
                       "20000000" );
    BOOST_CHECK_EQUAL( dimension["aligned"]["end"]["xNm"].get<std::string>(),
                       "60000000" );
    BOOST_CHECK_EQUAL( dimension["aligned"]["height"]["valueNm"].get<std::string>(),
                       "-3000000" );
    BOOST_CHECK_EQUAL(
            dimension["aligned"]["extensionHeight"]["valueNm"].get<std::string>(),
            "200000" );
    BOOST_CHECK_EQUAL( dimension["prefix"].get<std::string>(), "W=" );
    BOOST_CHECK_EQUAL( dimension["unit"].get<std::string>(), "DU_MILLIMETERS" );
    BOOST_CHECK_EQUAL( dimension["unitFormat"].get<std::string>(), "DUF_BARE_SUFFIX" );
    BOOST_CHECK_EQUAL( dimension["precision"].get<std::string>(), "DP_FIXED_2" );
    BOOST_CHECK_EQUAL( dimension["arrowDirection"].get<std::string>(), "DAD_INWARD" );
    BOOST_CHECK_EQUAL( dimension["textPosition"].get<std::string>(), "DTP_MANUAL" );
    BOOST_CHECK_EQUAL( dimension["text"]["position"]["xNm"].get<std::string>(),
                       "40000000" );
    BOOST_CHECK_EQUAL( dimension["text"]["position"]["yNm"].get<std::string>(),
                       "17000000" );
    BOOST_CHECK_EQUAL( dimension["lineThickness"]["valueNm"].get<std::string>(),
                       "150000" );
    BOOST_CHECK_EQUAL( dimension["arrowLength"]["valueNm"].get<std::string>(),
                       "800000" );
    BOOST_CHECK_EQUAL( dimension["extensionOffset"]["valueNm"].get<std::string>(),
                       "100000" );
    BOOST_CHECK( dimension["suppressTrailingZeroes"].get<bool>() );
    BOOST_CHECK( !dimension.value( "keepTextAligned", false ) );
    BOOST_CHECK_EQUAL( dimension["locked"].get<std::string>(), "LS_LOCKED" );

    JSON orthogonal = findDimension( "board-height", "orthogonal" );
    BOOST_CHECK_EQUAL( orthogonal["orthogonal"]["alignment"].get<std::string>(),
                       "AA_Y_AXIS" );
    BOOST_CHECK( orthogonal["keepTextAligned"].get<bool>() );
    JSON radial = findDimension( "mounting-radius", "radial" );
    BOOST_CHECK_EQUAL( radial["radial"]["leaderLength"]["valueNm"].get<std::string>(),
                       "2000000" );
    JSON leader = findDimension( "inspect-callout", "leader" );
    BOOST_CHECK_EQUAL( leader["leader"]["borderStyle"].get<std::string>(),
                       "DTBS_ROUNDRECT" );
    BOOST_CHECK_EQUAL( leader["overrideText"].get<std::string>(), "Inspect joint" );
    JSON center = findDimension( "mounting-center", "center" );
    BOOST_CHECK_EQUAL( center["center"]["end"]["xNm"].get<std::string>(), "47000000" );

    JSON footprints = getItems( "footprint", 2 );
    const auto findFootprint = [&]( const std::string& aReference )
    {
        auto found = std::find_if(
                footprints.begin(), footprints.end(),
                [&]( const JSON& candidate )
                {
                    return candidate["referenceField"]["text"]["text"]["text"]
                                   .get<std::string>() == aReference;
                } );
        BOOST_REQUIRE( found != footprints.end() );
        return *found;
    };
    JSON footprint = findFootprint( "R1" );
    BOOST_CHECK_EQUAL( footprint["id"]["value"].get<std::string>(),
                       "11111111-1111-8111-8111-111111111111" );
    BOOST_CHECK_EQUAL( footprint["position"]["xNm"].get<std::string>(), "30000000" );
    BOOST_CHECK_EQUAL( footprint["position"]["yNm"].get<std::string>(), "40000000" );
    BOOST_CHECK_EQUAL( footprint["orientation"]["valueDegrees"].get<double>(), 135.0 );
    BOOST_CHECK_EQUAL( footprint["layer"].get<std::string>(), "BL_B_Cu" );
    BOOST_CHECK_EQUAL( footprint["locked"].get<std::string>(), "LS_LOCKED" );
    BOOST_CHECK_EQUAL(
            footprint["referenceField"]["text"]["text"]["text"].get<std::string>(),
            "R1" );
    BOOST_REQUIRE_EQUAL( footprint["symbolPath"]["path"].size(), 1 );
    BOOST_CHECK_EQUAL( footprint["symbolPath"]["path"][0]["value"].get<std::string>(),
                       "66666666-6666-8666-8666-666666666666" );

    JSON pad;

    for( const JSON& child : footprint["definition"]["items"] )
    {
        if( child.value( "@type", "" ).ends_with( ".Pad" )
            && child.value( "number", "" ) == "1" )
        {
            pad = child;
            break;
        }
    }

    BOOST_REQUIRE_MESSAGE( !pad.is_null(), footprint.dump() );
    BOOST_CHECK_EQUAL( pad["id"]["value"].get<std::string>(),
                       "88888888-8888-8888-8888-888888888888" );
    BOOST_REQUIRE_EQUAL( pad["padStack"]["layers"].size(), 3 );
    BOOST_CHECK_EQUAL( pad["padStack"]["layers"][0].get<std::string>(), "BL_B_Cu" );
    BOOST_CHECK_EQUAL( pad["padStack"]["layers"][1].get<std::string>(), "BL_B_Mask" );
    BOOST_CHECK_EQUAL( pad["padStack"]["layers"][2].get<std::string>(), "BL_B_Paste" );

    JSON createdFootprint = findFootprint( "R2" );
    const std::string managedFootprintId = stableUuid( "footprint", "R2" );
    BOOST_CHECK_EQUAL( createdFootprint["id"]["value"].get<std::string>(),
                       managedFootprintId );
    BOOST_CHECK_EQUAL( createdFootprint["position"]["xNm"].get<std::string>(),
                       "50000000" );
    BOOST_CHECK_EQUAL( createdFootprint["position"]["yNm"].get<std::string>(),
                       "40000000" );
    BOOST_CHECK_EQUAL( createdFootprint["orientation"]["valueDegrees"].get<double>(), 90.0 );
    BOOST_CHECK_EQUAL( createdFootprint["layer"].get<std::string>(), "BL_F_Cu" );
    BOOST_CHECK_EQUAL( createdFootprint["locked"].get<std::string>(), "LS_UNLOCKED" );
    BOOST_CHECK_EQUAL( createdFootprint["definition"]["id"]["libraryNickname"]
                               .get<std::string>(),
                       "Resistor_SMD" );
    BOOST_CHECK_EQUAL( createdFootprint["definition"]["id"]["entryName"]
                               .get<std::string>(),
                       "R_0603_1608Metric" );
    BOOST_REQUIRE_EQUAL( createdFootprint["symbolPath"]["path"].size(), 2 );
    BOOST_CHECK_EQUAL(
            createdFootprint["symbolPath"]["path"][0]["value"].get<std::string>(),
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                    "live_apply", "schematic_sheet", "power" ) );
    BOOST_CHECK_EQUAL(
            createdFootprint["symbolPath"]["path"][1]["value"].get<std::string>(),
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                    "live_apply", "schematic_symbol", "R2/1" ) );

    JSON createdPad;

    for( const JSON& child : createdFootprint["definition"]["items"] )
    {
        if( child.value( "@type", "" ).ends_with( ".Pad" )
            && child.value( "number", "" ) == "1" )
        {
            createdPad = child;
            break;
        }
    }

    BOOST_REQUIRE_MESSAGE( !createdPad.is_null(), createdFootprint.dump() );
    BOOST_CHECK_EQUAL( createdPad["net"]["name"].get<std::string>(), "Net1" );

    const std::string placementLine =
            "    (place R2 (at 50mm 40mm) (rotation 90deg) (side front) (locked false))\n";
    std::string withoutManagedFootprint = originalSource;
    const size_t placementPosition = withoutManagedFootprint.find( placementLine );
    BOOST_REQUIRE_NE( placementPosition, std::string::npos );
    withoutManagedFootprint.erase( placementPosition, placementLine.size() );
    JSON removalSaved = registry.Handle(
            "design", { { "operation", "save" },
                         { "path", sourceName },
                         { "source", withoutManagedFootprint },
                         { "expectedSha256", restoredHash } } );
    BOOST_REQUIRE_MESSAGE( removalSaved.at( "success" ).get<bool>(), removalSaved.dump() );
    const std::string removalHash =
            envelope( removalSaved )["data"]["sourceSha256"].get<std::string>();
    JSON removed = registry.Handle(
            "design", { { "operation", "apply" },
                         { "path", sourceName },
                         { "boardPath", boardName },
                         { "expectedSha256", removalHash } } );
    BOOST_REQUIRE_MESSAGE( removed.at( "success" ).get<bool>(), removed.dump() );
    JSON removedData = envelope( removed )["data"];
    BOOST_CHECK_EQUAL( removedData["counts"]["delete"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( removedData["counts"]["footprintDelete"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( removedData["counts"]["footprintCreate"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( removedData["managedItems"].get<int>(), 12 );
    JSON remainingFootprints = getItems( "footprint", 1 );
    BOOST_CHECK_EQUAL( remainingFootprints[0]["referenceField"]["text"]["text"]["text"]
                               .get<std::string>(),
                       "R1" );

    JSON recreateSaved = registry.Handle(
            "design", { { "operation", "save" },
                         { "path", sourceName },
                         { "source", originalSource },
                         { "expectedSha256", removalHash } } );
    BOOST_REQUIRE_MESSAGE( recreateSaved.at( "success" ).get<bool>(), recreateSaved.dump() );
    const std::string recreateHash =
            envelope( recreateSaved )["data"]["sourceSha256"].get<std::string>();
    JSON recreated = registry.Handle(
            "design", { { "operation", "apply" },
                         { "path", sourceName },
                         { "boardPath", boardName },
                         { "expectedSha256", recreateHash } } );
    BOOST_REQUIRE_MESSAGE( recreated.at( "success" ).get<bool>(), recreated.dump() );
    JSON recreatedData = envelope( recreated )["data"];
    BOOST_CHECK_EQUAL( recreatedData["counts"]["footprintCreate"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( recreatedData["counts"]["footprintDelete"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( recreatedData["managedItems"].get<int>(), 13 );
    JSON recreatedFootprints = getItems( "footprint", 2 );
    auto recreatedManaged = std::find_if(
            recreatedFootprints.begin(), recreatedFootprints.end(),
            []( const JSON& candidate )
            {
                return candidate["referenceField"]["text"]["text"]["text"]
                               .get<std::string>() == "R2";
            } );
    BOOST_REQUIRE( recreatedManaged != recreatedFootprints.end() );
    BOOST_CHECK_EQUAL( ( *recreatedManaged )["id"]["value"].get<std::string>(),
                       managedFootprintId );

    kiapi::common::commands::SaveDocument saveBoard;
    saveBoard.mutable_document()->CopyFrom( ipcTarget.document );
    kiapi::common::ApiResponse saveEnvelope;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.Call( ipcTarget, saveBoard, saveEnvelope, ipcError ), ipcError );
    const std::string savedBoard = readExactTextFile( board );
    BOOST_CHECK_NE( savedBoard.find( "(title \"KiChad AI-native release\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( savedBoard.find( "(date \"2026-07-19\")" ), std::string::npos );
    BOOST_CHECK_NE( savedBoard.find( "(rev \"KDS-1\")" ), std::string::npos );
    BOOST_CHECK_NE( savedBoard.find( "(company \"KiChad QA\")" ), std::string::npos );
    BOOST_CHECK_NE( savedBoard.find(
                            "(comment 1 \"Controlled by the KDS sidecar\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( savedBoard.find(
                            "(comment 9 \"Complete indexed title-block proof\")" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( MaterializesAndReleasesStepperReferenceWhenRequested )
{
    wxString projectPath;

    if( !wxGetEnv( wxS( "KICHAD_QA_STEPPER_REFERENCE_PROJECT" ), &projectPath ) )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in production stepper reference" );
        return;
    }

    wxFileName project = wxFileName::DirName( projectPath );
    project.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    BOOST_REQUIRE( project.DirExists() );
    const std::string boardName = "reference_stepper_controller.kicad_pcb";
    const std::string schematicName = "reference_stepper_controller.kicad_sch";
    const std::string sourceName = "reference_stepper_controller.kicad_kds";
    BOOST_REQUIRE( wxFileName( project.GetFullPath(), wxString::FromUTF8( boardName ) )
                           .FileExists() );
    BOOST_REQUIRE( wxFileName( project.GetFullPath(), wxString::FromUTF8( schematicName ) )
                           .FileExists() );
    BOOST_REQUIRE( wxFileName( project.GetFullPath(), wxString::FromUTF8( sourceName ) )
                           .FileExists() );

    wxString socketDirectory;
    wxGetEnv( wxS( "KICHAD_QA_LIVE_SOCKET_DIR" ), &socketDirectory );
    CODEX_TOOL_REGISTRY registry( [project]() { return project.GetFullPath(); },
                                  []() { return true; },
                                  [socketDirectory]() { return socketDirectory; } );
    JSON compiled = registry.Handle(
            "design", { { "operation", "compile" }, { "path", sourceName } } );
    BOOST_REQUIRE_MESSAGE( compiled.at( "success" ).get<bool>(), compiled.dump() );
    JSON compileData = envelope( compiled )["data"];
    BOOST_REQUIRE_MESSAGE( compileData["valid"].get<bool>(), compileData.dump( 2 ) );
    const std::string hash = compileData["sourceSha256"].get<std::string>();
    JSON applied = registry.Handle(
            "design", { { "operation", "apply" }, { "path", sourceName },
                         { "boardPath", boardName }, { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    JSON applyData = envelope( applied )["data"];
    BOOST_CHECK_EQUAL( applyData["transaction"].get<std::string>(), "committed" );
    BOOST_CHECK_EQUAL( applyData["managedFootprintLibrariesApplied"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( applyData["managedSymbolLibrariesApplied"].get<int>(), 1 );
    BOOST_CHECK_GT( applyData["managedItems"].get<int>(), 0 );

    JSON erc = registry.Handle( "verify", { { "operation", "erc" },
                                              { "path", schematicName } } );
    BOOST_REQUIRE_MESSAGE( erc.at( "success" ).get<bool>(), erc.dump() );
    BOOST_REQUIRE_MESSAGE( envelope( erc )["data"]["clean"].get<bool>(), erc.dump( 2 ) );
    JSON drc = registry.Handle( "verify", { { "operation", "drc" },
                                              { "path", boardName } } );
    BOOST_REQUIRE_MESSAGE( drc.at( "success" ).get<bool>(), drc.dump() );
    BOOST_REQUIRE_MESSAGE( envelope( drc )["data"]["clean"].get<bool>(), drc.dump( 2 ) );

    JSON fabricationArguments = { { "operation", "plan" },
                                  { "path", sourceName },
                                  { "boardPath", boardName },
                                  { "schematicPath", schematicName },
                                  { "expectedSha256", hash } };
    JSON planned = registry.Handle( "fabricate", fabricationArguments );
    BOOST_REQUIRE_MESSAGE( planned.at( "success" ).get<bool>(), planned.dump() );
    BOOST_CHECK( envelope( planned )["data"]["productionReady"].get<bool>() );
    BOOST_CHECK( envelope( planned )["data"]["runningReady"].get<bool>() );

    fabricationArguments["operation"] = "export";
    JSON exported = registry.HandleWithContext( "fabricate", fabricationArguments,
                                                 project.GetFullPath(), true,
                                                 socketDirectory, true,
                                                 std::chrono::milliseconds( 5000 ) );
    BOOST_REQUIRE_MESSAGE( exported.at( "success" ).get<bool>(), exported.dump( 2 ) );
    BOOST_CHECK( envelope( exported )["data"]["runningReady"].get<bool>() );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/design/reference_stepper_controller.kicad_kds" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/production/firmware/controller.hex" ) ) );
}


BOOST_AUTO_TEST_CASE( PreviewsExternalDesignReadOnlyWhenExplicitlyRequested )
{
    wxString sourcePath;

    if( !wxGetEnv( wxS( "KICHAD_QA_EXTERNAL_PREVIEW_KDS" ), &sourcePath ) )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in external KDS preview test" );
        return;
    }

    wxFileName source( sourcePath );
    source.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    BOOST_REQUIRE( source.FileExists() );
    BOOST_REQUIRE_EQUAL( source.GetExt(), wxS( "kicad_kds" ) );
    const wxString projectPath = source.GetPath();
    const std::string sourceBefore = readExactTextFile( source );
    CODEX_TOOL_REGISTRY registry(
            [projectPath]() { return projectPath; }, {}, {},
            []( const wxFileName& aRoot, const JSON& aCompilerIr,
                const JSON& aResolvedSymbols, std::string& aError )
            {
                return KICHAD::CODEX_TOOLS::ValidateNativeSchematicHierarchy(
                        aRoot, aCompilerIr, aResolvedSymbols, aError );
            } );
    JSON preview = registry.Handle(
            "design", { { "operation", "preview" },
                        { "path", source.GetFullName().ToStdString() } } );
    BOOST_REQUIRE_MESSAGE( preview.at( "success" ).get<bool>(), preview.dump( 2 ) );
    const JSON data = envelope( preview )["data"];
    BOOST_CHECK( data["schematicPlan"]["fullyLowered"].get<bool>() );
    BOOST_CHECK( data["schematicPlan"]["diagnostics"].empty() );
    BOOST_CHECK_EQUAL( readExactTextFile( source ), sourceBefore );
}


BOOST_AUTO_TEST_CASE( AppliesExternalDesignAgainstLivePcbEditorWhenRequested )
{
    wxString projectPath;
    wxString boardPath;
    wxString sourcePath;

    if( !wxGetEnv( wxS( "KICHAD_QA_EXTERNAL_PROJECT" ), &projectPath )
        || !wxGetEnv( wxS( "KICHAD_QA_EXTERNAL_BOARD" ), &boardPath )
        || !wxGetEnv( wxS( "KICHAD_QA_EXTERNAL_KDS" ), &sourcePath ) )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in external KDS apply test" );
        return;
    }

    wxFileName project = wxFileName::DirName( projectPath );
    project.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    wxFileName board( boardPath );
    board.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    wxFileName source( sourcePath );
    source.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    BOOST_REQUIRE( project.DirExists() );
    BOOST_REQUIRE( board.FileExists() );
    BOOST_REQUIRE( source.FileExists() );
    BOOST_REQUIRE_EQUAL( board.GetPath(), project.GetPath() );
    BOOST_REQUIRE_EQUAL( source.GetPath(), project.GetPath() );

    wxString socketDirectory;
    wxGetEnv( wxS( "KICHAD_QA_LIVE_SOCKET_DIR" ), &socketDirectory );
    CODEX_TOOL_REGISTRY registry( [project]() { return project.GetFullPath(); },
                                  []() { return true; },
                                  [socketDirectory]() { return socketDirectory; },
                                  []( const wxFileName&, const JSON&, const JSON&,
                                      std::string& )
                                  {
                                      return true;
                                  } );
    const std::string sourceName = source.GetFullName().ToStdString();
    const std::string boardName = board.GetFullName().ToStdString();
    JSON compiled = registry.Handle( "design", { { "operation", "compile" },
                                                  { "path", sourceName } } );
    BOOST_REQUIRE_MESSAGE( compiled.at( "success" ).get<bool>(), compiled.dump( 2 ) );
    JSON compileData = envelope( compiled )["data"];
    BOOST_REQUIRE_MESSAGE( compileData["valid"].get<bool>(), compileData.dump( 2 ) );

    JSON applied = registry.Handle(
            "design", { { "operation", "apply" },
                         { "path", sourceName },
                         { "boardPath", boardName },
                         { "expectedSha256", compileData["sourceSha256"] } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump( 2 ) );
    JSON applyData = envelope( applied )["data"];
    BOOST_REQUIRE_MESSAGE( applyData.value( "transaction", "" ) == "committed",
                           applyData.dump( 2 ) );
    BOOST_TEST_MESSAGE( "External KDS apply committed: " + applyData.dump() );
}


BOOST_AUTO_TEST_SUITE_END()
