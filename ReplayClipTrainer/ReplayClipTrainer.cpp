#include "pch.h"
#include "ReplayClipTrainer.h"
#include "ClipFile.h"
#include "compatibility.h"
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <regex>
#include <sstream>
#include <map>
#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <cctype>
#pragma comment(lib, "Shell32.lib")

//converts a wide (UTF-16) unreal engine string into UTF-8, since CarWrapper::GetOwnerName() mangles non-ASCII characters (e.g. Japanese player names)
static std::string WStringToUtf8(const std::wstring& wstr) {
	if (wstr.empty()) { return std::string(); }
	int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
	std::string result(sizeNeeded, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), sizeNeeded, nullptr, nullptr);
	return result;
}

//converts UTF-8 (what ImGui text input gives us) into UTF-16, since passing a UTF-8 std::string
//straight into std::ofstream/std::filesystem would have Windows reinterpret it via the ANSI codepage
//instead - garbling or failing on non-ASCII (e.g. Japanese) clip names
static std::wstring Utf8ToWString(const std::string& utf8) {
	if (utf8.empty()) { return std::wstring(); }
	int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring result(sizeNeeded, 0);
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), sizeNeeded);
	return result;
}

//Windows reserved device names - creating a file with one of these names (with or without an extension) fails
static bool IsReservedDeviceName(const std::string& name) {
	std::string upper = name;
	for (char& c : upper) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
	static const std::vector<std::string> reserved = {
		"CON", "PRN", "AUX", "NUL",
		"COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
		"LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
	};
	for (const std::string& r : reserved) { if (upper == r) { return true; } }
	return false;
}

//formats a file's last-write time as "YYYY-MM-DD HH:MM" for the saved clips list. std::filesystem's
//file_time_type isn't directly convertible to std::time_t before C++20, so this uses the standard
//workaround of measuring its offset from file_time_type::clock::now() against system_clock::now().
static std::string FileTimeToString(const std::filesystem::path& path) {
	std::error_code ec;
	std::filesystem::file_time_type fileTime = std::filesystem::last_write_time(path, ec);
	if (ec) { return std::string(); }
	auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
		fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
	std::time_t timeT = std::chrono::system_clock::to_time_t(systemTime);
	std::tm localTm{};
	if (localtime_s(&localTm, &timeT) != 0) { return std::string(); }
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &localTm);
	return std::string(buffer);
}


//Manually expanded equivalent of BAKKESMOD_PLUGIN(ReplayClipTrainer, "...", plugin_version,
//PLUGINTYPE_FREEPLAY) (see bakkesmodplugin.h) instead of using the macro directly, so a fixed
//filename literal can be supplied instead of __FILE__. MSVC treats __FILE__ as a reserved macro
//and silently ignores any attempt to #undef/#define it (warning C4117), so overriding it isn't
//possible - without this, the macro bakes the builder's local absolute source path (including
//their Windows username) into the shipped DLL, visible to any user in BakkesMod's plugin-load
//console log.
static std::shared_ptr<ReplayClipTrainer> singleton;
extern "C" {
	BAKKESMOD_PLUGIN_EXPORT uintptr_t getPlugin()
	{
		if (!singleton) {
			singleton = std::shared_ptr<ReplayClipTrainer>(new ReplayClipTrainer());
		}
		return reinterpret_cast<std::uintptr_t>(&singleton);
	}
	BAKKESMOD_PLUGIN_EXPORT void deleteMe() {
		if (singleton)
			singleton = nullptr;
	}
	BAKKESMOD_PLUGIN_EXPORT BakkesMod::Plugin::PluginInfo exports =
	{
		BAKKESMOD_PLUGIN_API_VERSION,
		"ReplayClipTrainer.cpp",
		"ReplayClipTrainer",
		"ReplayClipTrainer is a bakkesmod Plugin which allows you to open replays in a private match and take control of any car in any situation",
		plugin_version,
		PLUGINTYPE_FREEPLAY,
		getPlugin,
		deleteMe
	};
}

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

void ReplayClipTrainer::onLoad() {
	//Compatibility compatibility;
	_globalCvarManager = cvarManager;
	this->Log("ReplayClipTrainer is a bakkesmod Plugin which allows you to record your replays and open them in Freeplay so you can take controll over a car at any time");
	this->CvarRegister();
	this->AutoConvert();
	this->RefreshSavedClipsList();


	//rendering
	gameWrapper->RegisterDrawable(bind(&ReplayClipTrainer::DrawHud, this, std::placeholders::_1));

	return;
}

void ReplayClipTrainer::onUnload() {
	gameWrapper->UnhookEvent("Function TAGame.Replay_TA.EventPostTimeSkip");
	return;
}


//Register All Cvars

void ReplayClipTrainer::CvarRegister() {

	cvarManager->registerCvar("reclip_replaySave", "0", "Starts saving the replay for private match", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::SaveReplay, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_privateMatch", "0", "Starts a private Match", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::StartPrivateMatch, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_botSpawn", "0", "Spawns right amount of bots", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::SpawnBots, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_controllGame", "0", "replays the replay as ReplayClipTrainer", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::ControllGame, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_switchPlayer", "0", "switches the player you are spectating", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::SwitchCars, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_switchBack", "0", "switches back to the player you where previosly spectating", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::SwitchBack, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_jumpIn", "0", "lets the player take controll of current spectated car", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::JumpIn, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_skip", "0", "skips given amount of seconds", true, true, -2000.0f, true, 2000.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::Skip, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_bindings", "0", "binds standard binds", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::Bindings, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_convert", "0", "converts replays into ReplayClipTrainers", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::Convert, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_convertClip", "0", "converts only a short clip around the current replay scrub position (see reclip_clipSecondsBefore/reclip_clipSecondsAfter)", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::ConvertClip, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_openReplay", "0", "opens saved ReplayClipTrainer", true, true, 0.0f, true, 1.0f, false).addOnValueChanged(std::bind(&ReplayClipTrainer::OpenReplay, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerCvar("reclip_autoConvert", "0", "automaticly converts replays into ReplayClipTrainers", true, true, 0.0f, true, 1.0f, true).addOnValueChanged(std::bind(&ReplayClipTrainer::AutoConvertCvar, this, std::placeholders::_1, std::placeholders::_2));
	cvarManager->registerNotifier("reclip_saveClip", [this](std::vector<std::string> params) {
		//params[0] is the command name itself; anything after that is the (optional) clip name, like "Save for Later" in the GUI - joined back together so a multi-word name doesn't need quoting
		std::string name;
		for (size_t i = 1; i < params.size(); i++) {
			if (i > 1) { name += " "; }
			name += params[i];
		}
		gameWrapper->SetTimeout([this, name](GameWrapper*) { this->ConvertClipToFile(name); }, 0.0f);
	}, "saves a clip around the current replay scrub position to file, like \"Save for Later\" in the GUI (name optional, auto-named like the GUI if omitted)", PERMISSION_ALL);

	cvarManager->registerCvar("reclip_doNotAskForDisableOfIncompatiblePlugins", "0", "asks for disableing plugins by ReplayClipTrainer", true, true, 0.0f, true, 1.0f, true);
	
	cvarManager->registerCvar("reclip_pause", "0", "pauses replay", true, true, 0.0f, true, 1.0f, false);
	cvarManager->registerCvar("reclip_inputToUnpause", "1", "unpauses replay when jumpedIn via controller input", true, true, 0.0f, true, 1.0f, true);
	cvarManager->registerCvar("reclip_resolution", "0.5", "resolution how the replay is converted (lower better Resolution->slower Conversion)", true, true, 0.01f, true, 20.0f, true);
	cvarManager->registerCvar("reclip_limitedBoost", "1", "limits boost in ReplayClipTrainers", true, true, 0.0f, true, 1.0f, true);
	cvarManager->registerCvar("reclip_showHud", "1", "shows hud in ReplayClipTrainers", true, true, 0.0f, true, 1.0f, true);
	cvarManager->registerCvar("reclip_convertKeyframes", "0", "only converts the replay for the keyframe sequences", true, true, 0.0f, true, 1.0f, true);
	cvarManager->registerCvar("reclip_timeBeforKeyframe", "20", "time that is converted before the keyframe", true, true, 0.0f, true, 1200.0f, true);
	cvarManager->registerCvar("reclip_timeAfterKeyframe", "5", "time  that is converted after the keyframe", true, true, 0.0f, true, 1200.0f, true);
	cvarManager->registerCvar("reclip_clipSecondsBefore", "2", "for reclip_convertClip: seconds before the current replay position to start converting", true, true, 0.0f, true, 1200.0f, true);
	cvarManager->registerCvar("reclip_clipSecondsAfter", "5", "for reclip_convertClip: seconds after the current replay position to stop converting", true, true, 0.0f, true, 1200.0f, true);
	cvarManager->registerCvar("reclip_disableGoal", "1", "disables the goal", false, true, 0.0f, true, 1.0f, true);
	cvarManager->registerCvar("reclip_ballFreePhysics", "0", "lets the ball go free to real physics as soon as you resume from pause, instead of following the recorded path", true, true, 0.0f, true, 1.0f, false);
	return;
}

//--------//
//FUNCTION//
//--------//

//Save Replay As ReplayClipTrainer

void ReplayClipTrainer::SaveReplay(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		if (gameWrapper->IsInReplay() == true) {
			if (cvar.getIntValue() == 1) {
				//check for plugins that are incompatible with replayClipTrainer
				//Compatibility compatibility;
				if (compatibility.CheckForIncompatibility()) {

					ReplayWrapper Replay = gameWrapper->GetGameEventAsReplay().GetReplay();
					ReplayServerWrapper ServerReplay = gameWrapper->GetGameEventAsReplay();
					ReplaySize = Replay.GetNumFrames();

					//get rid of old replay
					playerNames.clear();
					CarLayouts.clear();
					//StartCars.clear();
					StartTeams.clear();
					CarPositionsPerFrame.clear();
					BallPositionPerFrame.clear();
					primeColor.clear();
					//secondColor.clear();
					BlueScoredFrame.clear();
					OrangeScoredFrame.clear();
					GameInfoPerFrame.clear();
					Gamemode = 0;
					GamemodeStr = "";
					AdditionalMutators = "";

					//save new replay
					ReplayTick = 0;
					//Frame = 0;

					this->OneTimeSaves();

					if (useClipRange == false) {
						ConvertStartFrame = 0;
						ConvertEndFrame = ReplaySize - 1;
					}
					if (ConvertEndFrame <= 0 || ConvertEndFrame > ReplaySize - 1) { ConvertEndFrame = ReplaySize - 1; }
					if (ConvertStartFrame < 0 || ConvertStartFrame >= ReplaySize) { ConvertStartFrame = 0; }
					useClipRange = false;

					gameWrapper->GetGameEventAsReplay().SkipToFrame(ConvertStartFrame);

					gameWrapper->HookEvent("Function TAGame.EngineShare_TA.EventPostPhysicsStep", std::bind(&ReplayClipTrainer::SaveGameState, this, std::placeholders::_1));

					this->Log("Converting frames " + std::to_string(ConvertStartFrame) + " to " + std::to_string(ConvertEndFrame) + " (Total Frames in Replay " + std::to_string(ReplaySize) + ")");
				}
			}
			else {
				gameWrapper->UnhookEvent("Function TAGame.EngineShare_TA.EventPostPhysicsStep");
				gameWrapper->GetGameEventAsReplay().SetGameSpeed(1.0f);
			}
				
		}
		else {
			gameWrapper->UnhookEvent("Function TAGame.EngineShare_TA.EventPostPhysicsStep");
			gameWrapper->GetGameEventAsReplay().SetGameSpeed(1.0f);
			Log("you must be in a replay to execute this command");
			cvar.setValue(false);
		}
	}
	return;
}

