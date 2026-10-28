#pragma once

#pragma comment( lib, "pluginsdk.lib")

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#include "version.h"
#include "compatibility.h"

#include <vector>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <string>
struct ImFont;
//shown in the settings GUI and BakkesMod's plugin list. SemVer-style (MAJOR.MINOR.PATCH), bumped
//manually at release time - see version.h for what each number means.
constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH);

struct CarPosition
{
	Vector location;
	Vector velocitiy;
	Rotator rotation;
	Vector angVelocity;
	float steer;
	float throttle;
	bool handbrake;
	float boostAmount;
	bool isBoosting;
	//float lastJumped;
	//bool hasDodge;
	int ballTouches;
	int team;
	bool isActive;
	//std::string name;
};

struct BallPosition
{
	Vector location;
	Vector velocitiy;
	Rotator rotation;
	Vector angVelocity;
};

struct GameInfo
{
	int TimeRemaining;
	int OrangeScore;
	int BlueScore;
	//bool IsInOvertime;
	int Countdowntime;
	int FrameInReplay;
	//bool IsInCountdown;
};

struct TimeNotConverted
{
	int start;
	int end;
};

//goal / highlight positions of the replay being watched, shown on the replay position bar
struct ReplayMarker
{
	int frame = 0;
	int kind = 0; //0 = goal by the blue team, 1 = goal by the orange team, 2 = highlight
	std::string name;
};

struct SavedClipInfo
{
	std::string path;
	bool isCompatible;
	std::string saveTimeStr;
	int playerCount = 0;
	//folder relative to the clips root (UTF-8, '/'-separated); empty means the root itself. Only
	//filled in for the flattened recent-clips list.
	std::string folder;
	std::filesystem::file_time_type writeTime{};
};

class ReplayClipTrainer: public BakkesMod::Plugin::BakkesModPlugin, /*public BakkesMod::Plugin::PluginSettingsWindow,*/ public BakkesMod::Plugin::PluginWindow
{
public:
	friend struct UiPainter; //the settings-window painter (ReplayClipTrainerGUI.cpp) reads/writes plugin state
	virtual void onLoad();
	virtual void onUnload();

	//std::shared_ptr<GameWrapper> _globalGameWrapper = gameWrapper;
	Compatibility compatibility;

	void CvarRegister();

	//cvar commands
	void SaveReplay(std::string oldValue, CVarWrapper cvar);
	void StartPrivateMatch(std::string oldValue, CVarWrapper cvar);
	void SpawnBots(std::string oldValue, CVarWrapper cvar);
	void ControllGame(std::string oldValue, CVarWrapper cvar);
	void Skip(std::string oldValue, CVarWrapper cvar);
	void SwitchCars(std::string oldValue, CVarWrapper cvar);
	void SwitchBack(std::string oldValue, CVarWrapper cvar);
	void JumpIn(std::string oldValue, CVarWrapper cvar);
	void Convert(std::string oldValue, CVarWrapper cvar);
	void ConvertClip(std::string oldValue, CVarWrapper cvar);
	void ConvertClipToFile(std::string name, std::string folder = std::string()); //folder: relative to the clips root, "" = root
	void SaveCurrentClipToFile(std::string name);
	void LoadClipAndOpen(std::string path);
	void RefreshSavedClipsList();
	void RefreshRecentClipsList();
	void RefreshActiveClipList();
	void OpenClipsFolder();
	//opens the replay that was last converted from (the temporary export made at every conversion) again, a few seconds
	//before the last clip - lets you go back from a practice session to cut more scenes. Returns false if there is none.
	bool ReopenSourceReplay();
	float lastSourceStartSec_ = 0.f; //start of the last converted range, in seconds into the replay
	//the camera mode and the spectated player at the time of the last conversion, put back after ReopenSourceReplay()
	std::string sourceCameraState_;
	std::string sourceFocusActor_;
	void RestoreReplayView(int attempt, int applied);
	//clip / folder management for the clips tab. Each returns "" on success or an error key: "exists", "invalid", "notEmpty", "failed".
	//Folder paths are relative to the clips root, '/'-separated, UTF-8.
	std::string CreateClipFolder(const std::string& name);                              //inside currentClipFolder
	std::string RenameClip(const std::string& clipPath, const std::string& newName);
	std::string RenameClipFolder(const std::string& folderRel, const std::string& newName);
	std::string MoveClip(const std::string& clipPath, const std::string& destFolderRel);
	std::string DeleteClipFolder(const std::string& folderRel);                         //empty folders only
	std::vector<std::string> ListClipFolders();                                         //every folder under the root, "" (the root) first
	void SaveConfigSoon();                                                              //debounced `writeconfig`, so settings survive a crash
	std::atomic<bool> configSavePending_{ false };
	void CancelConversion();
	std::vector<SavedClipInfo> savedClips;
	//subfolder names (leaf only, UTF-8) found directly inside the currently browsed clips folder
	std::vector<std::string> savedClipFolders;
	//path (UTF-8, '/'-separated) of the currently browsed folder, relative to the clips root folder;
	//empty means the root. Folder creation/deletion is left to Explorer (via Open Clips Folder) -
	//this is browse-only.
	std::string currentClipFolder;
	//"recent clips" view: flat list of every clip under the clips root (any depth) saved within the
	//last recentClipHours, newest first
	bool showRecentClips = false;
	int recentClipHours = 12;
	std::vector<SavedClipInfo> recentClips;
	std::string lastLoadClipError;
	void RefreshReplayMarkers(); //game thread; fills replayMarkers_ from the replay being watched
	std::mutex markersMutex_;
	std::vector<ReplayMarker> replayMarkers_;
	int markersKey_ = -1; //replay length the markers were fetched for (-1 = none); render thread
	std::atomic<bool> markersPending_{ false };
	//Before + After seconds a clip must add up to at least; ConvertClip()/ConvertClipToFile() refuse shorter ones
	static constexpr float kMinClipTotalSeconds = 5.0f;
	//non-empty = the last convert attempt was refused as too short (the settings window shows a localized message)
	std::string lastClipRangeError;
	//file name (UTF-8) of the last Save for Later attempt; lastSaveClipSucceeded says how it went
	std::string lastSaveClipMessage;
	bool lastSaveClipSucceeded = false;
	bool pendingSaveToFile = false;
	std::string pendingSaveName;
	std::string pendingSaveFolder;
	void OpenReplay(std::string oldValue, CVarWrapper cvar);
	void Bindings(std::string oldValue, CVarWrapper cvar);
	void AutoConvertCvar(std::string oldValue, CVarWrapper cvar);

