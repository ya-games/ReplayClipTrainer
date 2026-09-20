#include "pch.h"
#include "ReplayClipTrainer.h"
#include "ClipFile.h"
#include "compatibility.h"
#include <cmath>
#include <filesystem>

//a slider that actually snaps to increments of "step" while dragging (SliderFloat's grab tracks
//the raw mouse position during an active drag, so rounding the value after the fact doesn't
//visibly snap) - implemented as a SliderInt over step-count, with -/+ buttons for fine adjustment.
static void SteppedFloatSlider(const char* cvarName, const char* label, float minVal, float maxVal, float step) {
	CVarWrapper cvar = _globalCvarManager->getCvar(cvarName);
	int maxSteps = static_cast<int>(std::round(maxVal / step));
	int steps = static_cast<int>(std::round(cvar.getFloatValue() / step));
	if (steps < 0) { steps = 0; }
	if (steps > maxSteps) { steps = maxSteps; }
	bool changed = false;

	ImGui::TextWrapped("%s: %.2f", label, cvar.getFloatValue());
	ImGui::PushID(cvarName);
	if (ImGui::Button("-")) { steps--; changed = true; }
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x));
	if (ImGui::SliderInt("##slider", &steps, 0, maxSteps, "")) { changed = true; }
	ImGui::SameLine();
	if (ImGui::Button("+")) { steps++; changed = true; }
	ImGui::PopID();

	if (changed) {
		if (steps < 0) { steps = 0; }
		if (steps > maxSteps) { steps = maxSteps; }
		float value = steps * step;
		if (value < minVal) { value = minVal; }
		if (value > maxVal) { value = maxVal; }
		cvar.setValue(value);
	}
}