void ReplayClipTrainer::SaveGameState(std::string eventName) {
	if (cvarManager->getCvar("reclip_replaySave").getBoolValue() == true) {
		if (gameWrapper->IsInReplay() == true) {
			if (Keyframes.size() > 0) {
				if (curKeyframeInConvert < Keyframes.size() && gameWrapper->GetGameEventAsReplay().GetCurrentReplayFrame() >= Keyframes.at(curKeyframeInConvert)) {
					KeyframesInConvert.push_back(ReplayTick);
					curKeyframeInConvert++;
				}
				if (cvarManager->getCvar("reclip_convertKeyframes").getBoolValue() == true) {
					int frameStartKeyframe = Keyframes.at(curKeyframe) - (cvarManager->getCvar("reclip_timeBeforKeyframe").getIntValue() * gameWrapper->GetGameEventAsReplay().GetReplayFPS());
					//if (frameStartKeyframe < 0) { frameStartKeyframe = 0; }
					int frameEndKeyframe = Keyframes.at(curKeyframe) + (cvarManager->getCvar("reclip_timeAfterKeyframe").getIntValue() * gameWrapper->GetGameEventAsReplay().GetReplayFPS());
					if (gameWrapper->GetGameEventAsReplay().GetCurrentReplayFrame() < frameStartKeyframe) {
						gameWrapper->GetGameEventAsReplay().SetGameSpeed(500000);
						//Log("frameStartKeyFrame= " + std::to_string(frameStartKeyframe));
						//Log("Frame= " + std::to_string(gameWrapper->GetGameEventAsReplay().GetCurrentReplayFrame()));
						return;
					}
					if (gameWrapper->GetGameEventAsReplay().GetCurrentReplayFrame() > frameEndKeyframe) {
						curKeyframe++;
						if (curKeyframe >= Keyframes.size()) {
							//ReplaySize = gameWrapper->GetGameEventAsReplay().GetCurrentReplayFrame();
						}
					}
				}
			}

			if (gameWrapper->GetGameEventAsReplay().GetGameSpeed() != cvarManager->getCvar("reclip_resolution").getFloatValue()) {
				gameWrapper->GetGameEventAsReplay().SetGameSpeed(cvarManager->getCvar("reclip_resolution").getFloatValue());
			}

			//Save time and score
			SaveGameInfo();

			//BallSave
			if (gameWrapper->GetGameEventAsReplay().GetGameBalls().Count() < 1) {
				SaveBall0();
			}
			else {
				SaveBall();
			}

			ArrayWrapper<CarWrapper> Cars = gameWrapper->GetGameEventAsReplay().GetCars();

			/*//get new cars
			for (int k = 0;k < Cars.Count();k++) {
				bool alreadySaved = false;
				for (int i = 0;i < playerNames.size();i++) {
					if (playerNames.at(i) == Cars.Get(k).GetOwnerName()) { alreadySaved = true; }
				}
				if (alreadySaved == false) { 
					playerNames.push_back(Cars.Get(k).GetOwnerName());
					newCars++;
				}
			}*/

			//CarSave

			for (int k = 0;k < LobbySize;k++) {
				int i = 0;
				//bool quit = false;
				while (i < Cars.Count() && playerNames.at(k) != Cars.Get(i).GetOwnerName()) {
					i++;
				}
				if (i<Cars.Count()) {
					SaveCar(i);
				}
				else {
					SaveCar0();
				}
			}

			//Save Positions in txt file
			//SavePositionsTxt();

			ReplayTick++;

			if (gameWrapper->GetGameEventAsReplay().GetCurrentReplayFrame() >= ConvertEndFrame || (curKeyframe >= Keyframes.size()&& Keyframes.size() > 0)) {
				gameWrapper->UnhookEvent("Function TAGame.EngineShare_TA.EventPostPhysicsStep");
				this->Log("is unhooked");
				gameWrapper->GetGameEventAsReplay().SetGameSpeed(1.0f);
				if (pendingSaveToFile) {
					pendingSaveToFile = false;
					std::string nameToSave = pendingSaveName;
					gameWrapper->SetTimeout([this, nameToSave](GameWrapper*) { this->SaveCurrentClipToFile(nameToSave); }, 0.0f);
				}
				if (cvarManager->getCvar("reclip_convert").getBoolValue() == true) {
					cvarManager->executeCommand("reclip_privateMatch 1");
				}
				cvarManager->getCvar("reclip_replaySave").setValue(false);
			}
		}
		else {
			gameWrapper->UnhookEvent("Function TAGame.EngineShare_TA.EventPostPhysicsStep");
			this->Log("is unhooked");
			//gameWrapper->GetGameEventAsReplay().SetGameSpeed(1.0f);
			if (cvarManager->getCvar("reclip_convert").getBoolValue() == true) {
				cvarManager->getCvar("reclip_convert").setValue(false);
			}
			cvarManager->getCvar("reclip_replaySave").setValue(false);
		}
	}

	return;
}

//stops an in-progress conversion. Just setting reclip_replaySave to false isn't enough - SaveGameState()'s
//own top-level check is gated on that same cvar being true, so it would silently no-op on the next tick
//without ever unhooking itself or restoring the replay's playback speed. This does that cleanup explicitly.
void ReplayClipTrainer::CancelConversion() {
	gameWrapper->UnhookEvent("Function TAGame.EngineShare_TA.EventPostPhysicsStep");
	if (gameWrapper->IsInReplay()) {
		gameWrapper->GetGameEventAsReplay().SetGameSpeed(1.0f);
	}
	pendingSaveToFile = false;
	useClipRange = false;
	cvarManager->getCvar("reclip_replaySave").setValue(false);
	cvarManager->getCvar("reclip_convert").setValue(false);
	this->Log("Conversion cancelled");
}

void ReplayClipTrainer::SaveCar(int i) {
	CarWrapper Car = gameWrapper->GetGameEventAsReplay().GetCars().Get(i);
	CarPosition CarTemp;
	CarTemp.location = Car.GetLocation();
	CarTemp.velocitiy = Car.GetVelocity();
	CarTemp.rotation = Car.GetRotation();
	CarTemp.angVelocity = Car.GetAngularVelocity();
	CarTemp.steer = Car.GetReplicatedSteer();
	CarTemp.throttle = Car.GetReplicatedThrottle();
	CarTemp.handbrake = Car.GetbReplicatedHandbrake();
	//GetReplicatedBoostAmount() always reads back 0 from a REPLAY's CarWrapper, so use the live-style
	//GetCurrentBoostAmount() (0-100 scale) instead - this is the recorded/actual boost value.
	float rawBoostAmount = Car.GetBoostComponent().IsNull() ? 0.0f : Car.GetBoostComponent().GetCurrentBoostAmount();
	if (rawBoostAmount < 0.0f) { rawBoostAmount = 0.0f; }
	if (rawBoostAmount > 100.0f) { rawBoostAmount = 100.0f; }
	CarTemp.boostAmount = rawBoostAmount;
	CarTemp.isBoosting = Car.IsBoostCheap();
	//CarTemp.lastJumped = !Car.GetbJumped() || Car.GetJumpComponent().IsNull() ? -1 : Car.GetJumpComponent().GetInactiveTime();
	//CarTemp.hasDodge = !Car.GetbDoubleJumped() && CarTemp.lastJumped < 1.2f;
	CarTemp.ballTouches = Car.GetPRI().IsNull() ? 0 : Car.GetPRI().GetBallTouches();
	CarTemp.team = Car.GetTeamNum2();
	CarTemp.isActive = true;
	//CarTemp.name = Car.GetOwnerName();
	CarPositionsPerFrame.push_back(CarTemp);

	//Log(std::to_string(i) + ": " + std::to_string(Car.GetReplicatedSteer()));
}

