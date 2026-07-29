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

#ifndef KICHAD_CODEX_AGENT_POLICY_H
#define KICHAD_CODEX_AGENT_POLICY_H

#include <nlohmann/json_fwd.hpp>


namespace KICHAD::CODEX_AGENT_POLICY
{

nlohmann::json ThreadConfig();
const char*    BaseInstructions();
const char*    DeveloperInstructions();
const char*    ExternalLayoutPolicy();

} // namespace KICHAD::CODEX_AGENT_POLICY

#endif // KICHAD_CODEX_AGENT_POLICY_H
