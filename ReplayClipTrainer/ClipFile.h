#pragma once
#include <filesystem>
#include <string>
class ReplayClipTrainer;

struct ClipMetadata
{
	bool isCompatible = false;
	int lobbySize = 0;
};

class ClipFile
{public:
	bool SaveFile(const std::filesystem::path& path, ReplayClipTrainer& plugin);
	bool ReadFile(const std::filesystem::path& path, ReplayClipTrainer& plugin, std::string& outError);

	//reads just the header (magic + format version) to cheaply check compatibility for list display,
	//without loading the full clip body
	static bool PeekIsCompatible(const std::filesystem::path& path);

	//same idea as PeekIsCompatible, but also grabs the handful of header fields useful for the saved
	//clips list (player count, clip length) - still cheap, stops reading before the big per-frame arrays
	static ClipMetadata PeekMetadata(const std::filesystem::path& path);
};
