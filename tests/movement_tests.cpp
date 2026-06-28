#include <bhop/config.h>
#include <bhop/movement.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
	int failures{};

	auto check( bool condition, const std::string& name ) -> void {
		if ( !condition ) {
			++failures;
			std::cerr << "FAIL: " << name << '\n';
		}
	}

	auto near( double actual, double expected, double tolerance = 1.0e-6 ) -> bool {
		return std::abs( actual - expected ) <= tolerance;
	}
}  // namespace

int main( ) {
	using namespace bhop;

	check(
					near(
									source_to_cm( 320.0 ),
									320.0 * centimeters_per_source_unit ),
					"Source-to-Unreal conversion" );
	check(
					near(
									cm_to_source( 320.0 * centimeters_per_source_unit ),
									320.0 ),
					"Unreal-to-Source conversion" );

	move_vars_t            vars{};
	const movement_input_t full_forward{
		.acceleration_cm           = { 2048.0, 0.0, 0.0 },
		.max_input_acceleration_cm = 2048.0,
		.delta_seconds             = 0.01,
		.grounded                  = true,
		.jump_queued               = true,
	};
	const vec3_t first_ground = calculate_velocity( {}, full_forward, vars );
	check( near( cm_to_source( first_ground.x ), 12.5 ), "GoldSrc ground acceleration" );
	check( near( first_ground.y, 0.0 ), "Ground acceleration direction" );

	const movement_input_t diagonal{
		.acceleration_cm           = { 2048.0, 2048.0, 0.0 },
		.max_input_acceleration_cm = std::hypot( 2048.0, 2048.0 ),
		.delta_seconds             = 0.01,
		.grounded                  = true,
		.jump_queued               = true,
	};
	const vec3_t diagonal_velocity = calculate_velocity( {}, diagonal, vars );
	check(
					near( cm_to_source( diagonal_velocity.horizontal_length( ) ), 12.5 ),
					"Diagonal input normalization" );

	const movement_input_t air{
		.acceleration_cm           = { 0.0, 2048.0, 0.0 },
		.max_input_acceleration_cm = 2048.0,
		.delta_seconds             = 0.01,
		.grounded                  = false,
		.jump_queued               = false,
	};
	const vec3_t air_velocity =
					calculate_velocity( { source_to_cm( 250.0 ), 0.0, 0.0 }, air, vars );
	check(
					near( cm_to_source( air_velocity.y ), 25.0 ),
					"GoldSrc air acceleration uses uncapped wishspeed" );

	const movement_input_t friction_only{
		.acceleration_cm           = {},
		.max_input_acceleration_cm = 2048.0,
		.delta_seconds             = 0.01,
		.grounded                  = true,
		.jump_queued               = false,
	};
	const vec3_t friction_velocity =
					calculate_velocity( { source_to_cm( 250.0 ), 0.0, 0.0 }, friction_only, vars );
	check( near( cm_to_source( friction_velocity.x ), 240.0 ), "Ground friction" );

	const movement_input_t reverse_ground{
		.acceleration_cm           = { -2048.0, 0.0, 0.0 },
		.max_input_acceleration_cm = 2048.0,
		.delta_seconds             = 0.01,
		.grounded                  = true,
		.jump_queued               = false,
	};
	const vec3_t reversed = calculate_velocity(
					{ source_to_cm( 250.0 ), 0.0, 0.0 },
					reverse_ground,
					vars );
	check(
					near( cm_to_source( reversed.x ), 227.5 ),
					"Ground reversal applies friction before acceleration" );

	const vec3_t uncapped{ source_to_cm( 700.0 ), 0.0, 123.0 };
	const vec3_t capped = apply_mega_bunny_cap( uncapped, vars );
	check(
					near( cm_to_source( capped.horizontal_length( ) ), 1.7 * 320.0 * 0.65 ),
					"Mega-bunny speed cap" );
	check( near( capped.z, uncapped.z ), "Mega-bunny cap preserves vertical velocity" );

	const auto temp = std::filesystem::temp_directory_path( ) / "etb_bhop_test.ini";
	{
		std::ofstream ini{ temp };
		ini << "[General]\nEnabled=true\nAutoBhop=false\n"
									"BunnyhopSpeedCap=true\nDuckRoll=true\nRawMouseInput=false\n"
									"[MoveVars]\nGravity=800\nAirAccelerate=12\n"
									"[DuckRoll]\nWindow=0.35\nHeight=18\n";
	}
	const config_result_t loaded = load_config( temp );
	std::filesystem::remove( temp );
	check( loaded.loaded, "INI loads" );
	check( !loaded.value.auto_bhop, "INI boolean" );
	check( loaded.value.duck_roll, "Duck roll enabled" );
	check( !loaded.value.raw_mouse_input, "Raw mouse input option" );
	check( near( loaded.value.duck_roll_window, 0.35 ), "Duck roll window" );
	check( near( loaded.value.duck_roll_height, 18.0 ), "Duck roll height" );
	check( near( loaded.value.move.air_accelerate, 12.0 ), "INI movevar" );
	const auto hash_a  = config_checksum( loaded.value );
	config_t   changed = loaded.value;
	changed.move.air_accelerate += 1.0;
	check( hash_a != config_checksum( changed ), "Physics checksum changes" );
	check( checksum_hex( hash_a ).size( ) == 16, "Checksum formatting" );

	if ( failures != 0 ) {
		std::cerr << failures << " test(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "All movement tests passed\n";
	return EXIT_SUCCESS;
}