	//cvar variablas

	//Function
	void AutoConvert();
	void SaveCar(int i);
	void SaveCar0();
	void SaveBall();
	void SaveBall0();
	void SaveGameInfo();
	void SpawnOneBot(int i);
	void SetCar(int i, int it);
	void SetCar0(int i);
	void SetBall();
	void SetGameInfo();

	//Event Hooks
	void SaveGameState(std::string eventName);
	void ControllGamePerTick(std::string eventName);
	void JoinPrivateMatch(std::string eventName);
	void JoinedFreeplay(std::string eventName);
	void IsInReplay(std::string eventName);
	void GoalReplayStart(std::string eventName);
	void GoalReplayEnd(std::string eventName);

	//One Time Saves
	bool hasReplaySaved = false;
	//true from when a private match is triggered until ControllGame() actually starts playback
	bool isPreparingSession = false;
	void OneTimeSaves();
	void KeyframeSaves();
	std::vector<int> Keyframes;
	std::vector<int> KeyframesInConvert;
	std::string temppath = gameWrapper->GetBakkesModPath().string() + "/data/replayClipTrainer/";
	int LobbySize = 0;
	std::vector<std::string> playerNames;
	std::vector<int> CarLayouts;
	int Gamemode = 0;
	int ReplayFPS = 30;
	std::string GamemodeStr;
	std::string Arena;
	std::string AdditionalMutators;
	//std::vector<unsigned long> StartCars;
	std::vector<int> StartTeams;
	//index into playerNames/CarLayouts/StartTeams for whoever was being spectated in the original
	//replay when conversion started - used as the default controlled player on session start
	int RecordedSpectateIndex = 0;
	//Car designs
	std::vector<LinearColor> primeColor;
	//std::vector<LinearColor> secondColor;
	//int hasColorsNormalized;

	//postitions text
	std::string positionstxt;
	void SavePositionsTxt();

	//int newCars = 0;

	//replay variables
	int curKeyframe = 0;
	int curKeyframeInConvert = 0;
	std::vector<TimeNotConverted> timesNotConverted;
	//int Frame = 0;
	int ReplayTick = 0;
	int ReplaySize = 0;
	int ConvertStartFrame = 0;
	int ConvertEndFrame = 0;
	bool useClipRange = false;
	int SavedFrames = 0;
	int TempBlueScore = 0;
	int TempOrangeScore = 0;

	int Tick = 8;
	int CarAmount = 0;
	int CountdownTime = 0;
	int TimeRemaining = 0;
	int OrangeScore = 0;
	int BlueScore = 0;
	int Score = 0;
	//bool isInOvertime = false;
	bool setBally = true;
	bool isInGoalReplay = false;

	//saves per frame
	std::vector<CarPosition> CarPositionsPerFrame;
	std::vector<BallPosition> BallPositionPerFrame;
	std::vector<GameInfo> GameInfoPerFrame;
	//std::vector<int> LobbySizePerFrame;

	//replay saves
	std::vector<int> BlueScoredFrame;
	std::vector<int> OrangeScoredFrame;

	//switch car variables
	int carit = 0;

