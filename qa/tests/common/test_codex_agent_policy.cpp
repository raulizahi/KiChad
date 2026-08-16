/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <boost/test/unit_test.hpp>

#include <codex/codex_agent_policy.h>

#include <nlohmann/json.hpp>

#include <string>


BOOST_AUTO_TEST_SUITE( CodexAgentPolicy )


BOOST_AUTO_TEST_CASE( ExposesOnlyRelevantGenericContext )
{
    const nlohmann::json config = KICHAD::CODEX_AGENT_POLICY::ThreadConfig();

    BOOST_CHECK( config.at( "features.goals" ).get<bool>() );
    BOOST_CHECK( config.at( "features.default_mode_request_user_input" ).get<bool>() );
    BOOST_CHECK( config.at( "tools.experimental_request_user_input.enabled" ).get<bool>() );
    BOOST_CHECK_EQUAL( config.at( "web_search" ).get<std::string>(), "live" );
    BOOST_CHECK( !config.at( "include_apps_instructions" ).get<bool>() );
    BOOST_CHECK( !config.at( "include_collaboration_mode_instructions" ).get<bool>() );
    BOOST_CHECK( !config.at( "include_environment_context" ).get<bool>() );
    BOOST_CHECK( !config.at( "include_permissions_instructions" ).get<bool>() );
    BOOST_CHECK_EQUAL( config.at( "project_doc_max_bytes" ).get<int>(), 0 );
}


BOOST_AUTO_TEST_CASE( KeepsSteeringCompactAndEngineeringSpecific )
{
    const std::string base = KICHAD::CODEX_AGENT_POLICY::BaseInstructions();
    const std::string developer = KICHAD::CODEX_AGENT_POLICY::DeveloperInstructions();

    BOOST_CHECK_LT( base.size(), 2200 );
    BOOST_CHECK_LT( developer.size(), 500 );
    BOOST_CHECK_NE( base.find( "electrical design engineer" ), std::string::npos );
    BOOST_CHECK_NE( base.find( "ERC and DRC do not prove electrical function" ),
                    std::string::npos );
    BOOST_CHECK_NE( base.find( "Never waive a gate without explicit user approval" ),
                    std::string::npos );
    BOOST_CHECK_NE( developer.find( "KDS is the sole authored design representation" ),
                    std::string::npos );

    for( const char* duplicatedToolDescription :
         { "design.context", "design.search", "design.read", "design.patch", "design.save",
           "design.apply", "inspect.render", "fabricate.plan", "fabricate.export" } )
    {
        BOOST_CHECK_EQUAL( base.find( duplicatedToolDescription ), std::string::npos );
        BOOST_CHECK_EQUAL( developer.find( duplicatedToolDescription ), std::string::npos );
    }
}


BOOST_AUTO_TEST_SUITE_END()
