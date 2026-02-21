#pragma once

namespace oscproto
{
static constexpr const char* triggerAddress = "/sap/v1/trigger";
static constexpr const char* noteOnAddress = "/sap/v1/note_on";
static constexpr const char* noteOffAddress = "/sap/v1/note_off";
static constexpr const char* modAddress = "/sap/v1/mod";
static constexpr const char* transportAddress = "/sap/v1/transport";
static constexpr const char* tempoAddress = "/sap/v1/tempo";
static constexpr int defaultPort = 9001;
} // namespace oscproto
