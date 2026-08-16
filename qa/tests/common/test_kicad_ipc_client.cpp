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
#include <kicad/codex/kicad_ipc_client.h>
#include <kicad/codex/kicad_ipc_transaction.h>

#include <api/board/board_types.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <chrono>
#include <kiid.h>
#include <kinng.h>
#include <memory>
#include <optional>
#include <vector>
#include <wx/filename.h>
#include <wx/utils.h>


BOOST_AUTO_TEST_SUITE( KiChadIpcClient )


BOOST_AUTO_TEST_CASE( DiscoversBoardAndUsesTransactionEnvelope )
{
    wxFileName socketDirectory = wxFileName::DirName( wxFileName::GetTempDir() );
    socketDirectory.AppendDir( wxS( "kichad-ipc-test-" ) + KIID().AsString() );
    BOOST_REQUIRE( wxFileName::Mkdir( socketDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );

    wxFileName projectDirectory( socketDirectory );
    projectDirectory.AppendDir( wxS( "project" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( projectDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );

    wxFileName socketPath( socketDirectory.GetFullPath(), wxS( "api-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-kicad-token";
    const std::string commitId = KIID().AsStdString();

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
                    document->mutable_project()->set_path(
                            projectDirectory.GetFullPath().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    kiapi::common::commands::BeginCommitResponse begin;
                    begin.mutable_id()->set_value( commitId );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( begin );
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;

                    if( request.message().UnpackTo( &end ) && end.id().value() == commitId
                        && end.action() == kiapi::common::commands::CMA_COMMIT
                        && request.header().kicad_token() == token )
                    {
                        kiapi::common::commands::EndCommitResponse ended;
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( ended );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    KICHAD_IPC_CLIENT client( "org.kichad.qa", socketDirectory.GetFullPath(),
                              std::chrono::milliseconds( 1000 ) );
    KICHAD_IPC_TARGET target;
    std::string       error;
    BOOST_REQUIRE_MESSAGE( client.FindOpenPcb( projectDirectory.GetFullPath(),
                                               wxS( "design.kicad_pcb" ), target, error ),
                           error );
    BOOST_CHECK_EQUAL( target.kicadToken, token );
    BOOST_CHECK_EQUAL( target.document.board_filename(), "design.kicad_pcb" );

    std::string returnedCommitId;
    BOOST_REQUIRE_MESSAGE( client.BeginCommit( target, returnedCommitId, error ), error );
    BOOST_CHECK_EQUAL( returnedCommitId, commitId );
    BOOST_CHECK_MESSAGE( client.EndCommit( target, returnedCommitId, true,
                                           "KiChad QA transaction", error ),
                         error );

    server.Stop();
    wxFileName::Rmdir( socketDirectory.GetFullPath(), wxPATH_RMDIR_RECURSIVE );
}


BOOST_AUTO_TEST_CASE( ReplacesFootprintExactlyAndRollsBackARejectedCreation )
{
    wxFileName socketDirectory = wxFileName::DirName( wxFileName::GetTempDir() );
    socketDirectory.AppendDir( wxS( "kichad-ipc-replace-test-" ) + KIID().AsString() );
    BOOST_REQUIRE( wxFileName::Mkdir( socketDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );
    wxFileName projectDirectory( socketDirectory );
    projectDirectory.AppendDir( wxS( "project" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( projectDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );
    wxFileName socketPath( socketDirectory.GetFullPath(), wxS( "api-replace.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-replace-token";
    const std::string oldId = "11111111-1111-8111-8111-111111111111";
    const std::string newId = "22222222-2222-8222-8222-222222222222";
    const std::string commitId = "33333333-3333-8333-8333-333333333333";
    const std::string modelPath =
            "${KICAD10_3DMODEL_DIR}/Resistor_SMD.3dshapes/R_0603_1608Metric.step";
    const std::string source =
            "(footprint \"NEW_PACKAGE\"\n"
            "  (version 20260206) (generator \"kichad_qa\") (layer \"F.Cu\")\n"
            "  (pad \"1\" smd rect (at 0 0) (size 1 2) (layers \"F.Cu\" \"F.Mask\"))\n"
            "  (pad \"\" smd rect (at 0 0) (size 0.5 0.5) (layers \"F.Paste\"))\n"
            "  (model \"" + modelPath + "\")\n"
            ")\n";
    kiapi::board::types::FootprintInstance oldFootprint;
    oldFootprint.mutable_id()->set_value( oldId );
    oldFootprint.mutable_definition()->mutable_id()->set_library_nickname( "Old" );
    oldFootprint.mutable_definition()->mutable_id()->set_entry_name( "OLD_PACKAGE" );
    oldFootprint.mutable_reference_field()->mutable_text()->mutable_text()->set_text( "U1" );
    oldFootprint.mutable_symbol_path()->add_path()->set_value(
            "44444444-4444-8444-8444-444444444444" );
    std::optional<kiapi::board::types::FootprintInstance> live = oldFootprint;
    std::optional<kiapi::board::types::FootprintInstance> snapshot;
    bool transactionActive = false;
    bool rejectReplacement = false;
    bool sawExactSource = false;
    bool sawDeleteCreateReplacement = false;
    std::optional<std::string> reservedUuid;
    int commits = 0;
    int drops = 0;

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
                    document->mutable_project()->set_path(
                            projectDirectory.GetFullPath().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    if( transactionActive )
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                    else
                    {
                        transactionActive = true;
                        snapshot = live;
                        kiapi::common::commands::BeginCommitResponse begin;
                        begin.mutable_id()->set_value( commitId );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( begin );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::DeleteItems>() )
                {
                    kiapi::common::commands::DeleteItems remove;
                    kiapi::common::commands::DeleteItemsResponse deleted;

                    if( transactionActive && request.message().UnpackTo( &remove )
                        && remove.item_ids_size() == 1 && live
                        && remove.item_ids( 0 ).value() == live->id().value() )
                    {
                        auto* result = deleted.add_deleted_items();
                        result->mutable_id()->CopyFrom( remove.item_ids( 0 ) );
                        result->set_status( kiapi::common::commands::IDS_OK );
                        deleted.set_status( kiapi::common::types::IRS_OK );
                        reservedUuid = live->id().value();
                        sawDeleteCreateReplacement = true;
                        live.reset();
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( deleted );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<
                                 kiapi::common::commands::ParseAndCreateItemsFromString>() )
                {
                    kiapi::common::commands::ParseAndCreateItemsFromString parse;
                    kiapi::common::commands::CreateItemsResponse created;
                    kiapi::board::types::FootprintInstance requested;

                    if( transactionActive && request.message().UnpackTo( &parse )
                        && parse.contents() == source && parse.item().UnpackTo( &requested ) )
                    {
                        sawExactSource = true;
                        auto* result = created.add_created_items();
                        created.set_status( kiapi::common::types::IRS_OK );

                        // Model the PCB editor's real transaction behavior: deleting an item does
                        // not release its UUID until the enclosing commit ends.  The old
                        // delete/create implementation therefore receives ISC_EXISTING here.
                        if( !parse.has_replace_item_id() && reservedUuid
                            && requested.id().value() == *reservedUuid )
                        {
                            result->mutable_status()->set_code(
                                    kiapi::common::commands::ISC_EXISTING );
                            result->mutable_status()->set_error_message(
                                    "an item with this UUID is reserved by the active commit" );
                        }
                        else if( !live || !parse.has_replace_item_id()
                                 || parse.replace_item_id().value() != live->id().value() )
                        {
                            result->mutable_status()->set_code(
                                    kiapi::common::commands::ISC_INVALID_DATA );
                            result->mutable_status()->set_error_message(
                                    "atomic replacement target is not the live footprint" );
                        }
                        else if( rejectReplacement )
                        {
                            result->mutable_status()->set_code(
                                    kiapi::common::commands::ISC_INVALID_DATA );
                            result->mutable_status()->set_error_message(
                                    "injected atomic replacement rejection" );
                        }
                        else
                        {
                            kiapi::board::types::FootprintInstance active = requested;
                            // KiCad's native angle type canonically serializes 270 degrees as
                            // -90 degrees.  KDS readback validation must compare rotations modulo
                            // one turn rather than rejecting equivalent native representations.
                            active.mutable_orientation()->set_value_degrees( -90.0 );
                            active.mutable_definition()->clear_items();
                            kiapi::board::types::Pad electricalPad;
                            electricalPad.set_number( "1" );
                            electricalPad.mutable_net()->set_name( "SIGNAL" );
                            active.mutable_definition()->add_items()->PackFrom( electricalPad );
                            kiapi::board::types::Pad aperturePad;
                            aperturePad.set_number( "" );
                            active.mutable_definition()->add_items()->PackFrom( aperturePad );
                            kiapi::board::types::Footprint3DModel model;
                            model.set_filename( modelPath );
                            active.mutable_definition()->add_items()->PackFrom( model );
                            kiapi::board::types::Field manufacturer;
                            manufacturer.set_name( "Manufacturer" );
                            manufacturer.mutable_text()->mutable_text()->set_text( "Acme" );
                            active.mutable_definition()->add_items()->PackFrom( manufacturer );
                            live = active;
                            result->mutable_status()->set_code(
                                    kiapi::common::commands::ISC_OK );
                            result->mutable_item()->PackFrom( active );
                        }

                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( created );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;

                    if( transactionActive && request.message().UnpackTo( &end )
                        && end.id().value() == commitId )
                    {
                        if( end.action() == kiapi::common::commands::CMA_COMMIT )
                        {
                            ++commits;
                        }
                        else if( end.action() == kiapi::common::commands::CMA_DROP )
                        {
                            live = snapshot;
                            ++drops;
                        }

                        transactionActive = false;
                        snapshot.reset();
                        reservedUuid.reset();
                        kiapi::common::commands::EndCommitResponse ended;
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( ended );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    KICHAD_IPC_CLIENT client( "org.kichad.qa-replace", socketDirectory.GetFullPath(),
                              std::chrono::milliseconds( 1000 ) );
    KICHAD_IPC_TARGET target;
    std::string error;
    BOOST_REQUIRE_MESSAGE( client.FindOpenPcb( projectDirectory.GetFullPath(),
                                               wxS( "design.kicad_pcb" ), target, error ),
                           error );
    const nlohmann::json action = {
        { "action", "replace_footprint" }, { "component", "U1" },
        { "logicalId", "U1" }, { "itemType", "footprint" },
        { "replacedItemId", oldId }, { "itemId", newId },
        { "instance",
          { { "libraryId", "New:NEW_PACKAGE" }, { "value", "Controller" },
            { "dnp", true },
            { "symbolPath", nlohmann::json::array(
                    { "44444444-4444-8444-8444-444444444444",
                      "55555555-5555-8555-8555-555555555555" } ) },
            { "symbolSheetName", "Control" },
            { "symbolSheetFilename", "control.kicad_sch" },
            { "fields",
              { { "Datasheet", "https://example.test/controller.pdf" },
                { "Description", "Replacement controller" },
                { "Manufacturer", "Acme" } } },
            { "padNets", { { "1", "SIGNAL" } } } } },
        { "position", { { "xNm", 12500000 }, { "yNm", 7500000 } } },
        { "rotationDegrees", 270.0 }, { "side", "back" }, { "locked", true },
        { "presentation", nlohmann::json::object() }
    };
    const nlohmann::json actions = nlohmann::json::array( { action } );
    const nlohmann::json sources = { { "New:NEW_PACKAGE", source } };

    {
        KICHAD_IPC_COMMIT_GUARD commit( client, target );
        BOOST_REQUIRE_MESSAGE( commit.Begin( error ), error );
        BOOST_REQUIRE_MESSAGE( KICHAD::CODEX_TOOLS::ExecutePcbActions(
                                       client, target, actions, sources, error ),
                               error );
        BOOST_REQUIRE_MESSAGE( commit.Commit( "replace footprint", error ), error );
    }

    BOOST_REQUIRE( live );
    BOOST_CHECK_EQUAL( live->id().value(), newId );
    BOOST_CHECK_EQUAL( live->definition().id().library_nickname(), "New" );
    BOOST_CHECK_EQUAL( live->definition().id().entry_name(), "NEW_PACKAGE" );
    BOOST_CHECK_EQUAL( live->orientation().value_degrees(), -90.0 );
    BOOST_CHECK_EQUAL( live->locked(), kiapi::common::types::LS_LOCKED );
    BOOST_CHECK( live->attributes().do_not_populate() );
    BOOST_CHECK_EQUAL( commits, 1 );
    BOOST_CHECK_EQUAL( drops, 0 );
    BOOST_CHECK( sawExactSource );
    BOOST_CHECK( !sawDeleteCreateReplacement );

    live = oldFootprint;
    rejectReplacement = true;
    sawExactSource = false;
    error.clear();

    {
        KICHAD_IPC_COMMIT_GUARD commit( client, target );
        BOOST_REQUIRE_MESSAGE( commit.Begin( error ), error );
        BOOST_CHECK( !KICHAD::CODEX_TOOLS::ExecutePcbActions(
                client, target, actions, sources, error ) );
        BOOST_CHECK_NE( error.find( "injected atomic replacement rejection" ),
                        std::string::npos );
        std::string dropError;
        BOOST_REQUIRE_MESSAGE( commit.Drop( dropError ), dropError );
    }

    BOOST_REQUIRE( live );
    BOOST_CHECK_EQUAL( live->id().value(), oldId );
    BOOST_CHECK_EQUAL( live->definition().id().library_nickname(), "Old" );
    BOOST_CHECK_EQUAL( live->definition().id().entry_name(), "OLD_PACKAGE" );
    BOOST_CHECK_EQUAL( commits, 1 );
    BOOST_CHECK_EQUAL( drops, 1 );
    BOOST_CHECK( sawExactSource );
    BOOST_CHECK( !sawDeleteCreateReplacement );
    server.Stop();
    wxFileName::Rmdir( socketDirectory.GetFullPath(), wxPATH_RMDIR_RECURSIVE );
}


BOOST_AUTO_TEST_CASE( DiscoveryTimeoutCoversAllUnresponsiveSockets )
{
    wxFileName socketDirectory = wxFileName::DirName( wxFileName::GetTempDir() );
    socketDirectory.AppendDir( wxS( "kichad-ipc-timeout-test-" ) + KIID().AsString() );
    BOOST_REQUIRE( wxFileName::Mkdir( socketDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );

    std::vector<std::unique_ptr<KINNG_REQUEST_SERVER>> servers;

    for( int i = 0; i < 3; ++i )
    {
        wxFileName socketPath( socketDirectory.GetFullPath(),
                               wxString::Format( wxS( "api-%d.sock" ), i ) );
        auto server = std::make_unique<KINNG_REQUEST_SERVER>(
                "ipc://" + socketPath.GetFullPath().ToStdString() );
        server->SetCallback( []( std::string* ) {} );
        servers.emplace_back( std::move( server ) );
    }

    KICHAD_IPC_CLIENT client( "org.kichad.qa", socketDirectory.GetFullPath(),
                              std::chrono::milliseconds( 100 ) );
    KICHAD_IPC_TARGET target;
    std::string       error;
    auto              start = std::chrono::steady_clock::now();

    BOOST_CHECK( !client.FindOpenPcb( socketDirectory.GetFullPath(),
                                      wxS( "missing.kicad_pcb" ), target, error ) );

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start );
    BOOST_CHECK_LT( elapsed.count(), 500 );
    BOOST_CHECK( !error.empty() );

    for( const auto& server : servers )
        server->Stop();

    wxFileName::Rmdir( socketDirectory.GetFullPath(), wxPATH_RMDIR_RECURSIVE );
}


BOOST_AUTO_TEST_CASE( StaleSocketDoesNotHideLiveEditor )
{
    wxFileName socketDirectory = wxFileName::DirName( wxFileName::GetTempDir() );
    socketDirectory.AppendDir( wxS( "kichad-ipc-stale-test-" ) + KIID().AsString() );
    BOOST_REQUIRE( wxFileName::Mkdir( socketDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );

    wxFileName projectDirectory( socketDirectory );
    projectDirectory.AppendDir( wxS( "project" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( projectDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );

    wxFileName stalePath( socketDirectory.GetFullPath(), wxS( "api-0.sock" ) );
    KINNG_REQUEST_SERVER staleServer( "ipc://" + stalePath.GetFullPath().ToStdString() );
    staleServer.SetCallback( []( std::string* ) {} );

    wxFileName livePath( socketDirectory.GetFullPath(), wxS( "api-1.sock" ) );
    KINNG_REQUEST_SERVER liveServer( "ipc://" + livePath.GetFullPath().ToStdString() );
    const std::string token = "qa-live-after-stale";
    liveServer.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( request.ParseFromString( *aSerializedRequest )
                    && request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path(
                            projectDirectory.GetFullPath().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }

                liveServer.Reply( response.SerializeAsString() );
            } );

    KICHAD_IPC_CLIENT client( "org.kichad.qa", socketDirectory.GetFullPath(),
                              std::chrono::milliseconds( 600 ) );
    KICHAD_IPC_TARGET target;
    std::string error;
    BOOST_REQUIRE_MESSAGE( client.FindOpenPcb( projectDirectory.GetFullPath(),
                                               wxS( "design.kicad_pcb" ), target, error ),
                           error );
    BOOST_CHECK_EQUAL( target.kicadToken, token );
    BOOST_CHECK_EQUAL( target.socketUrl,
                       "ipc://" + livePath.GetFullPath().ToStdString() );

    liveServer.Stop();
    staleServer.Stop();
    wxFileName::Rmdir( socketDirectory.GetFullPath(), wxPATH_RMDIR_RECURSIVE );
}


BOOST_AUTO_TEST_CASE( LiveCreateUpdateDeleteWhenRequested )
{
    wxString projectPath;
    wxString boardPath;
    wxString socketDirectory;

    if( !wxGetEnv( wxS( "KICHAD_QA_LIVE_PROJECT" ), &projectPath )
        || !wxGetEnv( wxS( "KICHAD_QA_LIVE_BOARD" ), &boardPath ) )
    {
        BOOST_TEST_MESSAGE( "Set KICHAD_QA_LIVE_PROJECT and KICHAD_QA_LIVE_BOARD to run the "
                            "live mutating IPC smoke test" );
        BOOST_CHECK( true );
        return;
    }

    wxGetEnv( wxS( "KICHAD_QA_LIVE_SOCKET_DIR" ), &socketDirectory );

    KICHAD_IPC_CLIENT client( "org.kichad.qa-live", socketDirectory,
                              std::chrono::milliseconds( 2000 ) );
    KICHAD_IPC_TARGET target;
    std::string       error;
    BOOST_REQUIRE_MESSAGE( client.FindOpenPcb( projectPath, boardPath, target, error ), error );

    std::string commitId;
    BOOST_REQUIRE_MESSAGE( client.BeginCommit( target, commitId, error ), error );
    bool transactionActive = true;

    auto dropTransaction = [&]()
    {
        if( transactionActive )
        {
            std::string ignored;
            client.EndCommit( target, commitId, false, "", ignored );
            transactionActive = false;
        }
    };

    kiapi::board::types::Track trace;
    trace.mutable_start()->set_x_nm( 10000000 );
    trace.mutable_start()->set_y_nm( 10000000 );
    trace.mutable_end()->set_x_nm( 12000000 );
    trace.mutable_end()->set_y_nm( 10000000 );
    trace.mutable_width()->set_value_nm( 200000 );
    trace.set_locked( kiapi::common::types::LS_UNLOCKED );
    trace.set_layer( kiapi::board::types::BL_F_Cu );

    kiapi::common::commands::CreateItems createRequest;
    createRequest.mutable_header()->mutable_document()->CopyFrom( target.document );
    createRequest.add_items()->PackFrom( trace );

    kiapi::common::ApiResponse createEnvelope;

    if( !client.Call( target, createRequest, createEnvelope, error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    kiapi::common::commands::CreateItemsResponse created;

    if( !createEnvelope.message().UnpackTo( &created ) || created.created_items_size() != 1
        || created.created_items( 0 ).status().code() != kiapi::common::commands::ISC_OK
        || !created.created_items( 0 ).item().UnpackTo( &trace ) || trace.id().value().empty() )
    {
        dropTransaction();
        BOOST_FAIL( "KiCad returned an invalid live create response" );
    }

    if( !client.EndCommit( target, commitId, true, "KiChad live IPC create", error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    transactionActive = false;

    if( !client.BeginCommit( target, commitId, error ) )
        BOOST_FAIL( error );

    transactionActive = true;

    kiapi::board::types::Track update;
    update.mutable_id()->CopyFrom( trace.id() );
    update.mutable_width()->set_value_nm( 250000 );

    kiapi::common::commands::UpdateItems updateRequest;
    updateRequest.mutable_header()->mutable_document()->CopyFrom( target.document );
    updateRequest.mutable_header()->mutable_field_mask()->add_paths( "width" );
    updateRequest.add_items()->PackFrom( update );

    kiapi::common::ApiResponse updateEnvelope;

    if( !client.Call( target, updateRequest, updateEnvelope, error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    kiapi::common::commands::UpdateItemsResponse updated;

    if( !updateEnvelope.message().UnpackTo( &updated ) || updated.updated_items_size() != 1
        || updated.updated_items( 0 ).status().code() != kiapi::common::commands::ISC_OK )
    {
        dropTransaction();
        BOOST_FAIL( "KiCad returned an invalid live update response: envelope="
                    + updateEnvelope.ShortDebugString() + " updated="
                    + updated.ShortDebugString() );
    }

    kiapi::board::types::Track updatedTrace;
    BOOST_REQUIRE( updated.updated_items( 0 ).item().UnpackTo( &updatedTrace ) );
    BOOST_CHECK_EQUAL( updatedTrace.start().x_nm(), 10000000 );
    BOOST_CHECK_EQUAL( updatedTrace.end().x_nm(), 12000000 );
    BOOST_CHECK_EQUAL( updatedTrace.width().value_nm(), 250000 );

    if( !client.EndCommit( target, commitId, true, "KiChad live IPC update", error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    transactionActive = false;

    if( !client.BeginCommit( target, commitId, error ) )
        BOOST_FAIL( error );

    transactionActive = true;

    kiapi::common::commands::DeleteItems deleteRequest;
    deleteRequest.mutable_header()->mutable_document()->CopyFrom( target.document );
    deleteRequest.add_item_ids()->CopyFrom( trace.id() );

    kiapi::common::ApiResponse deleteEnvelope;

    if( !client.Call( target, deleteRequest, deleteEnvelope, error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    kiapi::common::commands::DeleteItemsResponse deleted;

    if( !deleteEnvelope.message().UnpackTo( &deleted ) || deleted.deleted_items_size() != 1
        || deleted.deleted_items( 0 ).status() != kiapi::common::commands::IDS_OK )
    {
        dropTransaction();
        BOOST_FAIL( "KiCad returned an invalid live delete response" );
    }

    if( !client.EndCommit( target, commitId, true, "KiChad live IPC delete", error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    transactionActive = false;

    // Exercise the native footprint-exchange path against a real PCB Editor and prove that
    // dropping its enclosing commit restores the complete original instance.
    kiapi::common::commands::GetItems getFootprints;
    getFootprints.mutable_header()->mutable_document()->CopyFrom( target.document );
    getFootprints.add_types( kiapi::common::types::KOT_PCB_FOOTPRINT );
    kiapi::common::ApiResponse getEnvelope;
    BOOST_REQUIRE_MESSAGE( client.Call( target, getFootprints, getEnvelope, error ), error );
    kiapi::common::commands::GetItemsResponse footprintItems;
    BOOST_REQUIRE( getEnvelope.message().UnpackTo( &footprintItems ) );

    std::optional<kiapi::board::types::FootprintInstance> originalFootprint;

    for( const google::protobuf::Any& packed : footprintItems.items() )
    {
        kiapi::board::types::FootprintInstance candidate;

        if( packed.UnpackTo( &candidate ) && !candidate.id().value().empty()
            && candidate.has_reference_field() && candidate.has_value_field()
            && candidate.has_symbol_path() && candidate.symbol_path().path_size() > 0 )
        {
            originalFootprint = std::move( candidate );
            break;
        }
    }

    BOOST_REQUIRE_MESSAGE( originalFootprint,
                           "live atomic replacement QA requires one linked footprint" );
    kiapi::board::types::FootprintInstance replacement = *originalFootprint;
    replacement.mutable_definition()->clear_items();
    replacement.mutable_definition()->mutable_id()->set_library_nickname( "KiChadQA" );
    replacement.mutable_definition()->mutable_id()->set_entry_name( "AtomicReplacement" );
    const std::string replacementSource =
            "(footprint \"AtomicReplacement\"\n"
            "  (version 20260206) (generator \"kichad_qa\") (layer \"F.Cu\")\n"
            "  (fp_rect (start -1 -1) (end 1 1) (stroke (width 0.2) (type default)) "
            "(fill none) (layer \"F.SilkS\"))\n"
            "  (pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\" \"F.Mask\"))\n"
            ")\n";

    BOOST_REQUIRE_MESSAGE( client.BeginCommit( target, commitId, error ), error );
    transactionActive = true;
    kiapi::common::commands::ParseAndCreateItemsFromString replaceRequest;
    replaceRequest.mutable_document()->CopyFrom( target.document );
    replaceRequest.set_contents( replacementSource );
    replaceRequest.mutable_item()->PackFrom( replacement );
    replaceRequest.mutable_replace_item_id()->CopyFrom( originalFootprint->id() );
    kiapi::common::ApiResponse replaceEnvelope;

    if( !client.Call( target, replaceRequest, replaceEnvelope, error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    kiapi::common::commands::CreateItemsResponse replaced;

    if( !replaceEnvelope.message().UnpackTo( &replaced )
        || replaced.created_items_size() != 1
        || replaced.created_items( 0 ).status().code() != kiapi::common::commands::ISC_OK )
    {
        dropTransaction();
        BOOST_FAIL( "KiCad returned an invalid live atomic replacement response: "
                    + replaceEnvelope.ShortDebugString() );
    }

    kiapi::board::types::FootprintInstance activeReplacement;
    BOOST_REQUIRE( replaced.created_items( 0 ).item().UnpackTo( &activeReplacement ) );
    BOOST_CHECK_EQUAL( activeReplacement.id().value(), originalFootprint->id().value() );
    BOOST_CHECK_EQUAL( activeReplacement.definition().id().library_nickname(), "KiChadQA" );
    BOOST_CHECK_EQUAL( activeReplacement.definition().id().entry_name(),
                       "AtomicReplacement" );
    BOOST_REQUIRE_MESSAGE( client.EndCommit( target, commitId, false, "", error ), error );
    transactionActive = false;

    kiapi::common::commands::GetItemsById getRestored;
    getRestored.mutable_header()->mutable_document()->CopyFrom( target.document );
    getRestored.add_items()->CopyFrom( originalFootprint->id() );
    kiapi::common::ApiResponse restoredEnvelope;
    BOOST_REQUIRE_MESSAGE( client.Call( target, getRestored, restoredEnvelope, error ), error );
    kiapi::common::commands::GetItemsResponse restoredItems;
    BOOST_REQUIRE( restoredEnvelope.message().UnpackTo( &restoredItems ) );
    BOOST_REQUIRE_EQUAL( restoredItems.items_size(), 1 );
    kiapi::board::types::FootprintInstance restored;
    BOOST_REQUIRE( restoredItems.items( 0 ).UnpackTo( &restored ) );
    BOOST_CHECK_EQUAL( restored.id().value(), originalFootprint->id().value() );
    BOOST_CHECK_EQUAL( restored.definition().id().library_nickname(),
                       originalFootprint->definition().id().library_nickname() );
    BOOST_CHECK_EQUAL( restored.definition().id().entry_name(),
                       originalFootprint->definition().id().entry_name() );
}


BOOST_AUTO_TEST_SUITE_END()
