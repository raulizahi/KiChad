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

#ifndef KICHAD_CODEX_TOOL_REGISTRY_H
#define KICHAD_CODEX_TOOL_REGISTRY_H

#include <chrono>
#include <functional>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>
#include <wx/filename.h>
#include <wx/string.h>


/** Registry for the bounded set of native dynamic tools advertised to Codex. */
class CODEX_TOOL_REGISTRY
{
public:
    using JSON = nlohmann::json;

    enum class RUNTIME_APPLICATION
    {
        PCB_EDITOR,
        SCHEMATIC_EDITOR
    };

    struct RUNTIME_DEPENDENCY
    {
        RUNTIME_APPLICATION application;
        wxFileName          document;
    };

    using RUNTIME_DEPENDENCY_RESOLVER =
            std::function<bool( const RUNTIME_DEPENDENCY&, std::string& )>;

    using NATIVE_CHECK_RUNNER = std::function<bool( const std::string&, const wxFileName&,
                                                     std::string&, std::string& )>;
    using NATIVE_FABRICATION_RUNNER =
            std::function<bool( const wxFileName&, const wxFileName&, const JSON&,
                                const wxFileName&, std::string& )>;
    using NATIVE_PREVIEW_RUNNER =
            std::function<bool( const std::string&, const wxFileName&,
                                const wxFileName&, int, std::string& )>;
    // Bump whenever the model-visible tool or capability contract changes so a project cannot
    // resume a persistent thread created with a broader or incompatible surface.
    static constexpr int SCHEMA_VERSION = 17;

    explicit CODEX_TOOL_REGISTRY( std::function<wxString()> aProjectPathProvider,
                                  std::function<bool()> aMutationGuard = {},
                                  std::function<wxString()> aIpcSocketDirectoryProvider = {},
                                  std::function<bool( const wxFileName&, std::string& )>
                                          aSchematicValidator = {},
                                  NATIVE_CHECK_RUNNER aNativeCheckRunner = {},
                                  NATIVE_FABRICATION_RUNNER aNativeFabricationRunner = {},
                                  std::function<bool( const wxFileName&, std::string& )>
                                          aSymbolLibraryValidator = {},
                                  std::function<bool( const wxFileName&, std::string& )>
                                          aFootprintLibraryValidator = {},
                                  NATIVE_PREVIEW_RUNNER aNativePreviewRunner = {} );

    JSON Specs() const;

    /// Configured external place-and-route executable; thread-safe because tool handlers run
    /// off the main thread while preferences update from it.
    void SetExternalLayoutTool( const wxString& aPath );
    wxString ExternalLayoutTool() const;
    void SetExternalLayoutLayers( int aLayers );
    int ExternalLayoutLayers() const;
    void SetExternalLayoutEnabled( bool aEnabled );
    bool ExternalLayoutEnabled() const;

    static bool RequiresFinalConfirmation( const std::string& aTool,
                                           const JSON& aArguments );
    JSON Handle( const std::string& aTool, const JSON& aArguments ) const;
    JSON HandleWithContext( const std::string& aTool, const JSON& aArguments,
                            const wxString& aProjectPath, bool aMutationAvailable,
                            const wxString& aIpcSocketDirectory = wxString(),
                            bool aFinalActionApproved = false,
                            std::chrono::milliseconds aIpcTimeout =
                                    std::chrono::milliseconds( 2000 ),
                            RUNTIME_DEPENDENCY_RESOLVER aDependencyResolver = {} ) const;

private:
    JSON handleProject( const JSON& aArguments, const wxString& aProjectPath,
                        bool aMutationAvailable ) const;
    JSON handleInspect( const JSON& aArguments, const wxString& aProjectPath ) const;
    JSON handleDesign( const JSON& aArguments, const wxString& aProjectPath,
                       bool aMutationAvailable, const wxString& aIpcSocketDirectory,
                       std::chrono::milliseconds aIpcTimeout,
                       const RUNTIME_DEPENDENCY_RESOLVER& aDependencyResolver ) const;
    JSON handlePcb( const JSON& aArguments, const wxString& aProjectPath,
                    bool aMutationAvailable, const wxString& aIpcSocketDirectory,
                    std::chrono::milliseconds aIpcTimeout,
                    const RUNTIME_DEPENDENCY_RESOLVER& aDependencyResolver ) const;
    JSON handleVerify( const JSON& aArguments, const wxString& aProjectPath ) const;
    JSON handleLayout( const JSON& aArguments, const wxString& aProjectPath,
                       bool aMutationAvailable ) const;

    mutable std::mutex m_externalLayoutToolMutex;
    wxString           m_externalLayoutTool;
    int                m_externalLayoutLayers = 2;
    bool               m_externalLayoutEnabled = false;
    JSON handleElectricalVerify( const JSON& aArguments,
                                 const wxString& aProjectPath ) const;
    JSON handleLayoutVerify( const JSON& aArguments, const wxString& aProjectPath ) const;
    JSON handleSourcingVerify( const JSON& aArguments, const wxString& aProjectPath ) const;
    JSON handleFabricate( const JSON& aArguments, const wxString& aProjectPath,
                          bool aMutationAvailable, bool aFinalActionApproved ) const;
    JSON success( const JSON& aPayload ) const;
    JSON failure( const std::string& aCode, const std::string& aMessage,
                  const JSON& aDetails = JSON::object() ) const;
    JSON addFailureContext( JSON aResult, const std::string& aTool,
                            const JSON& aArguments ) const;
    wxString projectPath() const;

    std::function<wxString()> m_projectPathProvider;
    std::function<bool()>     m_mutationGuard;
    std::function<wxString()> m_ipcSocketDirectoryProvider;
    std::function<bool( const wxFileName&, std::string& )> m_schematicValidator;
    NATIVE_CHECK_RUNNER m_nativeCheckRunner;
    NATIVE_FABRICATION_RUNNER m_nativeFabricationRunner;
    std::function<bool( const wxFileName&, std::string& )> m_symbolLibraryValidator;
    std::function<bool( const wxFileName&, std::string& )> m_footprintLibraryValidator;
    NATIVE_PREVIEW_RUNNER m_nativePreviewRunner;
};

#endif // KICHAD_CODEX_TOOL_REGISTRY_H