// Settings panel shown inside the ReplayClipTrainer window (f2 -> plugins -> ReplayClipTrainer)
void ReplayClipTrainer::RenderSettingsGui() {
	bool conversionInProgress = cvarManager->getCvar("reclip_replaySave").getBoolValue();

	ImGui::TextWrapped("How to use: while watching a replay, scrub to the moment you want, then click \"Convert Clip Around Current Position\" to train it right away, or \"Save Clip to File\" to save it for later.");
	ImGui::TextWrapped("Do not change Teams, restart Match, or go to Spectate while converting.");
	if (conversionInProgress) {
		ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "A clip is currently being converted - please wait (or press Cancel on the progress bar).");
	}

	if (gameWrapper->IsInReplay() && !conversionInProgress) {
		ImGui::Spacing();
		ImGui::Text("Replay Position:");
		ImGui::Separator();

		ReplayServerWrapper replayServer = gameWrapper->GetGameEventAsReplay();
		int totalFrames = replayServer.GetReplay().GetNumFrames();
		int currentFrame = replayServer.GetCurrentReplayFrame();
		float fps = replayServer.GetReplayFPS();

		//keep the slider synced to live playback position, except while the user is actively dragging it
		//(otherwise the live update would fight the drag)
		static int seekFrame = 0;
		static bool seekActive = false;
		if (!seekActive) { seekFrame = currentFrame; }

		int curSec = fps > 0.0f ? static_cast<int>(currentFrame / fps) : 0;
		int totSec = fps > 0.0f ? static_cast<int>(totalFrames / fps) : 0;
		std::string label = std::to_string(curSec / 60) + ":" + (curSec % 60 < 10 ? "0" : "") + std::to_string(curSec % 60)
			+ " / " + std::to_string(totSec / 60) + ":" + (totSec % 60 < 10 ? "0" : "") + std::to_string(totSec % 60);

		ImGui::SetNextItemWidth(-1);
		ImGui::SliderInt("##replaySeek", &seekFrame, 0, totalFrames > 0 ? totalFrames - 1 : 0, label.c_str());
		seekActive = ImGui::IsItemActive();
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			//deferred like every other action here - calling replay-state-changing engine calls
			//synchronously from inside the ImGui callback has been unreliable in this codebase before
			int targetFrame = seekFrame;
			gameWrapper->SetTimeout([this, targetFrame](GameWrapper*) {
				if (gameWrapper->IsInReplay()) { gameWrapper->GetGameEventAsReplay().SkipToFrame(targetFrame); }
				}, 0.0f);
		}
	}

	ImGui::Spacing();
	ImGui::Text("Clip Conversion:");
	ImGui::Separator();
	SteppedFloatSlider("reclip_clipSecondsBefore", "Seconds Before Scrub Position", 0.0f, 20.0f, 0.5f);
	SteppedFloatSlider("reclip_clipSecondsAfter", "Seconds After Scrub Position", 0.0f, 20.0f, 0.5f);
	if (conversionInProgress) {
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Convert Clip Around Current Position");
	}
	else if (ImGui::Button("Convert Clip Around Current Position")) { gameWrapper->SetTimeout([this](GameWrapper*) { cvarManager->executeCommand("reclip_convertClip 1"); }, 0.0f); }
	ImGui::TextWrapped("Converts only a short clip around the replay's current scrub position (scrub to the moment you want first, then click this). Opens a private match and starts training right away.");
	if (!lastClipRangeError.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", lastClipRangeError.c_str());
	}

	ImGui::Spacing();
	static char clipNameBuf[128] = "";
	ImGui::InputTextWithHint("##clipName", "Clip name (optional, auto-named if left blank)", clipNameBuf, sizeof(clipNameBuf));
	if (conversionInProgress) {
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Save Clip to File");
	}
	else if (ImGui::Button("Save Clip to File")) {
		std::string name(clipNameBuf);
		gameWrapper->SetTimeout([this, name](GameWrapper*) { this->ConvertClipToFile(name); }, 0.0f);
		clipNameBuf[0] = '\0';
	}
	ImGui::TextWrapped("Converts and saves the clip to a file WITHOUT opening a private match, so you can save several scenes from one replay and pick one to train later.");
	if (!lastSaveClipMessage.empty()) {
		ImVec4 color = lastSaveClipSucceeded ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
		ImGui::TextColored(color, "%s", lastSaveClipMessage.c_str());
	}

	ImGui::Spacing();
	ImGui::Text("Saved Clips:");
	ImGui::Separator();
	if (ImGui::Button("Refresh List")) { RefreshSavedClipsList(); }
	ImGui::SameLine();
	if (ImGui::Button("Open Clips Folder")) { gameWrapper->SetTimeout([this](GameWrapper*) { this->OpenClipsFolder(); }, 0.0f); }
	ImGui::SameLine();
	static bool showNoClipWarning = false;
	if (conversionInProgress) {
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Retry Last Clip");
	}
	else if (ImGui::Button("Retry Last Clip")) {
		if (hasReplaySaved) {
			showNoClipWarning = false;
			gameWrapper->SetTimeout([this](GameWrapper*) { cvarManager->executeCommand("reclip_openReplay 1"); }, 0.0f);
		}
		else {
			showNoClipWarning = true;
		}
	}
	if (showNoClipWarning) {
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "No clip converted or loaded yet - nothing to retry.");
	}
	if (savedClips.empty() && savedClipFolders.empty()) {
		ImGui::TextWrapped("No saved clips yet.");
	}
	std::string clipToDelete;
	std::string folderToEnter;
	bool goBackFolder = false;
	//two-click confirmation for deleting a clip, to avoid an accidental single misclick losing it -
	//static so it persists across frames until confirmed or cancelled
	static std::string pendingDeleteConfirm;
	ImGui::BeginChild("SavedClipsList", ImVec2(0, 180), true);
	if (!currentClipFolder.empty()) {
		if (ImGui::Button("<- Back")) { goBackFolder = true; }
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "/%s", currentClipFolder.c_str());
	}
	for (const std::string& folderName : savedClipFolders) {
		ImGui::PushID(("folder_" + folderName).c_str());
		if (ImGui::Button(("[Folder] " + folderName).c_str())) { folderToEnter = folderName; }
		ImGui::PopID();
	}
	for (const SavedClipInfo& clip : savedClips) {
		std::string filename = std::filesystem::u8path(clip.path).stem().u8string();
		ImGui::PushID(clip.path.c_str());
		if (clip.isCompatible && !conversionInProgress) {
			if (ImGui::Button("Load")) {
				std::string clipPath = clip.path;
				gameWrapper->SetTimeout([this, clipPath](GameWrapper*) { this->LoadClipAndOpen(clipPath); }, 0.0f);
			}
		}
		else {
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Load");
		}
		ImGui::SameLine();
		if (pendingDeleteConfirm == clip.path) {
			if (ImGui::Button("Confirm Delete")) {
				clipToDelete = clip.path;
				pendingDeleteConfirm.clear();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) { pendingDeleteConfirm.clear(); }
		}
		else if (ImGui::Button("Delete")) {
			pendingDeleteConfirm = clip.path;
		}
		ImGui::SameLine();
		std::string label = filename;
		if (clip.isCompatible) {
			if (!clip.saveTimeStr.empty()) { label += " - " + clip.saveTimeStr; }
			if (clip.playerCount > 0) { label += " - " + std::to_string(clip.playerCount) + "p"; }
			ImGui::Text("%s", label.c_str());
		}
		else {
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s (incompatible)", filename.c_str());
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
	//navigation/deletion are applied here rather than inline above, since RefreshSavedClipsList()
	//mutates savedClips/savedClipFolders - doing that mid-iteration over those same vectors would be UB
	if (goBackFolder) {
		pendingDeleteConfirm.clear();
		size_t lastSlash = currentClipFolder.find_last_of('/');
		currentClipFolder = (lastSlash == std::string::npos) ? "" : currentClipFolder.substr(0, lastSlash);
		RefreshSavedClipsList();
	}
	else if (!folderToEnter.empty()) {
		pendingDeleteConfirm.clear();
		currentClipFolder = currentClipFolder.empty() ? folderToEnter : currentClipFolder + "/" + folderToEnter;
		RefreshSavedClipsList();
	}
	if (!clipToDelete.empty()) {
		std::filesystem::remove(std::filesystem::u8path(clipToDelete));
		RefreshSavedClipsList();
	}
	if (!lastLoadClipError.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Failed to load clip: %s", lastLoadClipError.c_str());
	}

	ImGui::Spacing();
	ImGui::Text("Settings:");
	ImGui::Separator();
	SteppedFloatSlider("reclip_resolution", "Conversion Resolution", 0.1f, 2.0f, 0.1f);
	bool settingLimitedBoost = cvarManager->getCvar("reclip_limitedBoost").getBoolValue();
	if (ImGui::Checkbox("Limited Boost in Replay", &settingLimitedBoost)) { cvarManager->getCvar("reclip_limitedBoost").setValue(settingLimitedBoost); }
	bool settingNormalTraining = cvarManager->getCvar("reclip_inputToUnpause").getBoolValue();
	if (ImGui::Checkbox("Unpause on Controller Input When Jumping In", &settingNormalTraining)) { cvarManager->getCvar("reclip_inputToUnpause").setValue(settingNormalTraining); }
	bool settingHUD = cvarManager->getCvar("reclip_showHud").getBoolValue();
	if (ImGui::Checkbox("Show HUD in ReplayClipTrainer", &settingHUD)) { cvarManager->getCvar("reclip_showHud").setValue(settingHUD); }
	bool settingDisableGoal = cvarManager->getCvar("reclip_disableGoal").getBoolValue();
	if (ImGui::Checkbox("Disable Goal Replay/Explosion", &settingDisableGoal)) { cvarManager->getCvar("reclip_disableGoal").setValue(settingDisableGoal); }

	ImGui::Spacing();
	ImGui::Text("Bindings:");
	ImGui::Separator();
	if (ImGui::Button("Apply Standard Bindings")) { gameWrapper->SetTimeout([this](GameWrapper*) { cvarManager->getCvar("reclip_bindings").setValue(true); }, 0.0f); }
	ImGui::TextWrapped("Standard bindings: DPadRight/RightArrow skips 10s forward, DPadLeft/LeftArrow skips 5s back, DPadUp/UpArrow next player, DPadDown/DownArrow previous player, LeftStickPress/B pauses (resets shot), Back/Select/V toggles JumpIn mode (unpause after). Change bindings in the BakkesMod bindings tab.");

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Text("ReplayClipTrainer by ya-games (github.com/ya-games)");
	ImGui::TextWrapped("Based on JumpInReplay by Atomus48 (github.com/Atomus48/JumpInReplay)");
	std::string versionText = std::string("Plugin Version: ") + plugin_version;
	ImGui::Text("%s", versionText.c_str());
}

// Draws the current player's name over the in-game HUD using a CJK-capable font (canvas.DrawString cannot render Japanese glyphs).
// Falls back to doing nothing (leaving the existing canvas.DrawString HUD text as-is) if no CJK font could be loaded.
void ReplayClipTrainer::RenderPlayerNameOverlay() {
	if (cjkFont_ == nullptr) { return; }
	if (cvarManager->getCvar("reclip_showHud").getBoolValue() == false) { return; }
	if (gameWrapper->IsInGame() == false || hasReplaySaved == false || cvarManager->getCvar("reclip_controllGame").getBoolValue() == false) { return; }
	if (carit < 0 || carit >= static_cast<int>(playerNames.size()) || carit >= static_cast<int>(StartTeams.size())) { return; }

	//mirror DrawPlayer()'s HD-space anchor (650, 920 of a 1920x1080 canvas)
	ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	float posX = (650.0f / 1920.0f) * displaySize.x;
	float posY = (920.0f / 1080.0f) * displaySize.y;

	ImGui::PushFont(cjkFont_);
	ImGui::SetNextWindowPos(ImVec2(posX, posY));
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
	if (ImGui::Begin("ReplayClipTrainerPlayerNameOverlay", nullptr, flags)) {
		bool isBlue = (StartTeams.at(carit) == 0);
		ImVec4 color = isBlue ? ImVec4(12 / 255.0f, 136 / 255.0f, 252 / 255.0f, 1.0f) : ImVec4(252 / 255.0f, 124 / 255.0f, 12 / 255.0f, 1.0f);
		ImGui::TextColored(color, "%s", playerNames.at(carit).c_str());
	}
	ImGui::End();
	ImGui::PopFont();
}

// Shows a "preparing" message while the private match is loading/spawning bots, between triggering
// reclip_privateMatch and ControllGame() actually starting playback (map load + bot spawn takes a few seconds).
void ReplayClipTrainer::RenderPreparingOverlay() {
	if (!isPreparingSession) { return; }

	ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.3f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
	if (ImGui::Begin("ReplayClipTrainerPreparing", nullptr, flags)) {
		int dotCount = (static_cast<int>(ImGui::GetTime() / 0.5f)) % 4;
		std::string dots(dotCount, '.');
		std::string text = "Preparing training session" + dots;
		ImGui::SetWindowFontScale(2.5f);
		ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s", text.c_str());
		ImGui::SetWindowFontScale(1.0f);
	}
	ImGui::End();
}

// Shows a small progress bar while a replay is being converted (reclip_replaySave is running).
void ReplayClipTrainer::RenderConversionProgressOverlay() {
	if (cvarManager->getCvar("reclip_replaySave").getBoolValue() == false) { return; }
	if (gameWrapper->IsInReplay() == false) { return; }

	int currentFrame = gameWrapper->GetGameEventAsReplay().GetCurrentReplayFrame();
	int totalFrames = ConvertEndFrame - ConvertStartFrame;
	float progress = 0.0f;
	if (totalFrames > 0) {
		progress = static_cast<float>(currentFrame - ConvertStartFrame) / static_cast<float>(totalFrames);
	}
	if (progress < 0.0f) { progress = 0.0f; }
	if (progress > 1.0f) { progress = 1.0f; }

	ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.08f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
	if (ImGui::Begin("ReplayClipTrainerConversionProgress", nullptr, flags)) {
		ImGui::Text("Converting Replay...  (%d%%)", static_cast<int>(progress * 100.0f));
		ImGui::ProgressBar(progress, ImVec2(300, 0));
		if (ImGui::Button("Cancel")) {
			gameWrapper->SetTimeout([this](GameWrapper*) { this->CancelConversion(); }, 0.0f);
		}
	}
	ImGui::End();
}

// Do ImGui rendering here
void ReplayClipTrainer::Render()
{
	ImGui::SetNextWindowSize(ImVec2(500, 520), ImGuiCond_FirstUseEver);
	if (ImGui::Begin(menuTitle_.c_str(), &isWindowOpen_, ImGuiWindowFlags_None))
	{
		if (compatibility.needsGUI) {
			compatibility.ImGuiForAsk();
			ImGui::Separator();
		}
		RenderSettingsGui();
	}
	ImGui::End();

	RenderPlayerNameOverlay();
	RenderConversionProgressOverlay();
	RenderPreparingOverlay();

	if (!isWindowOpen_)
	{
		cvarManager->executeCommand("togglemenu " + GetMenuName());
	}
}

// Name of the menu that is used to toggle the window.
std::string ReplayClipTrainer::GetMenuName()
{
	return "ReplayClipTrainer";
}

// Title to give the menu
std::string ReplayClipTrainer::GetMenuTitle()
{
	return menuTitle_;
}

// Don't call this yourself, BM will call this function with a pointer to the current ImGui context
void ReplayClipTrainer::SetImGuiContext(uintptr_t ctx)
{
	ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));

	//NOTE: loading a custom (CJK) font here via io.Fonts->AddFontFromFileTTF crashes BakkesMod.
	//Adding a font invalidates the shared ImGui font atlas, and only BakkesMod's own renderer
	//backend (which this plugin doesn't control) can rebuild/re-upload it - without that,
	//BakkesMod's next ImGui frame hits "Font atlas not built!" and asserts/crashes.
	//cjkFont_ is intentionally left null; RenderPlayerNameOverlay() no-ops in that case.
}

// Should events such as mouse clicks/key inputs be blocked so they won't reach the game
bool ReplayClipTrainer::ShouldBlockInput()
{
	return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
}

// Return true if window should be interactive
bool ReplayClipTrainer::IsActiveOverlay()
{
	return true;
}

// Called when window is opened
void ReplayClipTrainer::OnOpen()
{
	isWindowOpen_ = true;
}

// Called when window is closed
void ReplayClipTrainer::OnClose()
{
	isWindowOpen_ = false;
}

