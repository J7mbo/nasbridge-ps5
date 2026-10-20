// NASBridge - The app's screens, as the UI kit's "designs".
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replaces the kit's src/concepts/concepts.hpp in the build: the kit's shell
// runs whatever registry.cpp lists. NASBridge has one screen for now: the library, which also holds the game page.

#pragma once

#include "app/concept.hpp"

#include <memory>

namespace hui::concepts
{

std::unique_ptr<app::Concept> make_library(app::Context &context);

} // namespace hui::concepts
