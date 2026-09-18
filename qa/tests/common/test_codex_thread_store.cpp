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

#include <boost/test/unit_test.hpp>

#include <kicad/codex/codex_thread_store.h>

#include <nlohmann/json.hpp>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/utils.h>


BOOST_AUTO_TEST_SUITE( CodexThreadStore )


BOOST_AUTO_TEST_CASE( PersistsConversationAcrossNativeToolSchemaChanges )
{
    const wxString root = wxFileName::CreateTempFileName( wxS( "kichad-thread-store-" ) );
    BOOST_REQUIRE( wxRemoveFile( root ) );
    BOOST_REQUIRE( wxFileName::Mkdir( root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    const wxString index = wxFileName( root, wxS( "threads.json" ) ).GetFullPath();
    const wxString project = wxFileName( root, wxS( "project" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( project, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    CODEX_THREAD_STORE store( index );
    wxString error;
    CODEX_THREAD_STORE::BINDING saved = {
        "thread-current", 2,
        { { "user", "Design a motor controller." },
          { "assistant", "I created the design." } }
    };
    BOOST_REQUIRE_MESSAGE( store.Save( project, saved, &error ), error );

    CODEX_THREAD_STORE::BINDING loaded = store.Load( project );
    BOOST_CHECK_EQUAL( loaded.threadId, "thread-current" );
    BOOST_CHECK_EQUAL( loaded.toolSchemaVersion, 2 );
    BOOST_REQUIRE_EQUAL( loaded.messages.size(), 2 );
    BOOST_CHECK_EQUAL( loaded.messages[0].role, "user" );
    BOOST_CHECK_EQUAL( loaded.messages[0].text, "Design a motor controller." );
    BOOST_CHECK_EQUAL( loaded.messages[1].role, "assistant" );
    BOOST_CHECK_EQUAL( loaded.messages[1].text, "I created the design." );
    BOOST_CHECK( store.Load( wxFileName( root, wxS( "other" ) ).GetFullPath() )
                         .threadId.empty() );

    wxFFile file( index, wxS( "rb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    wxString text;
    BOOST_REQUIRE( file.ReadAll( &text, wxConvUTF8 ) );
    nlohmann::json document = nlohmann::json::parse( std::string( text.ToUTF8() ) );
    BOOST_CHECK_EQUAL( document["version"].get<int>(), 3 );
    file.Close();

    BOOST_CHECK( wxFileName::Rmdir( root, wxPATH_RMDIR_RECURSIVE ) );
}


BOOST_AUTO_TEST_CASE( ParsesProjectDialogLogsIntoConversationHistory )
{
    const std::string log =
            "[2026-09-16 20:58:53] USER:\n"
            "did I tell you that I want serializers between the image sensors and the STM32?\n\n"
            "[2026-09-16 20:59:01] CODEX:\n"
            "No. Should I make it a firm requirement?\n\n"
            "Second paragraph with a [bracket] inside.\n\n"
            "[2026-09-16 20:59:07] USER:\n"
            "yes\n\n"
            "[2026-09-16 21:00:06] CODEX:\n"
            "Confirmed and recorded.\n\n";

    std::vector<CODEX_THREAD_STORE::MESSAGE> messages =
            CODEX_THREAD_STORE::ParseDialogLog( log );
    BOOST_REQUIRE_EQUAL( messages.size(), 4 );
    BOOST_CHECK_EQUAL( messages[0].role, "user" );
    BOOST_CHECK_EQUAL( messages[0].text,
                       "did I tell you that I want serializers between the image sensors and the STM32?" );
    BOOST_CHECK_EQUAL( messages[1].role, "assistant" );
    BOOST_CHECK_EQUAL( messages[1].text,
                       "No. Should I make it a firm requirement?\n\nSecond paragraph with a [bracket] inside." );
    BOOST_CHECK_EQUAL( messages[2].text, "yes" );
    BOOST_CHECK_EQUAL( messages[3].role, "assistant" );

    // The byte budget keeps the newest messages and starts on a user message.
    std::vector<CODEX_THREAD_STORE::MESSAGE> recent =
            CODEX_THREAD_STORE::ParseDialogLog( log, 40 );
    BOOST_REQUIRE_EQUAL( recent.size(), 2 );
    BOOST_CHECK_EQUAL( recent[0].text, "yes" );
    BOOST_CHECK_EQUAL( recent[1].text, "Confirmed and recorded." );

    BOOST_CHECK( CODEX_THREAD_STORE::ParseDialogLog( "" ).empty() );
    BOOST_CHECK( CODEX_THREAD_STORE::ParseDialogLog( "no headers here\n" ).empty() );
}


BOOST_AUTO_TEST_CASE( LoadsLegacyUnversionedThreadBindingsForHistoryImport )
{
    const wxString root = wxFileName::CreateTempFileName( wxS( "kichad-thread-legacy-" ) );
    BOOST_REQUIRE( wxRemoveFile( root ) );
    BOOST_REQUIRE( wxFileName::Mkdir( root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    const wxString index = wxFileName( root, wxS( "threads.json" ) ).GetFullPath();
    const wxString project = wxFileName( root, wxS( "project" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( project, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    wxFileName canonicalProject = wxFileName::DirName( project );
    canonicalProject.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    nlohmann::json legacy = {
        { "version", 1 },
        { "projects",
          { { std::string( canonicalProject.GetFullPath().ToUTF8() ), "thread-legacy" } } }
    };
    wxFFile file( index, wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE( file.Write( wxString::FromUTF8( legacy.dump() ), wxConvUTF8 ) );
    file.Close();

    CODEX_THREAD_STORE store( index );
    CODEX_THREAD_STORE::BINDING loaded = store.Load( project );
    BOOST_CHECK_EQUAL( loaded.threadId, "thread-legacy" );
    BOOST_CHECK_EQUAL( loaded.toolSchemaVersion, 0 );
    BOOST_CHECK( loaded.messages.empty() );
    BOOST_CHECK( wxFileName::Rmdir( root, wxPATH_RMDIR_RECURSIVE ) );
}


BOOST_AUTO_TEST_CASE( LoadsHistoryWhenToolSchemaMetadataIsCorrupted )
{
    const wxString root = wxFileName::CreateTempFileName( wxS( "kichad-thread-corrupt-" ) );
    BOOST_REQUIRE( wxRemoveFile( root ) );
    BOOST_REQUIRE( wxFileName::Mkdir( root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    const wxString index = wxFileName( root, wxS( "threads.json" ) ).GetFullPath();
    const wxString project = wxFileName( root, wxS( "project" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( project, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    wxFileName canonicalProject = wxFileName::DirName( project );
    canonicalProject.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    nlohmann::json corrupted = {
        { "version", 2 },
        { "projects",
          { { std::string( canonicalProject.GetFullPath().ToUTF8() ),
              { { "threadId", "thread-corrupt" }, { "toolSchemaVersion", "bad" } } } } }
    };
    wxFFile file( index, wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE( file.Write( wxString::FromUTF8( corrupted.dump() ), wxConvUTF8 ) );
    file.Close();

    CODEX_THREAD_STORE store( index );
    CODEX_THREAD_STORE::BINDING loaded;
    BOOST_CHECK_NO_THROW( loaded = store.Load( project ) );
    BOOST_CHECK_EQUAL( loaded.threadId, "thread-corrupt" );
    BOOST_CHECK_EQUAL( loaded.toolSchemaVersion, 0 );
    BOOST_CHECK( loaded.messages.empty() );
    BOOST_CHECK( wxFileName::Rmdir( root, wxPATH_RMDIR_RECURSIVE ) );
}


BOOST_AUTO_TEST_SUITE_END()
