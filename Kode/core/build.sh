#!/bin/sh
# Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
# SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
# Builds the test CLI (kf_cli). The core uses its own thread helper (std::thread), so no OpenMP is needed.
cd "$(dirname "$0")" || exit 1
g++ -O3 -std=c++17 -Wall -Wextra kf_core.cpp kf_cli.cpp -o kf_cli -pthread && echo "built kf_cli"