void ReplayClipTrainer::SaveCar0() {
	CarPosition CarTemp1;
	CarTemp1.location = Vector(0, 0, 2200);
	CarTemp1.velocitiy = Vector(0, 0, 0);
	CarTemp1.rotation = Rotator(0, 0, 0);
	CarTemp1.angVelocity = Vector(0, 0, 0);
	CarTemp1.steer = 0.0f;
	CarTemp1.throttle = 0.0f;
	CarTemp1.handbrake = false;
	CarTemp1.boostAmount = 0.0f;
	CarTemp1.isBoosting = false;
	//CarTemp1.hasDodge = false;
	CarTemp1.ballTouches = 0;
	CarTemp1.team = 0;
	/*if (ReplayTick - 1 <= 0) { CarTemp1.name = ""; }
	else { CarTemp1.name = CarPositionsPerFrame.at(ReplayTick - 1).name; }*/
	CarTemp1.isActive = false;
	CarPositionsPerFrame.push_back(CarTemp1);
}

void ReplayClipTrainer::SaveBall() {
	BallWrapper Ball = gameWrapper->GetGameEventAsReplay().GetBall();
	BallPosition BallTemp;
	BallTemp.location = Ball.GetLocation();
	BallTemp.velocitiy = Ball.GetVelocity();
	BallTemp.rotation = Ball.GetRotation();
	BallTemp.angVelocity = Ball.GetAngularVelocity();
	BallPositionPerFrame.push_back(BallTemp);
}

void ReplayClipTrainer::SaveBall0() {
	BallPosition BallTemp;
	BallTemp.location = Vector(0, 0, 0);
	BallTemp.velocitiy = Vector(0, 0, 0);
	BallTemp.rotation = Rotator(0, 0, 0);
	BallTemp.angVelocity = Vector(0, 0, 0);
	BallPositionPerFrame.push_back(BallTemp);
}

void ReplayClipTrainer::SaveGameInfo() {
	ReplayServerWrapper Game = gameWrapper->GetGameEventAsReplay();
	GameInfo GameInfoTemp;
	GameInfoTemp.BlueScore = Game.GetTeams().Get(0).GetScore();
	GameInfoTemp.OrangeScore = Game.GetTeams().Get(1).GetScore();
	GameInfoTemp.Countdowntime = Game.GetReplicatedRoundCountDownNumber();
	GameInfoTemp.TimeRemaining = Game.GetSecondsRemaining();
	GameInfoTemp.FrameInReplay = Game.GetCurrentReplayFrame();
	GameInfoPerFrame.push_back(GameInfoTemp);
}

//only for log test (laggs out if enabled)
void ReplayClipTrainer::SavePositionsTxt() {
	positionstxt = positionstxt + "Frame: " + std::to_string(ReplayTick) + "\n";
	positionstxt = positionstxt + "Ball" + " X: " + std::to_string(BallPositionPerFrame.at(ReplayTick).location.X) + " Y: " + std::to_string(BallPositionPerFrame.at(ReplayTick).location.Y) + " Z: " + std::to_string(BallPositionPerFrame.at(ReplayTick).location.Z) + "\n";
	for (int i = 0;i < playerNames.size();i++) {
		positionstxt = positionstxt + playerNames.at(i) + ": Index: " + std::to_string(i) + " X: " + std::to_string(CarPositionsPerFrame.at(ReplayTick * i).location.X) + " Y: " + std::to_string(CarPositionsPerFrame.at(ReplayTick * i).location.Y) + " Z: " + std::to_string(CarPositionsPerFrame.at(ReplayTick * i).location.Z) + "\n";
	}
	positionstxt = positionstxt + "\n";
}

void ReplayClipTrainer::OneTimeSaves() {
	if (gameWrapper->IsInReplay() == true) {
		ReplayServerWrapper Replay = gameWrapper->GetGameEventAsReplay();
		//general saves
		LobbySize = Replay.GetCars().Count();
		Arena = gameWrapper->GetCurrentMap();
		Gamemode = gameWrapper->GetGameEventAsReplay().GetPlaylist().GetPlaylistId();

		//Keyframes save
		temppath = gameWrapper->GetBakkesModPath().string() + "/data/replayClipTrainer/current.replay";
		this->Log("temporary replay saved to: " + temppath);
		Replay.GetReplay().ExportReplay(temppath);
		this->KeyframeSaves();
		ReplayFPS = gameWrapper->GetGameEventAsReplay().GetReplayFPS();

		LinearColor orange = gameWrapper->GetGameEventAsReplay().GetTeams().Get(1).GetPrimaryColor();
		LinearColor blue = gameWrapper->GetGameEventAsReplay().GetTeams().Get(0).GetPrimaryColor();

		//player specific saves
		for (int i = 0;i < LobbySize;i++) {
			CarWrapper carX = Replay.GetCars().Get(i);
			if (carX.IsNull()) {
				//car not valid at this replay frame (e.g. a player who left mid-match) - use safe defaults
				playerNames.push_back("");
				CarLayouts.push_back(0);
				StartTeams.push_back(0);
				primeColor.push_back(blue);
				continue;
			}
			PriWrapper pri = carX.GetPRI();
			playerNames.push_back(pri.IsNull() ? carX.GetOwnerName() : WStringToUtf8(pri.GetPlayerName().ToWideString()));
			//StartCars.push_back(carX.GetbLoadoutSet());
			CarLayouts.push_back(carX.GetLoadoutBody());
			StartTeams.push_back(carX.GetTeamNum2());
			//save color
			if (StartTeams.at(i) == 0) {
				primeColor.push_back(blue);
				//secondColor.push_back(blue);
			}
			else {
				primeColor.push_back(orange);
				//secondColor.push_back(orange);
			}
		}

		//remember who was being spectated in the original replay, so playback can default to that
		//player instead of always starting on index 0 (see RecordedSpectateIndex comment in the header)
		RecordedSpectateIndex = 0;
		ActorWrapper viewTarget = Replay.GetViewTarget();
		if (!viewTarget.IsNull()) {
			for (int i = 0;i < LobbySize;i++) {
				if (Replay.GetCars().Get(i).memory_address == viewTarget.memory_address) {
					RecordedSpectateIndex = i;
					break;
				}
			}
		}

		hasReplaySaved = true;
		this->Log("OneTimeSaves got saved");
	}
}


void ReplayClipTrainer::KeyframeSaves() {
	this->Keyframes.clear();
	this->KeyframesInConvert.clear();
	curKeyframe = 0;
	curKeyframeInConvert = 0;
	//gameWrapper->GetGameEventAsReplay().GetReplayDirector().SaveUserKeyframe();
	//open temp file
	std::string ReplayAsString;
	std::ifstream ReplayFile(temppath,std::ios_base::out | std::ios_base::binary);
	if (!ReplayFile.is_open()) { this->Log("can't find temp file"); return; }
	std::stringstream buffer;
	buffer << ReplayFile.rdbuf();
	ReplayAsString = buffer.str();
	//convert to hex
	std::stringstream ss;
	ss << std::hex << std::setfill('0');
	for (size_t i = 0; ReplayAsString.length() > i; ++i) {
		ss << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(ReplayAsString[i]));
	}
	std::string ReplayAsHex = ss.str();
	//search for keyframes
	std::vector<std::string> KeyframesInHex;
	std::size_t stringpos = 0;
	while (stringpos <= ReplayAsHex.size()) {
		stringpos = ReplayAsHex.find("005573657200", stringpos + 1);
		if (stringpos != std::string::npos) {
			KeyframesInHex.push_back(ReplayAsHex.substr(stringpos+12, 4));
		}
		else {
			this->Log("no more keyframes found");
			break;
		}
	}
	//convert to int
	for (int k = 0;k<KeyframesInHex.size();k++) {
		//small endian to big endian
		std::string SmallEndian = KeyframesInHex.at(k);
		KeyframesInHex.at(k) = SmallEndian.substr(2, 2) + SmallEndian.substr(0, 2);
		//intinfy
		//handle zero case
		if (std::stoul(KeyframesInHex.at(k), nullptr, 16) > 65500) {
			Keyframes.push_back(1);
		}
		else {
			Keyframes.push_back(std::stoul(KeyframesInHex.at(k), nullptr, 16));
		}
	}
	return;
}

//Start Private Match

