#pragma once

#include <bhop/movement.h>
#include <filesystem>
#include <string>

namespace bhop {
	struct config_t {
		bool        enabled{ true };
		bool        auto_bhop{ true };
		bool        bunnyhop_speed_cap{ true };
		bool        duck_roll{ true };
		bool        raw_mouse_input{ true };
		double      duck_roll_window{ 0.4 };
		double      duck_roll_height{ 18.0 };
		move_vars_t move{};
	};

	struct config_result_t {
		config_t    value{};
		bool        loaded{};
		std::string error{};
	};

	[[nodiscard]] auto load_config( const std::filesystem::path& path ) -> config_result_t;
	[[nodiscard]] auto validate_config( const config_t& config ) -> std::string;
	[[nodiscard]] auto config_checksum( const config_t& config ) -> std::uint64_t;
}  // namespace bhop
