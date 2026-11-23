#pragma once

static void TestWaistSupportCore()
{
	BeginTest("waist_support_default_on_and_main_thread_control");
	Core core;
	Check(core.player.controller.IsExperimentalWaistSupportEnabled() && core.guiExperimentalWaistSupport.load(), "waist support starts enabled and published");
	core.QueueExperimentalWaistSupport(false);
	core.ProcessPendingGuiCommands();
	Check(core.player.controller.IsExperimentalWaistSupportEnabled() && core.guiExperimentalWaistSupport.load(),
	      "internalized waist support remains enabled across disable requests");
	core.QueueExperimentalWaistSupport(true);
	core.ProcessPendingGuiCommands();
	Check(core.player.controller.IsExperimentalWaistSupportEnabled() && core.guiExperimentalWaistSupport.load(),
	      "main-thread consumption keeps waist support enabled and published");

	const auto directory = std::filesystem::temp_directory_path() / "gaku_waist_config_test";
	const auto path = directory / "control.json";
	RuntimeConfig config;
	config.path = path.string();
	Check(config.experimentalWaistSupport == 1, "missing declaration defaults to enabled");
	WriteTextFile(path, R"({"experimental_waist_support":{"enabled":true}})");
	Check(config.Reload() && config.experimentalWaistSupport == 1, "typed opt-in declaration loads");
	WriteTextFile(path, R"({"experimental_waist_support":{"enabled":"false"}})");
	Check(config.Reload() && config.experimentalWaistSupport == 1,
	      "invalid boolean string cannot silently change state");
	WriteTextFile(path, R"({"command":{"seq":77,"pause":true}})");
	Check(config.Reload() && config.experimentalWaistSupport == 1,
	      "unrelated transport reload preserves session setting");
	WriteTextFile(path, R"({"experimental_waist_support":{"enabled":false}})");
	Check(config.Reload() && config.experimentalWaistSupport == 1,
	      "internalized waist support ignores legacy opt-out declaration");
	RuntimeConfig defaults;
	const auto templatePath = directory / "template.json";
	WriteTextFile(templatePath, defaults.DefaultTemplate());
	defaults.path = templatePath.string();
	Check(defaults.Reload() && defaults.experimentalWaistSupport == 1,
	      "generated startup template enables waist support");
	Check(defaults.DefaultTemplate().find("experimental_waist_support") == std::string::npos,
	      "generated startup template omits obsolete waist support toggle");
	std::filesystem::remove(path);
	std::filesystem::remove(templatePath);
	std::filesystem::remove(directory);
}