void ReplayClipTrainer::StartPrivateMatch(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		if (hasReplaySaved == true) {
			//shown by RenderPreparingOverlay() until ControllGame() actually starts playback
			isPreparingSession = true;
			//sets gamemode
			if (Gamemode < 14 && Gamemode>0) { GamemodeStr = "Soccar"; AdditionalMutators = ""; }
			else if(Gamemode==16 || Gamemode==22 || Gamemode==34 || Gamemode==35){ GamemodeStr = "Soccar"; AdditionalMutators = ""; }//normal
			else if (Gamemode == 17 || Gamemode == 27) { GamemodeStr = "Basketball"; AdditionalMutators = ""; }//hoops
			else if (Gamemode == 15 || Gamemode == 30) { GamemodeStr = "Hockey"; AdditionalMutators = ""; }//snowday
			else if (Gamemode == 46) { GamemodeStr = "Football"; AdditionalMutators = ""; }//american football
			else if (Gamemode == 38 || Gamemode == 43) { GamemodeStr = "GodBall"; AdditionalMutators = ""; }//heatseaker
			//not fully implemnted yet
			else if (Gamemode == 18 || Gamemode == 28) { GamemodeStr = "Soccar"; AdditionalMutators = ""; }//rumble
			else if (Gamemode == 29) { GamemodeStr = "Breakout"; AdditionalMutators = ""; }//dropshot
			else if (Gamemode == 37) { GamemodeStr = "Breakout"; AdditionalMutators = ""; }//dropshot rumble
			//not implemented yet
			else if (Gamemode == 41) { GamemodeStr = "Soccar"; AdditionalMutators = ""; }//boomer
			else if (Gamemode == 47) { GamemodeStr = "Soccar"; AdditionalMutators = ""; }//super cube
			else if (Gamemode == 33) { GamemodeStr = "Soccar"; AdditionalMutators = ""; }//spike rush
			else if (Gamemode == 31) { GamemodeStr = "Soccar"; AdditionalMutators = ""; }//ghost hunt
			else if (Gamemode == 44) { GamemodeStr = "Soccar"; AdditionalMutators = ""; }//winter brakeaway
			else if (Gamemode == 32) { GamemodeStr = "Soccar"; AdditionalMutators = ""; }//beach ball
			else{ GamemodeStr = "Soccar"; AdditionalMutators = ""; }//normal
			//enable goal?
			if (cvarManager->getCvar("reclip_disableGoal").getBoolValue() == true) {
				AdditionalMutators = AdditionalMutators + ",DisableGoalDelay";
			}
			//opens private match
			//gameWrapper->GetGameEventAsReplay().GetReplayDirector().SaveUserKeyframe();

			//cvarManager->executeCommand("unreal_command disconnect");
			//gameWrapper->HookEvent("Function TAGame.GFxData_MainMenu_TA.MainMenuAdded", std::bind(&ReplayClipTrainer::JoinPrivateMatch, this, std::placeholders::_1));
			cvarManager->executeCommand("unreal_command 'open " + Arena + "?Game=TAGame.GameInfo_" + GamemodeStr + "_TA?Offline?Gametags=noDemolish,UnlimitedTime" + AdditionalMutators + "'");

			//give saved positions back per frame
			//std::string positionstxtpath = gameWrapper->GetBakkesModPath().string() + "/data/replayClipTrainer/positions.txt";
			//std::ofstream positionstxtout(positionstxtpath);
			//positionstxtout << positionstxt;
			//positionstxtout.close();

		}
		cvar.setValue(false);
	}
}

void ReplayClipTrainer::JoinPrivateMatch(std::string eventName) {
	cvarManager->executeCommand("unreal_command 'open " + Arena + "?Game=TAGame.GameInfo_" + GamemodeStr + "_TA?Offline?Gametags=noDemolish,UnlimitedTime" + AdditionalMutators + "'");
	gameWrapper->UnhookEvent("Function TAGame.GFxData_MainMenu_TA.MainMenuAdded");
}


//Bot Spawning

void ReplayClipTrainer::SpawnBots(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		if (gameWrapper->IsInGame() == true && hasReplaySaved == true && gameWrapper->IsInFreeplay() == false) {
			this->Log(std::to_string(LobbySize));
			carit = (RecordedSpectateIndex >= 0 && RecordedSpectateIndex < LobbySize) ? RecordedSpectateIndex : 0;
			SavedFrames = ReplayTick;
			//set team player
			gameWrapper->GetGameEventAsServer().GetPRIs().Get(0).ServerChangeTeam(StartTeams.at(carit));
			//only Bots
			for (int i = 0;i < LobbySize;i++) {
				SpawnOneBot(i);
			}
			//every Car
			for (int i = 0;i < LobbySize;i++) {
				this->Log(playerNames.at(i));
			}
			
			gameWrapper->GetGameEventAsServer().GetCars().Get(carit + 1).SetHidden2(true);

			//sets boost limited
			if (cvarManager->getCvar("reclip_limitedBoost").getBoolValue() == true) {
				cvarManager->executeCommand("boost set limited", false);
			}
			else {
				cvarManager->executeCommand("boost set unlimited", false);
			}
			//open or converts replay
			if (cvarManager->getCvar("reclip_convert").getBoolValue() == true || cvarManager->getCvar("reclip_openReplay").getBoolValue() == true) {
				gameWrapper->SetTimeout([this](GameWrapper* gw) {
					cvarManager->executeCommand("reclip_controllGame 1", true);
					}, 2.0f);
				cvarManager->getCvar("reclip_convert").setValue(false);
				cvarManager->getCvar("reclip_openReplay").setValue(false);
			}
		}
		cvar.setValue(false);
	}
}

void ReplayClipTrainer::SpawnOneBot(int i) {
	gameWrapper->GetCurrentGameState().SetbRandomizedBotLoadouts(false);
	gameWrapper->GetGameEventAsServer().SpawnBot(CarLayouts.at(i), playerNames.at(i));
}


//Controll the game

void ReplayClipTrainer::ControllGame(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		if (gameWrapper->IsInGame() == true && hasReplaySaved == true && gameWrapper->IsInFreeplay() == false) {
			//reset values
			cvarManager->getCvar("reclip_jumpIn").setValue(0);
			cvarManager->getCvar("reclip_pause").setValue(0);
			//clear this each new session so a checkbox left on from a previous clip doesn't silently
			//affect this one - see reclip_ballFreePhysics comment in ControllGamePerTick()
			cvarManager->getCvar("reclip_ballFreePhysics").setValue(false);
			Tick = 0;
			carit = (RecordedSpectateIndex >= 0 && RecordedSpectateIndex < LobbySize) ? RecordedSpectateIndex : 0;
			CountdownTime = 0;
			jumpInSettleTicks = 0;
			timesNotConverted.clear();
			//isInOvertime = false;
			OrangeScore = 0;
			BlueScore = 0;
			TimeRemaining = 0;
			//match the actual current score instead of hardcoding 0 - if a goal was scored in a
			//previous training session within the same private match, GetTotalScore() would already
			//be non-zero here, and comparing that against a stale Score=0 on the very first tick
			//would incorrectly look like "a goal was just scored" and permanently freeze the ball
			//(the countdown-based recovery never fires since clips don't have a real kickoff)
			Score = gameWrapper->GetGameEventAsServer().GetTotalScore();
			setBally = true;

			isInGoalReplay = false;
			//hook for goal replays
			if (cvarManager->getCvar("reclip_disableGoal").getBoolValue() == false) {
				gameWrapper->HookEvent("Function GameEvent_Soccar_TA.ReplayPlayback.BeginState", std::bind(&ReplayClipTrainer::GoalReplayStart, this, std::placeholders::_1));
				gameWrapper->HookEvent("Function GameEvent_Soccar_TA.ReplayPlayback.EndState", std::bind(&ReplayClipTrainer::GoalReplayEnd, this, std::placeholders::_1));
			}

			//calculate goals frames
			for (int i = 1;i < SavedFrames;i++) {
				if (GameInfoPerFrame.at(i).BlueScore != GameInfoPerFrame.at(i - 1).BlueScore) { BlueScoredFrame.push_back(i); }
				if (GameInfoPerFrame.at(i).OrangeScore != GameInfoPerFrame.at(i - 1).OrangeScore) { OrangeScoredFrame.push_back(i); }
			}

			//calculate not recorded frames
			for (int i = 1;i < SavedFrames;i++) {
				TimeNotConverted TempNotConverted;
				if (i <= 1) {
					if (GameInfoPerFrame.at(i - 1).FrameInReplay > 30) {
						TempNotConverted.start = 0;
						TempNotConverted.end = GameInfoPerFrame.at(i-1).FrameInReplay;
						timesNotConverted.push_back(TempNotConverted);
					}
				}
				else if (i >= (SavedFrames - 2)) {
					if (GameInfoPerFrame.at(i).FrameInReplay < (ReplaySize - 7)) {
						TempNotConverted.start = GameInfoPerFrame.at(i).FrameInReplay;
						TempNotConverted.end = ReplaySize;
						timesNotConverted.push_back(TempNotConverted);
					}
				}
				else if ((GameInfoPerFrame.at(i).FrameInReplay - GameInfoPerFrame.at(i - 1).FrameInReplay) > 30) {
					TempNotConverted.start = GameInfoPerFrame.at(i - 1).FrameInReplay;
					TempNotConverted.end = GameInfoPerFrame.at(i).FrameInReplay;
					timesNotConverted.push_back(TempNotConverted);
				}
			}


			gameWrapper->GetGameEventAsServer().SetLobbyEndCountdown(0);
			CarAmount = gameWrapper->GetGameEventAsServer().GetCars().Count();
			gameWrapper->HookEvent("Function TAGame.EngineShare_TA.EventPostPhysicsStep", std::bind(&ReplayClipTrainer::ControllGamePerTick, this, std::placeholders::_1));
			isPreparingSession = false;

			//set lobby color and team
			gameWrapper->SetTimeout([this](GameWrapper* gw) {
				for (int i = 0;i < LobbySize;i++) {
					for (int p = 1;p < gameWrapper->GetGameEventAsServer().GetCars().Count();p++) {
						//set bot teams and car color
						if (playerNames.at(i) == gameWrapper->GetGameEventAsServer().GetCars().Get(p).GetOwnerName()) {
							//gameWrapper->GetGameEventAsServer().GetCars().Get(p).GetPRI().ServerChangeTeam(StartTeams.at(i));
							gameWrapper->GetGameEventAsServer().GetCarArchetype();
							
							gameWrapper->GetGameEventAsServer().GetCars().Get(p).GetPRI().SetPlayerTeam(gameWrapper->GetGameEventAsServer().GetTeams().Get(StartTeams.at(i)));
							//gameWrapper->GetGameEventAsServer().GetCars().Get(p).SetCarColor(primeColor.at(i), secondColor.at(i));
						}
						//set player car color and team
						//gameWrapper->GetGameEventAsServer().GetCars().Get(0).GetPRI().ServerChangeTeam(StartTeams.at(carit));
						gameWrapper->GetGameEventAsServer().GetCars().Get(0).GetPRI().SetPlayerTeam(gameWrapper->GetGameEventAsServer().GetTeams().Get(StartTeams.at(carit)));//sets team for player
						//gameWrapper->GetGameEventAsServer().GetCars().Get(0).SetCarColor(primeColor.at(carit), secondColor.at(carit));//sets color for car of player
					}
					//making teams of bots appear correct
				}
				for (int i = 1; i < LobbySize + 1; i++) {
					gameWrapper->GetGameEventAsServer().GetCars().Get(i).GetPRI().SetPlayerTeam(gameWrapper->GetGameEventAsServer().GetTeams().Get(CarPositionsPerFrame.at((Tick * LobbySize) + (i - 1)).team));
				}
				}, 0.5f);
		}
		else {
			gameWrapper->UnhookEvent("Function TAGame.EngineShare_TA.EventPostPhysicsStep");
			cvar.setValue(false);
			isPreparingSession = false;
		}
	}
}

