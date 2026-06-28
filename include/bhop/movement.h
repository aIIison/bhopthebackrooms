#pragma once

#include <cstdint>
#include <string>

namespace bhop {
	inline constexpr double centimeters_per_source_unit = 1.5;

	struct vec3_t {
		double x{};
		double y{};
		double z{};

		[[nodiscard]] auto horizontal_length( ) const noexcept -> double;
	};

	struct move_vars_t {
		double gravity{ 800.0 };
		double friction{ 4.0 };
		double max_speed{ 320.0 };
		double move_speed{ 250.0 };
		double accelerate{ 5.0 };
		double air_accelerate{ 10.0 };
		double stop_speed{ 75.0 };
		double jump_velocity{ 295.0 };
		double air_wish_speed_cap{ 30.0 };
		double bunny_max_speed_factor{ 1.7 };
		double bunny_speed_reduction{ 0.65 };
	};

	struct movement_input_t {
		vec3_t acceleration_cm{};
		double max_input_acceleration_cm{ 1.0 };
		double delta_seconds{};
		bool   grounded{};
		bool   jump_queued{};
	};

	[[nodiscard]] auto source_to_cm( double source_units ) noexcept -> double;
	[[nodiscard]] auto cm_to_source( double centimeters ) noexcept -> double;

	// CalcVelocity replacement. It updates horizontal velocity only; Unreal remains
	// responsible for gravity, collision, stepping, floor tests, and replication.
	[[nodiscard]] auto calculate_velocity(
					vec3_t                  velocity_cm,
					const movement_input_t& input,
					const move_vars_t&      vars ) noexcept -> vec3_t;

	[[nodiscard]] auto apply_mega_bunny_cap(
					vec3_t             velocity_cm,
					const move_vars_t& vars ) noexcept -> vec3_t;

	[[nodiscard]] auto physics_checksum( const move_vars_t& vars ) -> std::uint64_t;
	[[nodiscard]] auto checksum_hex( std::uint64_t checksum ) -> std::string;
}  // namespace bhop
