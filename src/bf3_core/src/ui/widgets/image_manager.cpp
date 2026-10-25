#include "image_manager.h"

#include "hud_widget_support.h"

namespace bf3::core::ui {

void ImageManagerWidget::construct(HudWidgetHost &r_host) {
	r_host.self().set_visible(false);
}

void ImageManagerWidget::on_clip_load(HudWidgetHost &r_host) {
	const As2Clip &self = r_host.self();
	holder_ = r_host.create_empty_movie_clip(self, "imgHolder", self.next_highest_depth());
	start_w_ = self.width();
	start_h_ = self.height();
	self.set_xscale(100.0f);
	self.set_yscale(100.0f);
	const As2Clip size = self.child("sizeClip");
	size.set_width(start_w_);
	size.set_height(start_h_);
	size.set_visible(false);
	const As2Clip spinner = self.child("spinnerMc");
	spinner.set_xscale(100.0f);
	spinner.set_yscale(100.0f);
	spinner.set_x(start_w_ * 0.5f);
	spinner.set_y(start_h_ * 0.5f);
}

void ImageManagerWidget::initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) {
	const std::string *url = p_init.property("StaticUrl");
	if (url != nullptr && !url->empty()) {
		update_image_url(r_host, *url);
	}
	const std::string *resize = p_init.property("ResizeToTarget");
	resize_to_target = resize != nullptr && *resize == "true";
	setup_refresh_data(*this, r_host, p_init.data);
	const As2Value *visible = p_init.value("Visible");
	r_host.self().set_visible(visible == nullptr || !visible->defined() || visible->truthy());
}

void ImageManagerWidget::update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) {
	if (p_key == "ImageUrl") {
		update_image_url(r_host, p_value.to_string());
	} else if (p_key == "Visible" && p_value.defined()) {
		r_host.self().set_visible(p_value.truthy());
	}
}

void ImageManagerWidget::advance(HudWidgetHost &r_host, double) {
	if (!texture_pending_ || !holder_.exists()) {
		return;
	}
	if (!texture_requested_) {
		texture_requested_ = true;
		loader_.load_clip(holder_, url_);
	}
	const As2ClipLoader::State st = loader_.poll();
	if (st == As2ClipLoader::State::Loading) {
		return;
	}
	texture_pending_ = false;
	texture_done(r_host, st == As2ClipLoader::State::Loaded);
}

void ImageManagerWidget::update_image_url(HudWidgetHost &r_host, std::string_view p_url) {
	if (contains_image_) {
		if (p_url == url_) {
			return;
		}
		unload_image(r_host);
	}
	if (currently_loading_) {
		queued_ = std::string(p_url);
		return;
	}
	queued_.reset();
	url_ = std::string(p_url);
	load_image(r_host);
}

void ImageManagerWidget::load_image(HudWidgetHost &r_host) {
	loaded_ = false;
	currently_loading_ = true;
	if (url_.empty() || url_.find('/') == std::string::npos) {
		dequeue(r_host);
		return;
	}
	if (r_host.self().scene()->image_source() == nullptr) {
		handle_call("LoadError1");
		dequeue(r_host);
	} else {
		texture_pending_ = true;
		texture_requested_ = false;
		show_spinner(r_host);
		handle_call("LoadStarted");
	}
	r_host.self().set_visible(true);
}

void ImageManagerWidget::texture_done(HudWidgetHost &r_host, bool p_result) {
	hide_spinner(r_host);
	if (!p_result) {
		dequeue(r_host);
		handle_call("LoadError2");
		return;
	}
	image_fully_loaded(r_host);
}

void ImageManagerWidget::image_fully_loaded(HudWidgetHost &r_host) {
	const As2Clip &mc = holder_;
	if (resize_to_target || resize_to_target_width || resize_to_target_height) {
		const double w = mc.width();
		const double h = mc.height();
		double scale = 1.0;
		if (resize_to_target) {
			scale = std::min(double(start_w_) / w, double(start_h_) / h);
		} else if (resize_to_target_width) {
			scale = double(start_w_) / w;
		} else {
			scale = double(start_h_) / h;
		}
		mc.set_width(float(as2_round(w * scale)));
		mc.set_height(float(as2_round(h * scale)));
	}
	if (center_image) {
		mc.set_x(start_w_ * 0.5f - mc.width() * 0.5f);
		mc.set_y(start_h_ * 0.5f - mc.height() * 0.5f);
	} else if (center_image_on_origo) {
		mc.set_x(0.0f - mc.width() * 0.5f);
		mc.set_y(0.0f - mc.height() * 0.5f);
	}
	loaded_ = true;
	contains_image_ = true;
	dequeue(r_host);
	handle_call("ImageLoaded");
}

void ImageManagerWidget::unload_image(HudWidgetHost &) {
	if (contains_image_ || currently_loading_) {
		loader_.unload_clip();
	}
	texture_pending_ = false;
	texture_requested_ = false;
	contains_image_ = false;
	loaded_ = false;
	currently_loading_ = false;
	queued_.reset();
	handle_call("UnLoadImage");
}

void ImageManagerWidget::show(HudWidgetHost &r_host) {
	r_host.self().set_visible(true);
}

void ImageManagerWidget::hide(HudWidgetHost &r_host) {
	r_host.self().set_visible(false);
}

void ImageManagerWidget::dequeue(HudWidgetHost &r_host) {
	currently_loading_ = false;
	if (queued_ && !queued_->empty()) {
		const std::string next = *queued_;
		update_image_url(r_host, next);
		queued_.reset();
	}
}

void ImageManagerWidget::handle_call(std::string_view p_message) {
	if (callback_) {
		callback_(p_message, *this);
	}
}

void ImageManagerWidget::show_spinner(HudWidgetHost &r_host) {
	const As2Clip spinner = r_host.self().child("spinnerMc");
	spinner.play();
	spinner.set_visible(true);
}

void ImageManagerWidget::hide_spinner(HudWidgetHost &r_host) {
	const As2Clip spinner = r_host.self().child("spinnerMc");
	spinner.goto_and_stop(1);
	spinner.set_visible(false);
}

void register_hud_widgets_image_manager(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Image.ImageManager", [] { return std::make_unique<ImageManagerWidget>(); });
}

}
