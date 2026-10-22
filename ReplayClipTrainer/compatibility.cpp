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
		//the plugin window (always open, see ReplayClipTrainerGUI.cpp) shows the question while needsGUI is set
		//DisableIncompatiblePlugin(plugin);
		return;
	}

}

void Compatibility::DisableIncompatiblePlugin(std::string plugin ) {
	_globalCvarManager->executeCommand("plugin unload "+plugin);
	LOG(plugin + " was disabled by ReplayClipTrainer");
	_globalCvarManager->executeCommand("reclip_replaySave 1");
}