void ReplayClipTrainer::ControllGamePerTick(std::string eventName) {
	if (cvarManager->getCvar("reclip_controllGame").getBoolValue() == true && gameWrapper->IsInGame() == true && gameWrapper->IsInFreeplay() == false) {
		if (Tick < (SavedFrames - 8)) {
			//wait out the private match's own kickoff countdown before taking over, so Tick doesn't advance while the ball/cars are still locked to center
			if (isInGoalReplay == false && gameWrapper->GetGameEventAsServer().GetReplicatedRoundCountDownNumber() == 0) {

				//cars
				for (int i = 1;i < LobbySize + 1;i++) {
					//car switching and setting cars
					if (i - 1 == carit) {
						SetCar0(i);
						//jump In
						bool pauseOff = cvarManager->getCvar("reclip_pause").getBoolValue() == false;
						bool jumpInOn = cvarManager->getCvar("reclip_jumpIn").getBoolValue() == true;
						if (pauseOff && jumpInOn && jumpInSettleTicks > 0) {
							//still settling - keep puppeting the car (see jumpInSettleTicks comment in the header)
							jumpInSettleTicks--;
							SetCar(0, i - 1);
						}
						else if (pauseOff && jumpInOn && GameInfoPerFrame.at(Tick).Countdowntime == 0) {
							gameWrapper->GetGameEventAsServer().GetCars().Get(0).ForceBoost(false);
							gameWrapper->GetGameEventAsServer().GetCars().Get(0).SetbOverrideHandbrakeOn(false);
							if (cvarManager->getCvar("reclip_limitedBoost").getBoolValue() == false) { gameWrapper->GetGameEventAsServer().GetCars().Get(0).GetBoostComponent().SetBoostAmount(1.0f); }
						}
						else {
							SetCar(0, i - 1);
						}
					}
					else {
						SetCar(i, i - 1);
					}
				}
				

				
				//ball
				//jumpin ball hit
				if (gameWrapper->GetGameEventAsServer().GetPRIs().Get(0).GetBallTouches() > BallHits && cvarManager->getCvar("reclip_pause").getBoolValue() == false && cvarManager->getCvar("reclip_jumpIn").getBoolValue() == true) {
					setBally = false;
				}
				//jumpin ghosthit exemption
				if (CarPositionsPerFrame.at(((Tick + 8) * LobbySize) + carit).ballTouches != CarPositionsPerFrame.at(((Tick + 7) * LobbySize) + carit).ballTouches && cvarManager->getCvar("reclip_jumpIn").getBoolValue() == true) {
					setBally = false;
				}
				//manual "let ball go free" toggle - lets the ball follow real physics from the moment
				//you resume from pause, instead of only when a recorded/real touch is detected. Useful
				//for practicing touches that happen later than the original recording. Only takes effect
				//while unpaused - during pause, the block below (reset pause) keeps forcing setBally=true
				//every tick, so checking this box during pause has no visible effect until you resume.
				if (cvarManager->getCvar("reclip_ballFreePhysics").getBoolValue() == true && cvarManager->getCvar("reclip_pause").getBoolValue() == false && cvarManager->getCvar("reclip_jumpIn").getBoolValue() == true) {
					setBally = false;
				}
				//score exemption
				if (Score != gameWrapper->GetGameEventAsServer().GetTotalScore()) {
					setBally = false;
				}
				if (GameInfoPerFrame.at(Tick).Countdowntime != CountdownTime) {
					setBally = true;
				}
				//set ball
				if (setBally == true) {
					SetBall();
					//Log("X:" + std::to_string(BallPositionPerFrame.at(Tick).location.X) + " Y:" + std::to_string(BallPositionPerFrame.at(Tick).location.Z));
				}

				//score and time
				SetGameInfo();

				//variable incrementation
				Score = gameWrapper->GetGameEventAsServer().GetTotalScore();
				CountdownTime = GameInfoPerFrame.at(Tick).Countdowntime;

				//pause
				if (cvarManager->getCvar("reclip_pause").getBoolValue() == false) {
					Tick++;
				}
				//reset pause
				else {
					if (cvarManager->getCvar("reclip_jumpIn").getBoolValue() == true) {
						//unpause with controller input
						if (cvarManager->getCvar("reclip_inputToUnpause").getBoolValue() == true) {
							auto CarInput = gameWrapper->GetPlayerController().GetVehicleInput();
							if (CarInput.HoldingBoost == true || CarInput.Throttle < -0.1 || CarInput.Throttle > 0.1) {
								cvarManager->executeCommand("reclip_pause 0");
							}
						}
						Tick = JumpInTick;
						BallHits = gameWrapper->GetGameEventAsServer().GetPRIs().Get(0).GetBallTouches();
						setBally = true;
						gameWrapper->GetGameEventAsServer().ResetPickups();
					}
				}
				
			}
		}
		else {
			Tick = 0;
		}
	}
	else {
		Tick = 0;
		gameWrapper->UnhookEvent("Function TAGame.EngineShare_TA.EventPostPhysicsStep");
		cvarManager->getCvar("reclip_controllGame").setValue(false);
	}
}

void ReplayClipTrainer::SetCar(int i, int it) {
	CarWrapper Car = gameWrapper->GetGameEventAsServer().GetCars().Get(i);
	if (i == 0) {
		static ControllerInput input;
		input.Throttle = (CarPositionsPerFrame.at((Tick * LobbySize) + it).throttle - 128.0) / 128.0;
		input.Steer = (CarPositionsPerFrame.at((Tick * LobbySize) + it).steer - 128.0) / 128.0;
	}

	Car.SetLocation(CarPositionsPerFrame.at((Tick * LobbySize) + it).location);
	Car.SetVelocity(CarPositionsPerFrame.at((Tick * LobbySize) + it).velocitiy);
	Car.SetRotation(CarPositionsPerFrame.at((Tick * LobbySize) + it).rotation);
	Car.SetAngularVelocity(CarPositionsPerFrame.at((Tick * LobbySize) + it).angVelocity, false);
	Car.SetbOverrideHandbrakeOn(CarPositionsPerFrame.at((Tick * LobbySize) + it).handbrake);
	//SetBoostAmount() expects a 0.0-1.0 fraction (see the SetBoostAmount(1.0f) full-boost call below),
	//but the recorded value was captured via GetCurrentBoostAmount() which is a 0-100 percentage -
	//using SetBoostAmount() here fed it a value ~100x too large, causing the boost HUD to show garbage.
	//SetCurrentBoostAmount() is the matching setter for the same percentage-scale field.
	float boostToApply = CarPositionsPerFrame.at((Tick * LobbySize) + it).boostAmount;
	if (boostToApply < 0.0f) { boostToApply = 0.0f; }
	if (boostToApply > 100.0f) { boostToApply = 100.0f; }
	Car.GetBoostComponent().SetCurrentBoostAmount(boostToApply);
	Car.ForceBoost(CarPositionsPerFrame.at((Tick * LobbySize) + it).isBoosting);
	
	//Car.GetPRI().SetPlayerTeam(gameWrapper->GetGameEventAsServer().GetTeams().Get(CarPositionsPerFrame.at((Tick * LobbySize) + it).team));
	//Car.GetPRI().eventSetPlayerName(CarPositionsPerFrame.at((Tick * LobbySize) + it).name);
	//Car.SetHidden2(false);
	//Car.SetbCanJump(CarPositionsPerFrame.at((Tick * LobbySize) + it).hasDodge);

	//Log(std::to_string(i)+": "+std::to_string(CarPositionsPerFrame.at(((Tick) * LobbySize) + it).location.X) + ", " + std::to_string(CarPositionsPerFrame.at(((Tick) * LobbySize) + it).location.Y) + ", " + std::to_string(CarPositionsPerFrame.at(((Tick) * LobbySize) + it).location.Z));
}

