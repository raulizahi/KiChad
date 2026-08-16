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

#include <kicad/codex/codex_app_server_client.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>


namespace
{

class ENVIRONMENT_GUARD
{
public:
    ENVIRONMENT_GUARD( const wxString& aName, const wxString& aValue ) :
            m_name( aName ),
            m_hadValue( wxGetEnv( aName, &m_value ) )
    {
        wxSetEnv( m_name, aValue );
    }

    ~ENVIRONMENT_GUARD()
    {
        if( m_hadValue )
            wxSetEnv( m_name, m_value );
        else
            wxUnsetEnv( m_name );
    }

private:
    wxString m_name;
    wxString m_value;
    bool     m_hadValue;
};


std::string readFileSnapshot( const wxString& aPath )
{
    std::ifstream input( aPath.ToStdString(), std::ios::binary );

    if( !input )
        return {};

    return { std::istreambuf_iterator<char>( input ), std::istreambuf_iterator<char>() };
}

} // namespace


BOOST_AUTO_TEST_SUITE( CodexAppServerClient )


BOOST_AUTO_TEST_CASE( DeliversLargeJsonRpcFrameAcrossPipeBackpressure )
{
    const wxString root = wxFileName::CreateTempFileName( wxS( "kichad-codex-pipe-" ) );
    BOOST_REQUIRE( wxRemoveFile( root ) );
    BOOST_REQUIRE( wxFileName::Mkdir( root, 0700, wxPATH_MKDIR_FULL ) );
    const wxString script = wxFileName( root, wxS( "slow-app-server" ) ).GetFullPath();
    const wxString capture = wxFileName( root, wxS( "captured.ndjson" ) ).GetFullPath();
    const wxString codexHome = wxFileName( root, wxS( "codex-home" ) ).GetFullPath();
    const wxString configHome = wxFileName( root, wxS( "config" ) ).GetFullPath();
    const char* scriptSource =
            "#!/usr/bin/env python3\n"
            "import os\n"
            "import sys\n"
            "import time\n"
            "time.sleep(0.5)\n"
            "with open(os.environ['KICHAD_CODEX_CAPTURE'], 'wb', buffering=0) as output:\n"
            "    while True:\n"
            "        line = sys.stdin.buffer.readline()\n"
            "        if not line:\n"
            "            break\n"
            "        output.write(line)\n";
    wxFFile scriptFile( script, wxS( "wb" ) );
    BOOST_REQUIRE( scriptFile.IsOpened() );
    BOOST_REQUIRE( scriptFile.Write( scriptSource, std::strlen( scriptSource ) ) );
    scriptFile.Close();
    BOOST_REQUIRE_EQUAL( wxChmod( script, 0700 ), 0 );

    ENVIRONMENT_GUARD executableGuard( wxS( "KICHAD_CODEX_EXECUTABLE" ), script );
    ENVIRONMENT_GUARD homeGuard( wxS( "KICHAD_CODEX_HOME" ), codexHome );
    ENVIRONMENT_GUARD captureGuard( wxS( "KICHAD_CODEX_CAPTURE" ), capture );
    ENVIRONMENT_GUARD configGuard( wxS( "XDG_CONFIG_HOME" ), configHome );
    CODEX_APP_SERVER_CLIENT client;
    bool reportedUnavailable = false;
    client.SetStateHandler(
            [&]( bool aRunning, const wxString& )
            {
                if( !aRunning )
                    reportedUnavailable = true;
            } );
    BOOST_REQUIRE( client.Start() );

    const std::string payload( 512 * 1024, 'K' );
    const nlohmann::json response = {
        { "id", 71 }, { "result", { { "payload", payload } } }
    };
    const std::string expected = response.dump() + "\n";
    BOOST_REQUIRE_GT( expected.size(), 64 * 1024 );
    BOOST_REQUIRE( client.SendResponse( 71, { { "payload", payload } } ) );

    bool captured = false;

    for( int attempt = 0; attempt < 120 && !captured; ++attempt )
    {
        wxMilliSleep( 25 );

        // A normal outbound notification also asks the client to drain any older queued frame.
        // The ordered queue guarantees these probes cannot overtake the large response.
        BOOST_REQUIRE( client.SendNotification( "transport/probe",
                                                { { "attempt", attempt } } ) );
        captured = wxFileName::FileExists( capture )
                   && wxFileName::GetSize( capture ) >= expected.size();
    }

    BOOST_REQUIRE_MESSAGE( captured, "slow app-server did not receive the complete JSON-RPC frame" );
    const std::string bytes = readFileSnapshot( capture );
    const size_t newline = bytes.find( '\n' );
    BOOST_REQUIRE_NE( newline, std::string::npos );
    BOOST_CHECK_EQUAL( bytes.substr( 0, newline + 1 ), expected );
    BOOST_CHECK( !reportedUnavailable );

    const std::string secret = "private-answer-must-not-enter-the-log";
    const nlohmann::json sensitiveResponse = {
        { "id", 72 }, { "result", { { "answers", { { "private", secret } } } } }
    };
    const std::string expectedSensitive = sensitiveResponse.dump() + "\n";
    BOOST_REQUIRE( client.SendResponse( 72, sensitiveResponse["result"], true ) );

    bool sensitiveCaptured = false;
    std::string capturedBytes;

    for( int attempt = 0; attempt < 80 && !sensitiveCaptured; ++attempt )
    {
        wxMilliSleep( 25 );
        capturedBytes = readFileSnapshot( capture );
        sensitiveCaptured = capturedBytes.find( expectedSensitive ) != std::string::npos;
    }

    BOOST_REQUIRE_MESSAGE( sensitiveCaptured,
                           "slow app-server did not receive the sensitive JSON-RPC frame" );
    BOOST_CHECK_NE( capturedBytes.find( expectedSensitive ), std::string::npos );

    const wxString diagnosticLog =
            wxFileName( wxFileName( configHome, wxS( "kichad" ) ).GetFullPath(),
                        wxS( "kichad-codex.log" ) ).GetFullPath();
    wxFFile logFile( diagnosticLog, wxS( "rb" ) );
    BOOST_REQUIRE( logFile.IsOpened() );
    wxString logText;
    BOOST_REQUIRE( logFile.ReadAll( &logText, wxConvUTF8 ) );
    BOOST_CHECK_EQUAL( std::string( logText.ToUTF8() ).find( secret ), std::string::npos );
    BOOST_CHECK_NE( std::string( logText.ToUTF8() ).find( "[REDACTED]" ), std::string::npos );

    client.Stop();
    BOOST_CHECK( wxFileName::Rmdir( root, wxPATH_RMDIR_RECURSIVE ) );
}


BOOST_AUTO_TEST_SUITE_END()
