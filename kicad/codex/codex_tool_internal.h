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

#ifndef KICHAD_CODEX_TOOL_INTERNAL_H
#define KICHAD_CODEX_TOOL_INTERNAL_H

#include <string>
#include <memory>
#include <set>
#include <array>
#include <filesystem>
#include <vector>

#include <import_export.h>
#include <api/board/board.pb.h>
#include <api/common/types/enums.pb.h>
#include <api/common/types/base_types.pb.h>
#include <api/common/types/project_settings.pb.h>
#include <google/protobuf/any.pb.h>
#include <google/protobuf/message.h>
#include <nlohmann/json.hpp>
#include <wx/filename.h>
#include <wx/string.h>

#include "design_script_pcb_reconciler.h"
#include "kicad_ipc_client.h"


namespace KICHAD::CODEX_TOOLS
{

/** Complete app-server specification owned by each native tool implementation. */
nlohmann::json ProjectSpec();
nlohmann::json InspectSpec();
nlohmann::json DesignSpec();
nlohmann::json PcbSpec();
nlohmann::json VerifySpec();
nlohmann::json FabricateSpec();
nlohmann::json LayoutSpec();

class PRIVATE_TEMPORARY_DIRECTORY
{
public:
    ~PRIVATE_TEMPORARY_DIRECTORY();