void ReplayClipTrainer::SetCar0(int i) {
	CarWrapper Car = gameWrapper->GetGameEventAsServer().GetCars().Get(i);
	
	Car.SetLocation(Vector(0, 0, 5000));
	Car.SetVelocity(Vector(0, 0, 0));
	Car.SetRotation(Rotator(0, 0, 0));
	Car.SetAngularVelocity(Vector(0, 0, 0), false);
	//Car.GetBoostComponent().SetBoostAmount();
	Car.ForceBoost(0);
	//Car.SetHidden2(true);
	//Car.SetbCanJump(0);
}

void ReplayClipTrainer::SetBall() {
	BallWrapper Ball = gameWrapper->GetGameEventAsServer().GetBall();
	Ball.SetLocation(BallPositionPerFrame.at(Tick).location);
	Ball.SetVelocity(BallPositionPerFrame.at(Tick).velocitiy);
	Ball.SetRotation(BallPositionPerFrame.at(Tick).rotation);
	Ball.SetAngularVelocity(BallPositionPerFrame.at(Tick).angVelocity, false);
}

void ReplayClipTrainer::SetGameInfo() {
	gameWrapper->GetGameEventAsServer().SetScoreAndTime(gameWrapper->GetGameEventAsServer().GetLocalPrimaryPlayer(), GameInfoPerFrame.at(Tick).BlueScore, GameInfoPerFrame.at(Tick).OrangeScore, GameInfoPerFrame.at(Tick).TimeRemaining, false, false);

	if (GameInfoPerFrame.at(Tick).Countdowntime != CountdownTime) {
		if (GameInfoPerFrame.at(Tick).Countdowntime == 0) {
			gameWrapper->GetGameEventAsServer().BroadcastGoMessage();
		}
		else {
			gameWrapper->GetGameEventAsServer().BroadcastCountdownMessage(GameInfoPerFrame.at(Tick).Countdowntime);
		}
	}
}

//hooks for goal scoring enabled
void ReplayClipTrainer::GoalReplayStart(std::string eventName) {
	isInGoalReplay = true;
}

void ReplayClipTrainer::GoalReplayEnd(std::string eventName) {
	isInGoalReplay = false;
}


//skip

void ReplayClipTrainer::Skip(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getIntValue() != 0) {
		if (gameWrapper->IsInGame() == true && cvarManager->getCvar("reclip_controllGame").getBoolValue() == true) {
			setBally = true;
			int tickBeforeSkip = Tick;
			//skip certain amount of time - skipping back past the clip's start just clamps to frame 0
			//(the start of the clip) below, rather than wrapping around to the end
			Tick = Tick + (120 * cvar.getFloatValue());
			//skip to keyframe starts
			for (int i = 0;i < KeyframesInConvert.size();i++) {
				int begin = (KeyframesInConvert.at(i)- (cvarManager->getCvar("reclip_timeBeforKeyframe").getIntValue() * 120));
				if (Tick > begin && tickBeforeSkip < begin) { Tick = begin; }
			}
			if (Tick < 0) { Tick = 0; }
			if (Tick > SavedFrames - 8) { Tick = 0; }
		}
		cvar.setValue(0);
	}
}


//car switching

void ReplayClipTrainer::SwitchCars(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		if (gameWrapper->IsInGame() == true && cvarManager->getCvar("reclip_controllGame").getBoolValue() == true) {
			ServerWrapper Game = gameWrapper->GetGameEventAsServer();
			//find the next active player after carit, wrapping around at most once - the old version
			//incremented carit with no bound/wrap check, so if several consecutive players were
			//inactive (e.g. bots not yet joined at this Tick in the original replay) it would walk
			//past the end of CarPositionsPerFrame and crash with an uncaught out_of_range
			int nextCarit = carit;
			bool foundActive = false;
			for (int attempts = 0; attempts < LobbySize; attempts++) {
				nextCarit = (nextCarit + 1) % LobbySize;
				if (CarPositionsPerFrame.at((Tick * LobbySize) + nextCarit).isActive) {
					foundActive = true;
					break;
				}
			}
			if (foundActive) {
				Game.GetCars().Get(carit + 1).SetHidden2(false);
				carit = nextCarit;
				gameWrapper->GetGameEventAsServer().GetCars().Get(0).GetPRI().SetPlayerTeam(gameWrapper->GetGameEventAsServer().GetTeams().Get(StartTeams.at(carit)));//sets team for player
				//gameWrapper->GetGameEventAsServer().GetCars().Get(0).SetCarColor(primeColor.at(carit), secondColor.at(carit));//sets color for car of player
				Game.GetCars().Get(carit + 1).SetHidden2(true);

				//making teams of bots appear correct
				for (int i = 1; i < LobbySize + 1; i++) {
					gameWrapper->GetGameEventAsServer().GetCars().Get(i).GetPRI().SetPlayerTeam(gameWrapper->GetGameEventAsServer().GetTeams().Get(CarPositionsPerFrame.at((Tick * LobbySize) + (i - 1)).team));
				}
			}
		}
		cvar.setValue(false);
	}
}

void ReplayClipTrainer::SwitchBack(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		if (gameWrapper->IsInGame() == true && cvarManager->getCvar("reclip_controllGame").getBoolValue() == true) {
			for (int i = 0;i < (LobbySize - 1);i++) {
				cvarManager->executeCommand("reclip_switchPlayer 1", false);
			}
		}
		cvar.setValue(false);
	}
}


//jump In

void ReplayClipTrainer::JumpIn(std::string oldValue, CVarWrapper cvar) {
	if (suppressJumpInCallback) { return; } //re-entrant call from our own revert below - ignore
	auto now = std::chrono::steady_clock::now();
	if (now - lastJumpInToggleTime < std::chrono::milliseconds(400)) {
		//holding the bound gamepad button can re-fire this toggle every tick instead of once per
		//physical press, flickering JumpIn/JumpOut back and forth. Revert this extra toggle so a
		//long press only flips state once - see the member comment in the header.
		suppressJumpInCallback = true;
		cvar.setValue(oldValue);
		suppressJumpInCallback = false;
		return;
	}
	lastJumpInToggleTime = now;
	if (gameWrapper->IsInGame() == true && cvarManager->getCvar("reclip_controllGame").getBoolValue() == true) {
		BallHits = gameWrapper->GetGameEventAsServer().GetPRIs().Get(0).GetBallTouches();
		if (cvar.getIntValue() == 1) {
			cvarManager->executeCommand("reclip_pause 1");
			gameWrapper->GetGameEventAsServer().ResetPickups();
			setBally = true;
			JumpInTick = Tick;
			jumpInSettleTicks = 60; //~0.5s at 120 ticks/sec
			//set boost because airdribble plugin fuck my shit up like crazy and isn't even open source AAAAAAAHHHHH
			cvarManager->executeCommand("sv_soccar_enablegoal 0", false);
			if (cvarManager->getCvar("reclip_limitedBoost").getBoolValue() == true) {
				cvarManager->executeCommand("boost set limited", false);
			}
			else {
				cvarManager->executeCommand("boost set unlimited", false);
			}
		}
		else {
			setBally = true;
			Tick = JumpInTick;
			cvarManager->executeCommand("reclip_pause 1");
		}
	}
}


//convert

//manual
void ReplayClipTrainer::Convert(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		if (gameWrapper->IsInReplay() == true) {
			cvarManager->executeCommand("reclip_replaySave 1", true);
		}
		gameWrapper->HookEvent("Function Engine.GameInfo.InitGame", std::bind(&ReplayClipTrainer::JoinedFreeplay, this, std::placeholders::_1));
	}
}

//minimum total clip length (before + after) - clips shorter than this aren't useful to train and can
//run into edge cases (e.g. the private match's own kickoff countdown eating most/all of the clip)
static const float kMinClipTotalSeconds = 5.0f;

//converts only a short clip around the current replay scrub position
void ReplayClipTrainer::ConvertClip(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		if (gameWrapper->IsInReplay() == true) {
			float fps = gameWrapper->GetGameEventAsReplay().GetReplayFPS();
			int center = gameWrapper->GetGameEventAsReplay().GetCurrentReplayFrame();
			float secondsBefore = cvarManager->getCvar("reclip_clipSecondsBefore").getFloatValue();
			float secondsAfter = cvarManager->getCvar("reclip_clipSecondsAfter").getFloatValue();

			if (secondsBefore + secondsAfter < kMinClipTotalSeconds) {
				lastClipRangeError = "Clip is too short - Before + After must add up to at least " + std::to_string(static_cast<int>(kMinClipTotalSeconds)) + " seconds.";
				cvar.setValue(false);
				return;
			}
			lastClipRangeError.clear();

			ConvertStartFrame = center - static_cast<int>(secondsBefore * fps);
			ConvertEndFrame = center + static_cast<int>(secondsAfter * fps);
			useClipRange = true;

			cvarManager->executeCommand("reclip_convert 1", true);
		}
		else {
			this->Log("you must be in a replay to execute this command");
		}
		cvar.setValue(false);
	}
}

