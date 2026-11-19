#pragma once

#include <filesystem>
#include <string>

// Reads the current back buffer and writes it as PNG. Used by --screenshot for
// README images and for comparing renders against a reference image.
bool saveFramebufferPng(const std::filesystem::path& path, int width, int height, std::string& error);