    bool Create( const std::string& aPrefix, std::string& aError );
    const std::filesystem::path& Path() const;

private:
    std::filesystem::path m_path;
};

/** Shared project-confinement primitive used by individual native tool implementations. */
bool ResolveProjectFile( const wxString& aProjectPath, const std::string& aRelativePath,
                         wxFileName& aResolved, std::string& aError );

/** Expected KiCad s-expression root for an inspectable native file extension. */
std::string ExpectedRootHead( const wxString& aExtension );

kiapi::common::types::KiCadObjectType PcbObjectType( const std::string& aItemType );
std::unique_ptr<google::protobuf::Message> NewPcbItem( const std::string& aItemType );
nlohmann::json DescribePcbMessage( const std::string& aItemType,
                                   const std::string& aMessagePath, std::string& aError );
bool ParsePcbItem( const std::string& aItemType, const nlohmann::json& aJson,
                   std::unique_ptr<google::protobuf::Message>& aMessage, std::string& aError );
std::string PcbItemId( const google::protobuf::Any& aItem, const std::string& aItemType );
std::string PcbAnyType( const google::protobuf::Any& aItem );

bool ResolveProjectLibraryTable( const wxString& aProjectPath, const std::string& aName,
                                 wxFileName& aResolved, std::string& aError );
bool ResolveProjectSchematic( const wxString& aProjectPath, const std::string& aRelativePath,
                              wxFileName& aResolved, std::string& aError );
bool ResolveProjectSidecar( const wxString& aProjectPath, const std::string& aRelativePath,
                            wxFileName& aResolved, std::string& aError );
bool ReadOptionalTextFile( const wxFileName& aPath, bool& aPresent, std::string& aSource,
                           std::string& aError );
bool InstallTextFileAtomically( const wxFileName& aPath, bool aPresent,
                                const std::string& aSource, std::string& aError );
bool ReadOptionalSchematic( const wxFileName& aPath, bool& aPresent, std::string& aSource,
                            std::string& aError );
bool InstallSchematicAtomically( const wxFileName& aPath, bool aPresent,
                                 const std::string& aSource, std::string& aError );
bool InventoryProjectSymbolLibraries( const wxString& aProjectPath,
                                      const nlohmann::json& aCompilerIr,
                                      nlohmann::json& aSources, std::string& aError );
bool InventoryProjectFootprints( const wxString& aProjectPath,
                                 const nlohmann::json& aCompilerIr,
                                 nlohmann::json& aSources, std::string& aError );
bool ValidateFootprintModelAssets( const wxString& aProjectPath,
                                   const nlohmann::json& aCompilerIr,
                                   nlohmann::json& aSummary, std::string& aError );
bool SchematicScreenUuid( const std::string& aSource, std::string& aUuid,
                          std::string& aError );
bool ValidateNativeSchematicHierarchy( const wxFileName& aRootSchematic,
                                       std::string& aError );
bool ValidateNativeSchematicHierarchy( const wxFileName& aRootSchematic,
                                       const nlohmann::json& aCompilerIr,
                                       std::string& aError );
bool ValidateNativeSchematicHierarchy( const wxFileName& aRootSchematic,
                                       const nlohmann::json& aCompilerIr,
                                       const nlohmann::json& aResolvedSymbols,
                                       std::string& aError );
bool ValidateProjectLibraryTable( const std::string& aKind, const std::string& aSource,
                                  size_t& aRows, std::string& aError );
bool ReadDesignScriptSidecar( const wxFileName& aFile, std::string& aSource,
                              std::string& aError );
bool InstallDesignScriptSidecarAtomically( const wxFileName& aFile,
                                           const std::string& aSource,
                                           std::string& aError );
bool ReadJsonFile( const wxFileName& aPath, nlohmann::json& aDocument, std::string& aError );
bool WriteJsonAtomically( const wxFileName& aPath, const nlohmann::json& aDocument,
                          std::string& aError );
bool MergeRecoveryJournal(
        const nlohmann::json& aJournal,
        const KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT& aContext,
        nlohmann::json& aPreviousState, std::string& aError );
bool QueryPcbInventory( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                        const std::set<std::string>& aIds, nlohmann::json& aInventory,
                        std::string& aError );
bool QueryPcbStackup( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                      kiapi::board::BoardStackup& aStackup, std::string& aError );
bool QueryPcbTitleBlock( const KICHAD_IPC_CLIENT& aClient,
                         const KICHAD_IPC_TARGET& aTarget,
                         kiapi::common::types::TitleBlockInfo& aTitleBlock,
                         std::string& aError );
bool UpdatePcbTitleBlock( const KICHAD_IPC_CLIENT& aClient,
                          const KICHAD_IPC_TARGET& aTarget,
                          const kiapi::common::types::TitleBlockInfo& aTitleBlock,
                          std::string& aError );
bool UpdatePcbStackup( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                       const kiapi::board::BoardStackup& aStackup, std::string& aError );
bool QueryPcbRules( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                    kiapi::board::BoardDesignRules& aRules, std::string& aError );
bool UpdatePcbRules( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                     const kiapi::board::BoardDesignRules& aRules, std::string& aError );
bool QueryPcbCustomRules( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                          bool& aPresent, std::string& aSource, std::string& aError );
bool UpdatePcbCustomRules( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                           bool aPresent, const std::string& aSource, std::string& aError );
bool QueryNetClassSettings( const KICHAD_IPC_CLIENT& aClient,
                            const KICHAD_IPC_TARGET& aTarget,
                            kiapi::common::project::NetClassSettings& aSettings,
                            std::string& aError );
bool UpdateNetClassSettings( const KICHAD_IPC_CLIENT& aClient,
                             const KICHAD_IPC_TARGET& aTarget,
                             const kiapi::common::project::NetClassSettings& aSettings,
                             std::string& aError );
bool QueryPcbFootprintInventory( const KICHAD_IPC_CLIENT& aClient,
                                 const KICHAD_IPC_TARGET& aTarget,
                                 const std::set<std::string>& aReferences,
                                 nlohmann::json& aInventory, std::string& aError );
bool RefillPcbZones( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                     const std::set<std::string>& aExpectedZoneIds, std::string& aError );
bool ExecutePcbActions( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                        const nlohmann::json& aActions,
                        const nlohmann::json& aFootprintSources, std::string& aError );
bool RunNativeKiCadCheck( const std::string& aCheck, const wxFileName& aInput,
                          std::string& aReport, std::string& aError );
bool RunNativeKiCadPreview( const std::string& aView, const wxFileName& aInput,
                            const std::vector<int>& aPages,
                            const std::vector<wxFileName>& aOutputs,
                            std::string& aError );
bool CanonicalizeExisting( wxFileName& aPath, bool aDirectory = false );
bool CreateFabricationVerificationSnapshot(
        const wxFileName& aProjectRoot, const wxFileName& aBoard,
        const wxFileName& aSchematic, const wxFileName& aSidecar,
        const std::array<std::string, 3>& aRelativePaths,
        const std::vector<std::string>& aAdditionalRelativePaths,
        PRIVATE_TEMPORARY_DIRECTORY& aSnapshot, std::string& aError );
nlohmann::json BuildFabricationPlan( const nlohmann::json& aIr,
                                     const std::string& aFileStem );
bool BuildNativeNetlistValidationPlan(
        const nlohmann::json& aIr, const nlohmann::json& aResolvedSymbols,
        const std::string& aFileStem, nlohmann::json& aPlan, std::string& aError );
bool RunNativeKiCadFabrication( const wxFileName& aBoard,
                                const wxFileName& aSchematic,
                                const nlohmann::json& aPlan,
                                const wxFileName& aStaging,
                                std::string& aError );
bool ValidateExactNativeFormat( const wxFileName& aPath, const std::string& aRoot,
                                const std::string& aVersion, std::string& aError );
bool ValidateBoardFabricationIntent( const wxFileName& aBoard, const nlohmann::json& aIr,
                                     const nlohmann::json& aPlan, std::string& aError );
bool WriteFabricationBom( const nlohmann::json& aIr, const wxFileName& aStaging,
                          const nlohmann::json& aPlan, size_t& aRows, std::string& aError );
bool ValidateFabricationArtifacts( const wxFileName& aStaging,
                                   const nlohmann::json& aPlan,
                                   nlohmann::json& aArtifacts, std::string& aError );
bool HashFabricationFile( const std::filesystem::path& aPath, std::string& aDigest );

} // namespace KICHAD::CODEX_TOOLS

#endif // KICHAD_CODEX_TOOL_INTERNAL_H