//converts a clip like ConvertClip(), but saves the result to a named file instead of opening a private match immediately
void ReplayClipTrainer::ConvertClipToFile(std::string name) {
	if (gameWrapper->IsInReplay() == false) {
		this->Log("you must be in a replay to execute this command");
		return;
	}
	float fps = gameWrapper->GetGameEventAsReplay().GetReplayFPS();
	int center = gameWrapper->GetGameEventAsReplay().GetCurrentReplayFrame();
	float secondsBefore = cvarManager->getCvar("reclip_clipSecondsBefore").getFloatValue();
	float secondsAfter = cvarManager->getCvar("reclip_clipSecondsAfter").getFloatValue();

	if (secondsBefore + secondsAfter < kMinClipTotalSeconds) {
		lastClipRangeError = "Clip is too short - Before + After must add up to at least " + std::to_string(static_cast<int>(kMinClipTotalSeconds)) + " seconds.";
		return;
	}
	lastClipRangeError.clear();

	ConvertStartFrame = center - static_cast<int>(secondsBefore * fps);
	ConvertEndFrame = center + static_cast<int>(secondsAfter * fps);
	useClipRange = true;
	pendingSaveToFile = true;
	pendingSaveName = name;

	cvarManager->executeCommand("reclip_replaySave 1", true);
}

//actually writes the just-converted clip (currently in memory) to disk, called once conversion finishes
void ReplayClipTrainer::SaveCurrentClipToFile(std::string name) {
	//sanitize into a safe filename
	for (char& c : name) {
		if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') { c = '_'; }
	}
	if (name.empty()) {
		std::time_t now = std::time(nullptr);
		std::tm localTime;
		localtime_s(&localTime, &now);
		std::stringstream ss;
		ss << std::put_time(&localTime, "clip_%Y%m%d_%H%M%S");
		name = ss.str();
	}
	if (IsReservedDeviceName(name)) { name += "_clip"; }

	//build the path from a wide string so a non-ASCII (e.g. Japanese) name survives correctly -
	//constructing std::filesystem::path from a UTF-8 std::string directly would go through the
	//Windows ANSI codepage instead and can garble or fail
	std::filesystem::path folder = std::filesystem::path(gameWrapper->GetBakkesModPath().wstring()) / L"data" / L"replayClipTrainer" / L"clips";
	std::filesystem::create_directories(folder);
	std::filesystem::path path = folder / (Utf8ToWString(name) + L".reclip");

	ClipFile file;
	if (file.SaveFile(path, *this)) {
		this->Log("Saved clip to " + path.u8string());
		lastSaveClipSucceeded = true;
		lastSaveClipMessage = "Saved clip: " + path.filename().u8string();
	}
	else {
		this->Log("Failed to save clip to " + path.u8string());
		lastSaveClipSucceeded = false;
		lastSaveClipMessage = "Failed to save clip to " + path.filename().u8string();
	}
	this->RefreshSavedClipsList();
}

//loads a previously saved clip from disk and starts a private match/training session with it
void ReplayClipTrainer::LoadClipAndOpen(std::string path) {
	ClipFile file;
	std::string error;
	if (!file.ReadFile(std::filesystem::u8path(path), *this, error)) {
		this->Log("Failed to load clip: " + path + " (" + error + ")");
		lastLoadClipError = error;
		return;
	}
	lastLoadClipError.clear();
	hasReplaySaved = true;
	//go through the same reclip_openReplay cvar path "Open Saved Replay" uses, since SpawnBots()
	//only auto-starts reclip_controllGame (the actual per-tick playback) when that cvar is left true
	cvarManager->executeCommand("reclip_openReplay 1", true);
}

//root folder all saved clips live under, regardless of which subfolder is currently browsed
static std::filesystem::path GetClipsRootFolder(GameWrapper* gameWrapper) {
	return std::filesystem::path(gameWrapper->GetBakkesModPath().wstring()) / L"data" / L"replayClipTrainer" / L"clips";
}

//refreshes the cached list of saved clip files and subfolders for the currently browsed folder
//(used by the settings GUI). Folder creation/deletion isn't handled here - see currentClipFolder
//comment in the header - this only browses what's already on disk.
void ReplayClipTrainer::RefreshSavedClipsList() {
	savedClips.clear();
	savedClipFolders.clear();
	std::filesystem::path root = GetClipsRootFolder(gameWrapper.get());
	std::filesystem::path folder = currentClipFolder.empty() ? root : root / std::filesystem::u8path(currentClipFolder);
	if (!std::filesystem::exists(folder)) {
		//the browsed folder vanished (e.g. deleted in Explorer while browsing it) - fall back to root
		currentClipFolder.clear();
		folder = root;
		if (!std::filesystem::exists(folder)) { return; }
	}
	for (const auto& entry : std::filesystem::directory_iterator(folder)) {
		if (entry.is_directory()) {
			savedClipFolders.push_back(entry.path().filename().u8string());
		}
		else if (entry.is_regular_file() && entry.path().extension() == ".reclip") {
			SavedClipInfo info;
			info.path = entry.path().u8string();
			ClipMetadata meta = ClipFile::PeekMetadata(entry.path());
			info.isCompatible = meta.isCompatible;
			info.saveTimeStr = FileTimeToString(entry.path());
			info.playerCount = meta.lobbySize;
			savedClips.push_back(info);
		}
	}
	std::sort(savedClipFolders.begin(), savedClipFolders.end());
}

//opens the currently browsed clips folder in Windows Explorer
void ReplayClipTrainer::OpenClipsFolder() {
	std::filesystem::path root = GetClipsRootFolder(gameWrapper.get());
	std::filesystem::path folder = currentClipFolder.empty() ? root : root / std::filesystem::u8path(currentClipFolder);
	std::filesystem::create_directories(folder);
	ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ReplayClipTrainer::JoinedFreeplay(std::string eventName) {
	//gameWrapper->HookEvent("Function TAGame.Ball_TA.SetGameEvent", std::bind(&ReplayClipTrainer::BallSpawned, this, std::placeholders::_1));
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		cvarManager->executeCommand("reclip_botSpawn 1", true);
		}, 3.0f);
	gameWrapper->UnhookEvent("Function Engine.GameInfo.InitGame");
}


//auto
void ReplayClipTrainer::AutoConvertCvar(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		gameWrapper->HookEvent("Function TAGame.GameInfo_Replay_TA.InitGame", std::bind(&ReplayClipTrainer::IsInReplay, this, std::placeholders::_1));
		cvarManager->executeCommand("reclip_convert 1", false);
	}
	else {
		gameWrapper->UnhookEvent("Function TAGame.GameInfo_Replay_TA.InitGame");
		cvarManager->getCvar("reclip_convert").setValue(false);
	}
}

void ReplayClipTrainer::AutoConvert() {
	if (cvarManager->getCvar("reclip_autoConvert").getBoolValue() == true) {
		gameWrapper->HookEvent("Function TAGame.GameInfo_Replay_TA.InitGame", std::bind(&ReplayClipTrainer::IsInReplay, this, std::placeholders::_1));
	}
	else {
		gameWrapper->UnhookEvent("Function TAGame.GameInfo_Replay_TA.InitGame");
	}
}

void ReplayClipTrainer::IsInReplay(std::string eventName) {
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		cvarManager->executeCommand("reclip_convert 1", true);
		}, 4.0f);
}


void ReplayClipTrainer::OpenReplay(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		if (hasReplaySaved == true) {
			cvarManager->getCvar("reclip_controllGame").setValue(false);
			cvarManager->executeCommand("reclip_privateMatch 1", true);
			gameWrapper->HookEvent("Function Engine.GameInfo.InitGame", std::bind(&ReplayClipTrainer::JoinedFreeplay, this, std::placeholders::_1));
		}
	}
}


//bindings

void ReplayClipTrainer::Bindings(std::string oldValue, CVarWrapper cvar) {
	if (cvar.getBoolValue() == true) {
		//controller
		cvarManager->executeCommand("bind XboxTypeS_DPad_Right 'default_right; reclip_skip 10'");
		cvarManager->executeCommand("bind XboxTypeS_DPad_Left 'default_left; reclip_skip -5'");
		cvarManager->executeCommand("bind XboxTypeS_DPad_Up 'default_up; reclip_switchPlayer 1'");
		cvarManager->executeCommand("bind XboxTypeS_DPad_Down 'default_down; reclip_switchBack 1'");
		cvarManager->executeCommand("bind XboxTypeS_Back 'toggle reclip_jumpIn 1 0'");
		cvarManager->executeCommand("bind XboxTypeS_LeftThumbStick 'toggle reclip_pause 1 0'");
		//keyboard
		cvarManager->executeCommand("bind Right 'reclip_skip 10'");
		cvarManager->executeCommand("bind Left 'reclip_skip -5'");
		cvarManager->executeCommand("bind Up 'reclip_switchPlayer 1'");
		cvarManager->executeCommand("bind Down 'reclip_switchBack 1'");
		cvarManager->executeCommand("bind V 'toggle reclip_jumpIn 1 0'");
		cvarManager->executeCommand("bind B 'toggle reclip_pause 1 0'");

		cvar.setValue(false);
	}
}


//-------//
//OVERLAY//
//-------//


//draw HUD

void ReplayClipTrainer::DrawHud(CanvasWrapper canvas) {
	if (cvarManager->getCvar("reclip_showHud").getBoolValue() == true) {
		if (gameWrapper->IsInGame() == true && hasReplaySaved == true && cvarManager->getCvar("reclip_controllGame").getBoolValue() == true) {
			resX = canvas.GetSize().X;
			resY = canvas.GetSize().Y;
			DrawTime(canvas);
			DrawPause(canvas);
			DrawTimeline(canvas);
			DrawTimelineSpent(canvas);
			DrawTimelineNotConverted(canvas);
			DrawPlayer(canvas);
			DrawJumpIn(canvas);
			DrawBlueScored(canvas);
			DrawOrangeScored(canvas);
			DrawKeyframe(canvas);
		}
	}
}


