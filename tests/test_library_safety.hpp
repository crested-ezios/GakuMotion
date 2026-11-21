#pragma once

namespace LibraryDeleteFixture
{
	inline void WriteClip(const std::filesystem::path& path)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write("GKCLIPCL", 8);
		auto write = [&](auto value) { file.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
		write(uint32_t{4}); write(30.0f); write(uint32_t{2}); write(2.0f / 30.0f);
		write(uint32_t{1}); write(.083f); write(uint16_t{0}); write(uint8_t{4}); write(uint8_t{0}); file.write("Hips", 4);
		for (int i = 0; i < 2; ++i) { write(GakuMotion::Vector3{}); write(GakuMotion::Quaternion::Identity()); }
		write(uint8_t{0}); write(uint32_t{0});
	}
}

static void TestLibrarySafety(const std::string& repoRoot)
{
	BeginTest("library_confirmed_permanent_delete");
	namespace fs = std::filesystem;
	const auto root = fs::absolute(fs::path(repoRoot) / "build" / "tests" / ("gkmotion_library_safety_" +
		std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
	const auto track = root / "motions" / "A";
	LibraryDeleteFixture::WriteClip(track / "motion.gkclip");
	WriteTextFile(track / "custom" / "note.txt", "synthetic imported bytes");
	Core core; core.Initialize(MotionPathToUtf8(root));
	const auto read = [](const fs::path& path) {
		std::ifstream in(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	};
	core.SavePlaylist();
	const auto before = read(MotionPathFromUtf8(core.playlistPath));
	const auto clipBefore = read(track / "motion.gkclip");
	Check(!core.DeleteMotionFiles("A") && read(MotionPathFromUtf8(core.playlistPath)) == before &&
		read(track / "motion.gkclip") == clipBefore && !fs::exists(root / "motions" / ".gakumotion-trash"),
		"unconfirmed delete performs zero disk/list writes");
	WriteTextFile(MotionPathFromUtf8(core.playlistPath), R"({"version":1,"mode":1,"order":["A"],"removed":["A"]})");
	Core legacy; legacy.Initialize(MotionPathToUtf8(root));
	Check(FindPlaylistIndex(legacy.motions, "A") >= 0, "legacy removed keys no longer hide local tracks");
	Check(legacy.SavePlaylist() && read(MotionPathFromUtf8(legacy.playlistPath)).find("removed") == std::string::npos,
		"new playlist saves retire the removed field");
	LibraryDeleteFixture::WriteClip(root / "motions" / ".gakumotion-trash" / "motion.gkclip");
	core.RefreshMotionLibrary();
	Check(core.motions.size() == 1, "legacy recovery storage remains excluded and untouched");

	LibraryDeleteFixture::WriteClip(root / "outside" / "motion.gkclip");
	const auto originalDirectory = core.motions[0].dir;
	core.motions[0].dir = MotionPathToUtf8(root / "outside");
	Check(!core.DeleteMotionFiles("A", true) && fs::exists(root / "outside" / "motion.gkclip"),
		"outside directory cannot be deleted even when its entry is forged");
	core.motions[0].dir = originalDirectory;
	std::string error;
	fs::path checkedRoot, checkedChild;
	Check(LibraryDelete::ResolveRoot(root / "motions", checkedRoot, error) &&
		!LibraryDelete::ResolveChild(checkedRoot, root / "outside", checkedChild, error) &&
		!LibraryDelete::ResolveChild(checkedRoot, root / "motions", checkedChild, error) &&
		!LibraryDelete::ResolveChild(checkedRoot, root / "motions" / ".gakumotion-trash", checkedChild, error),
		"delete boundary rejects outside paths, library root and legacy recovery storage");
	std::error_code linkError;
	fs::create_directory_symlink(root / "outside", track / "external-link", linkError);
	if (!linkError)
	{
		Check(!core.DeleteMotionFiles("A", true) && read(track / "motion.gkclip") == clipBefore &&
			fs::exists(root / "outside" / "motion.gkclip"), "nested symlink refuses deletion before any file is removed");
		fs::remove(track / "external-link");
	}
	else std::printf("  [SKIP] symlink fixture unavailable: %s\n", linkError.message().c_str());

#ifdef _WIN32
	const auto existingPlaylist = read(MotionPathFromUtf8(core.playlistPath));
	const auto playlistHandle = CreateFileW(MotionPathFromUtf8(core.playlistPath).c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, 0, nullptr);
	Check(playlistHandle != INVALID_HANDLE_VALUE, "lock synthetic playlist against replacement");
	if (playlistHandle != INVALID_HANDLE_VALUE)
	{
		Check(!core.DeleteMotionFiles("A", true) && read(MotionPathFromUtf8(core.playlistPath)) == existingPlaylist &&
			read(track / "motion.gkclip") == clipBefore, "playlist persistence failure prevents permanent deletion");
		CloseHandle(playlistHandle);
	}
	const auto lockedFile = track / "custom" / "note.txt";
	const auto handle = CreateFileW(lockedFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	Check(handle != INVALID_HANDLE_VALUE, "lock nested synthetic file against deletion");
	if (handle != INVALID_HANDLE_VALUE)
	{
		Check(!core.DeleteMotionFiles("A", true) && FindPlaylistIndex(core.motions, "A") >= 0 &&
			read(track / "motion.gkclip") == clipBefore && read(lockedFile) == "synthetic imported bytes",
			"locked nested file is detected before any irreversible file deletion");
		CloseHandle(handle);
	}
	SetFileAttributesW(lockedFile.c_str(), FILE_ATTRIBUTE_READONLY);
	Check(!core.DeleteMotionFiles("A", true) && read(track / "motion.gkclip") == clipBefore,
		"readonly nested file is detected before any irreversible file deletion");
	SetFileAttributesW(lockedFile.c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
	const auto playlistPath = core.playlistPath;
	fs::create_directory(root / "blocked-playlist");
	core.playlistPath = MotionPathToUtf8(root / "blocked-playlist");
	Check(!core.DeleteMotionFiles("A", true) && FindPlaylistIndex(core.motions, "A") >= 0 &&
		read(track / "motion.gkclip") == clipBefore, "invalid playlist destination preserves track directory");
	core.playlistPath = playlistPath;
	core.importBusy = true;
	Check(!core.DeleteMotionFiles("A", true) && fs::exists(track / "motion.gkclip"), "import blocks deletion");
	core.importBusy = false;
	Check(core.SelectMotionByIndex(0), "load synthetic track before deletion");
	core.player.Play();
	const bool deleted = core.DeleteMotionFiles("A", true);
	if (!deleted) std::printf("  DELETE FAILURE: %s\n", core.playlistNotice.c_str());
	Check(deleted && core.motions.empty() && core.player.state == PlaybackState::Stopped &&
		core.player.loadedMotionFile.empty() && !fs::exists(track),
		"confirmed delete permanently removes complete track tree and releases loaded playback");
	Check(fs::exists(root / "motions" / ".gakumotion-trash" / "motion.gkclip") &&
		fs::exists(root / "outside" / "motion.gkclip"), "delete preserves legacy storage and outside files");
	core.RefreshMotionLibrary();
	Core restarted; restarted.Initialize(MotionPathToUtf8(root));
	Check(core.motions.empty() && restarted.motions.empty(), "deleted track does not reappear after refresh or restart");

	// Group-song permanent deletion removes nested positions/slot_{k}/ completely
	const auto grp = root / "motions" / "GroupTrack";
	LibraryDeleteFixture::WriteClip(grp / "motion.gkclip");
	LibraryDeleteFixture::WriteClip(grp / "positions" / "slot_2" / "motion.gkclip");
	WriteTextFile(grp / "positions" / "slot_2" / "bone_overrides.json", "{}");
	LibraryDeleteFixture::WriteClip(grp / "positions" / "slot_3" / "motion.gkclip");
	core.RefreshMotionLibrary();
	Check(core.motions.size() == 1 && core.motions[0].isGroup && core.motions[0].positionCount == 3,
		"group song discovered before deletion");
	Check(core.DeleteMotionFiles("GroupTrack", true) && !fs::exists(grp) && core.motions.empty(),
		"confirmed delete removes group song and nested positions/slot_{k}/ completely");

	core.player.Stop(); legacy.player.Stop(); restarted.player.Stop();
	fs::remove_all(root); // Unique synthetic fixture created by this test only.
}
