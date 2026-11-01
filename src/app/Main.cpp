// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include <juce_core/juce_core.h>

#include "Version.h"

#include <cstring>
#include <iostream>

int main (int argc, char** argv)
{
    if (argc == 2 && std::strcmp (argv[1], "--version") == 0)
    {
        std::cout << "Rovox " << rovox::rovoxVersion() << " (" << rovox::rovoxGitSha() << ")\n"
                  << "JUCE " << juce::SystemStats::getJUCEVersion().toStdString() << "\n"
                  << "Tracktion Engine " << rovox::tracktionVersion() << "\n";
        return 0;
    }

    std::cerr << "usage: rovox --version\n";
    return 2;
}
