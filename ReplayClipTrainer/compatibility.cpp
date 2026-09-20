#include "pch.h"
#include "compatibility.h"
#include "ReplayClipTrainer.h"



bool Compatibility::CheckForIncompatibility() {
	for (int i = 0;i < IncompatiblePlugin.size();i++) {
		for (int k = 0;k < CompGameWrapper->GetPluginManager().GetLoadedPlugins()->size();k++) {
			if (IncompatiblePlugin.at(i)== CompGameWrapper->GetPluginManager().GetLoadedPlugins()->at(k).get()->_filename) {
				LOG("found incompatiable plugin");
				AskForDisable(IncompatiblePlugin.at(i));
				return false;
			}
		}
	}
	return true;
}

void Compatibility::AskForDisable(std::string plugin) {
	currentNotCompatiblePlugin = plugin;
	if (_globalCvarManager->getCvar("reclip_doNotAskForDisableOfIncompatiblePlugins").getBoolValue()==true){ 
		DisableIncompatiblePlugin(plugin);
	}
	else {
		LOG("does ask for disable");
		needsGUI = true;
		_globalCvarManager->executeCommand("togglemenu ReplayClipTrainer");
		//DisableIncompatiblePlugin(plugin);
		return;
	}

}

void Compatibility::ImGuiForAsk() {

	std::string errorMsg = "the plugin " + currentNotCompatiblePlugin + " is unfortunatly incompatible with ReplayClipTrainer do you want to disable " + currentNotCompatiblePlugin+"?";
	ImGui::Text(errorMsg.c_str());

	std::string disableMsg = "disable " + currentNotCompatiblePlugin;
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.7f, 0.0f, 1.f));
	if(ImGui::Button(disableMsg.c_str())){ 
		_globalCvarManager->executeCommand("plugin unload " + currentNotCompatiblePlugin); 
		LOG(currentNotCompatiblePlugin + " was disabled by ReplayClipTrainer");
		_globalCvarManager->executeCommand("togglemenu ReplayClipTrainer");
		//_globalCvarManager->executeCommand("reclip_replaySave 1");
		needsGUI = false;
	}
	ImGui::PopStyleColor();

	ImGui::SameLine();

	std::string notdisableMsg = "do not disable " + currentNotCompatiblePlugin;
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.0f, 0.0f, 1.f));
	if(ImGui::Button(notdisableMsg.c_str())){
		_globalCvarManager->executeCommand("reclip_replaySave 0"); 
		_globalCvarManager->executeCommand("togglemenu ReplayClipTrainer");
		needsGUI = false;
	}
	ImGui::PopStyleColor();

	bool askForDisable = _globalCvarManager->getCvar("reclip_doNotAskForDisableOfIncompatiblePlugins").getBoolValue();
	if (ImGui::Checkbox("do not ask again for disabling incompatible plugins", &askForDisable)) { _globalCvarManager->getCvar("reclip_doNotAskForDisableOfIncompatiblePlugins").setValue(askForDisable); }
}

void Compatibility::DisableIncompatiblePlugin(std::string plugin ) {
	_globalCvarManager->executeCommand("plugin unload "+plugin);
	LOG(plugin + " was disabled by ReplayClipTrainer");
	_globalCvarManager->executeCommand("reclip_replaySave 1");
}