	//JumpIn
	int BallHits = 0;
	int JumpInTick = 0;
	//ticks to keep puppeting the car (SetCar) after unpausing before handing off real control -
	//gives the engine's velocity replication time to settle so a high (e.g. supersonic) speed
	//doesn't get reset to 0 on handoff
	int jumpInSettleTicks = 0;
	//true once the private match's own kickoff countdown has finished (ReplicatedRoundCountDownNumber seen at 0). Playback waits for
	//this, but must not keep waiting on that number afterwards: a clip that starts in a recorded kickoff countdown broadcasts
	//countdown messages, and gating every tick on the number being 0 could then freeze the whole session.
	bool nativeCountdownDone_ = false;
	//debounces reclip_jumpIn's toggle bind - holding the bound gamepad button (e.g. Select/Back) can
	//re-fire the toggle every tick instead of once per physical press, making JumpIn/JumpOut flicker
	//back and forth. See JumpIn() in ReplayClipTrainer.cpp.
	std::chrono::steady_clock::time_point lastJumpInToggleTime{};
	bool suppressJumpInCallback = false;


	//Overlay
	int resX = 1920;
	int resY = 1080;

	void DrawHud(CanvasWrapper canvas);
	void DrawTime(CanvasWrapper canvas);
	void DrawPause(CanvasWrapper canvas);
	void DrawTimeline(CanvasWrapper canvas);
	void DrawTimelineSpent(CanvasWrapper canvas);
	void DrawTimelineNotConverted(CanvasWrapper canvas);
	void DrawPlayer(CanvasWrapper canvas);
	void DrawJumpIn(CanvasWrapper canvas);
	void DrawOrangeScored(CanvasWrapper canvas);
	void DrawBlueScored(CanvasWrapper canvas);
	void DrawKeyframe(CanvasWrapper canvas);

	int ConvertToScreenSizeX(int HDpixel);
	int ConvertToScreenSizeY(int HDpixel);
	float ConvertTextSizeX(float Text);
	float ConvertTextSizeY(float Text);


	//Overlay variables
	int MinutesAvailable = 0;
	int SecondsAvailable = 0;
	int MinutesDone = 0;
	int SecondsDone = 0;

private:

	void Log(std::string msg);

	// Inherited via PluginSettingsWindow
	//void RenderSettings() override;
	//std::string GetPluginName() override;

	// Inherited via PluginWindow
	bool isWindowOpen_ = false;
	bool isMinimized_ = false;
	std::string menuTitle_ = "ReplayClipTrainer";

	//---- settings window / overlays (ReplayClipTrainerGUI.cpp) ----
	void SetupUi();
	void UiKeepAlive();
	void EnsureFonts();
	void SetLanguage(bool japanese);
	void RenderControls();
	//game thread (DrawHud) -> render thread: name of the controlled player for the ImGui-drawn HUD text
	void PublishHudName(bool valid, const std::string& name, int team);
	void PublishHudTime(const std::string& text);
	void PublishHudJump(bool jumpedIn);

	static constexpr int kFontCount = 3;
	ImFont* fonts_[kFontCount] = {};       //Latin font at the baked sizes in kFontPx (ReplayClipTrainerGUI.cpp)
	ImFont* fontsJa_[kFontCount] = {};     //Japanese font at the same sizes
	bool fontsRequested_ = false;
	std::atomic<bool> jpReady_{ false };   //all Japanese sizes loaded -> Japanese text allowed
	std::vector<unsigned short> jaRanges_; //glyph ranges (ImWchar) of the Japanese font; must outlive the atlas build
	bool showKeyGuide_ = true;             //small key guide on the training HUD (cvar reclip_showKeyGuide)
	bool uiJa_ = true;                     //window + overlay language (cvar reclip_language)
	float uiScale_ = 0.85f;                //settings window size (cvar reclip_ui_scale)
	float uiOpacity_ = 0.94f;              //settings window opacity (cvar reclip_ui_opacity)
	float uiW_ = 560.f, uiH_ = 640.f;      //settings window size at 100% scale, dragged with the bottom-right grip (cvars reclip_ui_w/_h)
	//the ImGui window stays open permanently so the overlays keep rendering; this only shows/hides the settings panel
	bool controlsVisible_ = false;
	bool unloading_ = false;

	std::mutex hudMutex_;
	bool hudNameValid_ = false;
	std::string hudName_;
	int hudTeam_ = 0;
	bool hudTimeValid_ = false;
	std::string hudTime_;
	bool hudJumpValid_ = false;
	bool hudJumpedIn_ = false;
	std::chrono::steady_clock::time_point hudStamp_{};

	virtual void Render() override;
	virtual std::string GetMenuName() override;
	virtual std::string GetMenuTitle() override;
	virtual void SetImGuiContext(uintptr_t ctx) override;
	virtual bool ShouldBlockInput() override;
	virtual bool IsActiveOverlay() override;
	virtual void OnOpen() override;
	virtual void OnClose() override;
};

