#include "pch.h"
#include "ClipFile.h"
#include "ReplayClipTrainer.h"
#include <fstream>
#include <cctype>

//simple binary format: [magic][version][fields...][string-vectors][POD-struct-vectors].
//CarPosition/BallPosition/GameInfo are plain structs of primitives/Vector/Rotator, safe to dump as raw bytes.
static const uint32_t kClipFileMagic = 0x31524A4A; // "JJR1"

//bump this ONLY when the binary layout actually changes (CarPosition/BallPosition/GameInfo fields,
//or the field order in SaveFile/ReadFile below). This is independent from the plugin's own
//MAJOR.MINOR.PATCH version - most feature/UI changes don't touch this.
//reset to 1 for the first public release - the earlier 1/2 only ever existed on a dev machine, so
//there's no one else's saved clips to stay compatible with.
static const int32_t kClipFileFormatVersion = 1;

//sanity caps against corrupted/malicious files (clips may be shared between users) - generous enough
//for any realistic clip, but prevent a bogus length/count from trying to allocate gigabytes of memory
static const uint32_t kMaxStringLength = 1024;
static const uint32_t kMaxVectorCount = 500000;
static const int32_t kMaxLobbySize = 64;

static bool IsValidArenaName(const std::string& s) {
	if (s.empty() || s.size() > 256) { return false; }
	for (char c : s) {
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') { return false; }
	}
	return true;
}

static void WriteString(std::ofstream& out, const std::string& s) {
	uint32_t len = static_cast<uint32_t>(s.size());
	out.write(reinterpret_cast<const char*>(&len), sizeof(len));
	if (len > 0) { out.write(s.data(), len); }
}

static bool ReadString(std::ifstream& in, std::string& s) {
	uint32_t len = 0;
	in.read(reinterpret_cast<char*>(&len), sizeof(len));
	if (!in || len > kMaxStringLength) { return false; }
	s.resize(len);
	if (len > 0) { in.read(s.data(), len); }
	return static_cast<bool>(in) || len == 0;
}

static void WriteStringVector(std::ofstream& out, const std::vector<std::string>& vec) {
	uint32_t count = static_cast<uint32_t>(vec.size());
	out.write(reinterpret_cast<const char*>(&count), sizeof(count));
	for (const std::string& s : vec) { WriteString(out, s); }
}

static bool ReadStringVector(std::ifstream& in, std::vector<std::string>& vec) {
	uint32_t count = 0;
	in.read(reinterpret_cast<char*>(&count), sizeof(count));
	if (!in || count > kMaxVectorCount) { return false; }
	vec.resize(count);
	for (std::string& s : vec) { if (!ReadString(in, s)) { return false; } }
	return true;
}

template<typename T>
static void WritePodVector(std::ofstream& out, const std::vector<T>& vec) {
	uint32_t count = static_cast<uint32_t>(vec.size());
	out.write(reinterpret_cast<const char*>(&count), sizeof(count));
	if (count > 0) { out.write(reinterpret_cast<const char*>(vec.data()), count * sizeof(T)); }
}

template<typename T>
static bool ReadPodVector(std::ifstream& in, std::vector<T>& vec) {
	uint32_t count = 0;
	in.read(reinterpret_cast<char*>(&count), sizeof(count));
	if (!in || count > kMaxVectorCount) { return false; }
	vec.resize(count);
	if (count > 0) { in.read(reinterpret_cast<char*>(vec.data()), count * sizeof(T)); }
	return static_cast<bool>(in) || count == 0;
}

bool ClipFile::PeekIsCompatible(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) { return false; }

	uint32_t magic = 0;
	in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	if (!in || magic != kClipFileMagic) { return false; }

	int32_t version = 0;
	in.read(reinterpret_cast<char*>(&version), sizeof(version));
	if (!in || version != kClipFileFormatVersion) { return false; }

	return true;
}

