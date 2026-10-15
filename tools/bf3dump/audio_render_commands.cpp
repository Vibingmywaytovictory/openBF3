// bf3dump audio census and render (audio_commands.h); filled in by the A2 compiler and mixer.
#include "audio_commands.h"

#include <cstdio>

namespace bf3dump {

int cmd_audio_census(const bf3::core::InstallReader &, std::shared_ptr<bf3::core::Install>, const std::vector<std::string> &) {
	std::fprintf(stderr, "audio census: not built yet\n");
	return 1;
}

int cmd_audio_render(const bf3::core::InstallReader &, std::shared_ptr<bf3::core::Install>, const std::vector<std::string> &) {
	std::fprintf(stderr, "audio render: not built yet\n");
	return 1;
}

} // namespace bf3dump
