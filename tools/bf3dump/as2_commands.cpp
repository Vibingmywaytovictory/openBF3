#include "as2_commands.h"

#include "bf3/core/ui/avm1.h"
#include "bf3/core/ui/swf_library.h"

#include <charconv>
#include <cstdio>
#include <string>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace bf3dump {

namespace {

std::string movie_name(const std::string &p_name) {
	return p_name.find('/') == std::string::npos ? "ui/assets/" + p_name : p_name;
}

int census(const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	unsigned threads = 0;
	for (const std::string &a : p_args) {
		if (a.starts_with("--threads=")) {
			std::from_chars(a.data() + 10, a.data() + a.size(), threads);
		} else {
			std::fprintf(stderr, "swf as2 census: unknown argument %s\n", a.c_str());
			return 2;
		}
	}
	SwfLibraryOptions o;
	o.threads = threads;
	Result<SwfLibrary> lib = SwfLibrary::open(p_install, o);
	if (!lib) {
		std::fprintf(stderr, "swf as2 census: %s\n", lib.error().message.c_str());
		return 1;
	}
	Result<Avm1Census> c = run_avm1_census(*lib, threads);
	if (!c) {
		std::fprintf(stderr, "swf as2 census: %s\n", c.error().message.c_str());
		return 1;
	}
	std::printf("%s", format_avm1_census(*c).c_str());
	return c->errors == 0 ? 0 : 1;
}

int listing(const std::shared_ptr<Install> &p_install, const std::string &p_movie, const std::string &p_class) {
	Result<SwfLibrary> lib = SwfLibrary::open(p_install);
	if (!lib) {
		std::fprintf(stderr, "swf as2: %s\n", lib.error().message.c_str());
		return 1;
	}
	const std::string name = movie_name(p_movie);
	Result<std::shared_ptr<const SwfMovie>> movie = lib->load(name);
	if (!movie) {
		std::fprintf(stderr, "swf as2 %s: %s\n", name.c_str(), movie.error().message.c_str());
		return 1;
	}
	Result<Avm1Movie> avm1 = read_avm1(**movie);
	if (!avm1) {
		std::fprintf(stderr, "swf as2 %s: %s\n", name.c_str(), avm1.error().message.c_str());
		return 1;
	}
	const Avm1Movie &m = *avm1;
	if (!p_class.empty()) {
		const Avm1Class *c = m.find_class(p_class);
		if (c == nullptr) {
			std::fprintf(stderr, "swf as2 %s: no class %s\n", name.c_str(), p_class.c_str());
			return 1;
		}
		std::printf("%s", format_avm1_class(*c).c_str());
		std::printf("%s", format_avm1_body_header(**movie, m, c->body).c_str());
		std::printf("%s", format_avm1_code(m.bodies[c->body].code).c_str());
		return 0;
	}
	std::printf("movie %s: %zu bodies; DoAction %u tags %llu bytes, DoInitAction %u tags %llu bytes, clip actions %u tags %llu bytes\n", name.c_str(),
			m.bodies.size(), m.do_action_tags, (unsigned long long)m.do_action_bytes, m.do_init_action_tags, (unsigned long long)m.do_init_action_bytes,
			m.clip_action_tags, (unsigned long long)m.clip_action_bytes);
	for (const std::string &p : m.packages) {
		std::printf("package %s\n", p.c_str());
	}
	for (const Avm1Registration &r : m.registrations) {
		std::printf("registerClass \"%s\" -> %s (body %u)\n", r.linkage.c_str(), r.class_name.c_str(), r.body);
	}
	for (const Avm1Class &c : m.classes) {
		std::printf("%s", format_avm1_class(c).c_str());
	}
	for (uint32_t i = 0; i < m.bodies.size(); ++i) {
		std::printf("%s", format_avm1_body_header(**movie, m, i).c_str());
		std::printf("%s", format_avm1_code(m.bodies[i].code).c_str());
	}
	return 0;
}

}

int cmd_swf_as2(const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	if (p_args.empty()) {
		return 2;
	}
	if (p_args[0] == "census") {
		return census(p_install, std::vector<std::string>(p_args.begin() + 1, p_args.end()));
	}
	if (p_args.size() == 1) {
		return listing(p_install, p_args[0], {});
	}
	if (p_args.size() == 3 && p_args[1] == "--class") {
		return listing(p_install, p_args[0], p_args[2]);
	}
	return 2;
}

}