ClipMetadata ClipFile::PeekMetadata(const std::filesystem::path& path) {
	ClipMetadata meta;

	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) { return meta; }

	uint32_t magic = 0;
	in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	if (!in || magic != kClipFileMagic) { return meta; }

	int32_t version = 0;
	in.read(reinterpret_cast<char*>(&version), sizeof(version));
	if (!in || version != kClipFileFormatVersion) { return meta; }

	meta.isCompatible = true;

	int32_t lobbySize = 0, gamemode = 0, replayFPS = 0, replaySize = 0, savedFrames = 0;
	in.read(reinterpret_cast<char*>(&lobbySize), sizeof(lobbySize));
	in.read(reinterpret_cast<char*>(&gamemode), sizeof(gamemode));
	in.read(reinterpret_cast<char*>(&replayFPS), sizeof(replayFPS));
	in.read(reinterpret_cast<char*>(&replaySize), sizeof(replaySize));
	in.read(reinterpret_cast<char*>(&savedFrames), sizeof(savedFrames));
	if (!in || lobbySize < 0 || lobbySize > kMaxLobbySize || savedFrames < 0) { return meta; }

	meta.lobbySize = lobbySize;
	return meta;
}

bool ClipFile::SaveFile(const std::filesystem::path& path, ReplayClipTrainer& plugin) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) { return false; }

	out.write(reinterpret_cast<const char*>(&kClipFileMagic), sizeof(kClipFileMagic));
	out.write(reinterpret_cast<const char*>(&kClipFileFormatVersion), sizeof(kClipFileFormatVersion));

	int32_t lobbySize = plugin.LobbySize;
	int32_t gamemode = plugin.Gamemode;
	int32_t replayFPS = plugin.ReplayFPS;
	int32_t replaySize = plugin.ReplaySize;
	int32_t savedFrames = plugin.ReplayTick;
	int32_t recordedSpectateIndex = plugin.RecordedSpectateIndex;
	out.write(reinterpret_cast<const char*>(&lobbySize), sizeof(lobbySize));
	out.write(reinterpret_cast<const char*>(&gamemode), sizeof(gamemode));
	out.write(reinterpret_cast<const char*>(&replayFPS), sizeof(replayFPS));
	out.write(reinterpret_cast<const char*>(&replaySize), sizeof(replaySize));
	out.write(reinterpret_cast<const char*>(&savedFrames), sizeof(savedFrames));
	out.write(reinterpret_cast<const char*>(&recordedSpectateIndex), sizeof(recordedSpectateIndex));

	WriteString(out, plugin.Arena);
	WriteStringVector(out, plugin.playerNames);
	WritePodVector(out, plugin.CarLayouts);
	WritePodVector(out, plugin.StartTeams);
	WritePodVector(out, plugin.KeyframesInConvert);

	WritePodVector(out, plugin.CarPositionsPerFrame);
	WritePodVector(out, plugin.BallPositionPerFrame);
	WritePodVector(out, plugin.GameInfoPerFrame);

	return out.good();
}

