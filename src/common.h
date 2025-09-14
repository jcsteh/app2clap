/*
 * App2Clap
 * Header for common code
 * Author: James Teh <jamie@jantrid.net>
 * Copyright 2025 James Teh
 * License: GNU General Public License version 2.0
 */

#pragma once

constexpr WORD NUM_CHANNELS = 2;
constexpr WORD BYTES_PER_FRAME = sizeof(float) * NUM_CHANNELS;
constexpr WORD BITS_PER_SAMPLE = sizeof(float) * 8;
constexpr REFERENCE_TIME REFTIMES_PER_SEC = 10000000;
