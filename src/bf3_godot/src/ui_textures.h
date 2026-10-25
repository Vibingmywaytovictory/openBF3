#pragma once

#include "bf3/core/install.h"
#include "bf3/core/ui/swf_image.h"
#include "bf3/render/texture_prep.h"

#include <godot_cpp/classes/texture2d.hpp>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace bf3::godot_side {

class Bf3UiTextures final : public bf3::core::ui::SwfImageSource {
public:
	static std::shared_ptr<Bf3UiTextures> shared();

	explicit Bf3UiTextures(std::shared_ptr<const bf3::core::Install> p_install);
	~Bf3UiTextures() override;
	Bf3UiTextures(const Bf3UiTextures &) = delete;
	Bf3UiTextures &operator=(const Bf3UiTextures &) = delete;

	bf3::core::ui::SwfImageInfo request(std::string_view p_res_name) override;
	godot::Ref<godot::Texture2D> texture(std::string_view p_res_name) const;
	uint32_t decoded() const;
	uint32_t failed() const;

private:
	struct Entry {
		bf3::core::ui::SwfImageInfo info;
		std::optional<bf3::render::PreparedTexture> prepared;
		bool decoded = false;
		godot::Ref<godot::Texture2D> texture;
	};
	void run();

	std::shared_ptr<const bf3::core::Install> install_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<std::string> queue_;
	std::map<std::string, Entry, std::less<>> entries_;
	bool stop_ = false;
	uint32_t decoded_ = 0;
	uint32_t failed_ = 0;
	std::thread worker_;
};

}