bool ClipFile::ReadFile(const std::filesystem::path& path, ReplayClipTrainer& plugin, std::string& outError) {
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) { outError = "file not found or can't be opened"; return false; }

	uint32_t magic = 0;
	in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	if (!in || magic != kClipFileMagic) { outError = "not a valid ReplayClipTrainer clip file"; return false; }

	int32_t version = 0;
	in.read(reinterpret_cast<char*>(&version), sizeof(version));
	if (!in || version != kClipFileFormatVersion) {
		outError = "this clip was saved by an incompatible version of ReplayClipTrainer and can't be loaded";
		return false;
	}

	int32_t lobbySize = 0, gamemode = 0, replayFPS = 0, replaySize = 0, savedFrames = 0, recordedSpectateIndex = 0;
	in.read(reinterpret_cast<char*>(&lobbySize), sizeof(lobbySize));
	in.read(reinterpret_cast<char*>(&gamemode), sizeof(gamemode));
	in.read(reinterpret_cast<char*>(&replayFPS), sizeof(replayFPS));
	in.read(reinterpret_cast<char*>(&replaySize), sizeof(replaySize));
	in.read(reinterpret_cast<char*>(&savedFrames), sizeof(savedFrames));
	in.read(reinterpret_cast<char*>(&recordedSpectateIndex), sizeof(recordedSpectateIndex));
	if (!in) { outError = "clip file is corrupted or truncated"; return false; }
	if (lobbySize < 0 || lobbySize > kMaxLobbySize || replayFPS < 0 || replaySize < 0 || savedFrames < 0) {
		outError = "clip file contains invalid data";
		return false;
	}
	//out-of-range index just falls back to 0 below rather than rejecting the whole file - cosmetic only

	std::string arena;
	if (!ReadString(in, arena)) { outError = "clip file is corrupted or truncated"; return false; }
	if (!IsValidArenaName(arena)) { outError = "clip file contains an invalid map name"; return false; }

	std::vector<std::string> playerNames;
	std::vector<int> carLayouts, startTeams, keyframesInConvert;
	std::vector<CarPosition> carPositionsPerFrame;
	std::vector<BallPosition> ballPositionPerFrame;
	std::vector<GameInfo> gameInfoPerFrame;

	if (!ReadStringVector(in, playerNames)) { outError = "clip file is corrupted or truncated"; return false; }
	if (!ReadPodVector(in, carLayouts)) { outError = "clip file is corrupted or truncated"; return false; }
	if (!ReadPodVector(in, startTeams)) { outError = "clip file is corrupted or truncated"; return false; }
	if (!ReadPodVector(in, keyframesInConvert)) { outError = "clip file is corrupted or truncated"; return false; }

	if (!ReadPodVector(in, carPositionsPerFrame)) { outError = "clip file is corrupted or truncated"; return false; }
	if (!ReadPodVector(in, ballPositionPerFrame)) { outError = "clip file is corrupted or truncated"; return false; }
	if (!ReadPodVector(in, gameInfoPerFrame)) { outError = "clip file is corrupted or truncated"; return false; }

	//cross-check sizes against the header fields before accepting - a mismatch here would otherwise
	//surface later as an out-of-range .at() during playback, which is uncaught and crashes the game
	size_t lobby = static_cast<size_t>(lobbySize);
	size_t frames = static_cast<size_t>(savedFrames);
	if (playerNames.size() != lobby || carLayouts.size() != lobby || startTeams.size() != lobby) {
		outError = "clip file contents are inconsistent or corrupted";
		return false;
	}
	if (ballPositionPerFrame.size() != frames || gameInfoPerFrame.size() != frames) {
		outError = "clip file contents are inconsistent or corrupted";
		return false;
	}
	if (carPositionsPerFrame.size() != frames * lobby) {
		outError = "clip file contents are inconsistent or corrupted";
		return false;
	}

	//teams are always binary in this game mode - clamp anything else instead of trusting the file
	for (int& team : startTeams) { team = (team == 0) ? 0 : 1; }
	if (recordedSpectateIndex < 0 || recordedSpectateIndex >= lobbySize) { recordedSpectateIndex = 0; }

	plugin.LobbySize = lobbySize;
	plugin.Gamemode = gamemode;
	plugin.ReplayFPS = replayFPS;
	plugin.ReplaySize = replaySize;
	plugin.ReplayTick = savedFrames;
	plugin.RecordedSpectateIndex = recordedSpectateIndex;
	plugin.Arena = arena;
	plugin.playerNames = std::move(playerNames);
	plugin.CarLayouts = std::move(carLayouts);
	plugin.StartTeams = std::move(startTeams);
	plugin.KeyframesInConvert = std::move(keyframesInConvert);
	plugin.CarPositionsPerFrame = std::move(carPositionsPerFrame);
	plugin.BallPositionPerFrame = std::move(ballPositionPerFrame);
	plugin.GameInfoPerFrame = std::move(gameInfoPerFrame);

	return true;
}