//time

void ReplayClipTrainer::DrawTime(CanvasWrapper canvas) {
	//calculate minutes and seconds
	MinutesAvailable = ReplaySize / (60 * ReplayFPS);
	SecondsAvailable = (ReplaySize / ReplayFPS) - (MinutesAvailable * 60);
	MinutesDone = GameInfoPerFrame.at(Tick).FrameInReplay / (60* ReplayFPS);
	SecondsDone = (GameInfoPerFrame.at(Tick).FrameInReplay / ReplayFPS) - (MinutesDone * 60);
	//make seconds two didgets
	std::stringstream SSecondsDone;
	SSecondsDone << std::setw(2) << std::setfill('0') << SecondsDone;
	std::string StringSecondsDone = SSecondsDone.str();
	std::stringstream SSecondsAvailable;
	SSecondsAvailable << std::setw(2) << std::setfill('0') << SecondsAvailable;
	std::string StringSecondsAvailable = SSecondsAvailable.str();
	//draw time
	canvas.SetColor(236, 236, 236, 255);
	canvas.SetPosition(Vector2{ ConvertToScreenSizeX(1361),ConvertToScreenSizeY(1000) });
	canvas.DrawString(std::to_string(MinutesDone) + ":" + StringSecondsDone + "/" + std::to_string(MinutesAvailable) + ":" + StringSecondsAvailable, ConvertTextSizeX(1.5), ConvertTextSizeY(1.5), true, false);
}


//pause

void ReplayClipTrainer::DrawPause(CanvasWrapper canvas) {
	LinearColor color;
	color.R = 236;
	color.G = 236;
	color.B = 236;
	color.A = 255;
	if (cvarManager->getCvar("reclip_pause").getBoolValue() == true) {
		//triangle
		canvas.FillTriangle(Vector2{ ConvertToScreenSizeX(460),ConvertToScreenSizeY(1000) }, Vector2{ ConvertToScreenSizeX(460),ConvertToScreenSizeY(1024) }, Vector2{ ConvertToScreenSizeX(484),ConvertToScreenSizeY(1012) }, color);
	}
	else {
		//pause button
		canvas.SetColor(color);
		canvas.DrawLine(Vector2{ ConvertToScreenSizeX(460),ConvertToScreenSizeY(1000) }, Vector2{ ConvertToScreenSizeX(468),ConvertToScreenSizeY(1000) }, ConvertToScreenSizeY(24));
		canvas.DrawLine(Vector2{ ConvertToScreenSizeX(476),ConvertToScreenSizeY(1000) }, Vector2{ ConvertToScreenSizeX(484),ConvertToScreenSizeY(1000) }, ConvertToScreenSizeY(24));
	}
}


//Timeline

void ReplayClipTrainer::DrawTimeline(CanvasWrapper canvas) {
	canvas.SetColor(232, 232, 232, 150);
	canvas.DrawLine(Vector2{ ConvertToScreenSizeX(460),ConvertToScreenSizeY(960) }, Vector2{ ConvertToScreenSizeX(1460),ConvertToScreenSizeY(960) }, ConvertToScreenSizeY(6));
}

void ReplayClipTrainer::DrawTimelineSpent(CanvasWrapper canvas) {
	int length = 0;
	float ReplaySpent = GameInfoPerFrame.at(Tick).FrameInReplay;
	float ReplayLength = ReplaySize;
	length = ConvertToScreenSizeX(1000) * (ReplaySpent / ReplayLength);
	canvas.SetColor(0, 126, 255, 230);
	canvas.DrawLine(Vector2{ ConvertToScreenSizeX(460),ConvertToScreenSizeY(960) }, Vector2{ (ConvertToScreenSizeX(460) + length),ConvertToScreenSizeY(960) }, ConvertToScreenSizeY(6));
}

void ReplayClipTrainer::DrawTimelineNotConverted(CanvasWrapper canvas) {
	if (timesNotConverted.size() > 0) {
		for (int i = 0;i < timesNotConverted.size();i++) {
			float begin = timesNotConverted.at(i).start;
			float end = timesNotConverted.at(i).end;
			float ReplayLength = ReplaySize;
			int beginInt = ConvertToScreenSizeX(1000) * (begin / ReplayLength);
			int endInt = ConvertToScreenSizeX(1000) * (end / ReplayLength);
			canvas.SetColor(65, 65, 65, 255);
			canvas.DrawLine(Vector2{ (ConvertToScreenSizeX(460) + beginInt),ConvertToScreenSizeY(960) }, Vector2{ (ConvertToScreenSizeX(460) + endInt),ConvertToScreenSizeY(960) }, ConvertToScreenSizeY(6));
		}
	}
}


//Player

void ReplayClipTrainer::DrawPlayer(CanvasWrapper canvas) {

	//team color
	if ((static_cast<int>(StartTeams.at(carit))) == 0) {
		canvas.SetColor(12, 136, 252, 255);//blue
	}
	else {
		canvas.SetColor(252, 124, 12, 255);//orange
	}
	canvas.SetPosition(Vector2{ ConvertToScreenSizeX(650),ConvertToScreenSizeY(920) });
	canvas.DrawString(playerNames.at(carit), ConvertTextSizeX(1.5), ConvertTextSizeY(1.5), true, false);

}


//JumpIn

void ReplayClipTrainer::DrawJumpIn(CanvasWrapper canvas) {
	canvas.SetPosition(Vector2{ ConvertToScreenSizeX(1240),ConvertToScreenSizeY(920) });
	if (cvarManager->getCvar("reclip_jumpIn").getBoolValue() != true) {
		//jumpout
		canvas.SetColor(114, 225, 78, 255);
		canvas.DrawString("JumpIn", ConvertTextSizeX(1.5), ConvertTextSizeY(1.5), true, false);
	}
	else {
		//jumpIn
		canvas.SetColor(255, 1, 1, 255);
		canvas.DrawString("JumpOut", ConvertTextSizeX(1.5), ConvertTextSizeY(1.5), true, false);
	}
}


//show goals in Timelin

void ReplayClipTrainer::DrawBlueScored(CanvasWrapper canvas) {
	LinearColor color = { 12, 136, 252, 255 };
	for (int i = 0;i < BlueScoredFrame.size();i++) {
		float ReplayLength = ReplaySize;
		float FrameScored = GameInfoPerFrame.at(BlueScoredFrame.at(i)).FrameInReplay;
		int position = ConvertToScreenSizeX(460) + (ConvertToScreenSizeX(1000) * (FrameScored / ReplayLength));
		canvas.FillTriangle(Vector2{ position,ConvertToScreenSizeY(963) }, Vector2{ position - ConvertToScreenSizeX(3),ConvertToScreenSizeY(972) }, Vector2{ position + ConvertToScreenSizeX(3),ConvertToScreenSizeY(972) }, color);
	}
}

void ReplayClipTrainer::DrawOrangeScored(CanvasWrapper canvas) {
	LinearColor color = { 252, 124, 12, 255 };
	for (int i = 0;i < OrangeScoredFrame.size();i++) {
		float ReplayLength = ReplaySize;
		float FrameScored = GameInfoPerFrame.at(OrangeScoredFrame.at(i)).FrameInReplay;
		int position = ConvertToScreenSizeX(460) + (ConvertToScreenSizeX(1000) * (FrameScored / ReplayLength));
		canvas.FillTriangle(Vector2{ position,ConvertToScreenSizeY(963) }, Vector2{ position - ConvertToScreenSizeX(3),ConvertToScreenSizeY(972) }, Vector2{ position + ConvertToScreenSizeX(3),ConvertToScreenSizeY(972) }, color);
	}
}

void ReplayClipTrainer::DrawKeyframe(CanvasWrapper canvas) {
	LinearColor color = { 190, 190, 190, 255 };
	for (int i = 0;i < KeyframesInConvert.size();i++) {
		float ReplayLength = ReplaySize;
		float KeyFrame = GameInfoPerFrame.at(KeyframesInConvert.at(i)).FrameInReplay;
		int position = ConvertToScreenSizeX(460) + (ConvertToScreenSizeX(1000) * (KeyFrame / ReplayLength));
		canvas.FillTriangle(Vector2{ position,ConvertToScreenSizeY(963) }, Vector2{ position - ConvertToScreenSizeX(3),ConvertToScreenSizeY(972) }, Vector2{ position + ConvertToScreenSizeX(3),ConvertToScreenSizeY(972) }, color);
	}
}


//convert from 1080p to all resolutions

int ReplayClipTrainer::ConvertToScreenSizeY(int HDpixel) {
	float position = std::roundf((HDpixel / 1080.0f) *  resY);

	return static_cast<int>(position);
}

int ReplayClipTrainer::ConvertToScreenSizeX(int HDpixel) {
	float position = std::roundf((HDpixel / 1920.0f) * resX);

	return static_cast<int>(position);
}

float ReplayClipTrainer::ConvertTextSizeY(float Text) {
	float scale = (Text / 1080.0f) * resY;

	return scale;
}

float ReplayClipTrainer::ConvertTextSizeX(float Text) {
	float scale = (Text / 1920.0f) * resX;

	return scale;
}


//Bakkesmod Log

void ReplayClipTrainer::Log(std::string msg) {
	cvarManager->log(msg);

	return;
}
