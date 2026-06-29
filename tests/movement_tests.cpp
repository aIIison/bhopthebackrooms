#include <bhop/config.h>
#include <bhop/movement.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
	int failures{ };

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

	move_vars_t            vars{ };
	const movement_input_t full_forward{
		.acceleration_cm           = { 2048.0, 0.0, 0.0 },
		.max_input_acceleration_cm = 2048.0,
		.delta_seconds             = 0.01,
		.grounded                  = true,
		.jump_queued               = true,
	};
	const vec3_t first_ground = calculate_velocity( { }, full_forward, vars );
	check( near( cm_to_source( first_ground.x ), 12.5 ), "GoldSrc ground acceleration" );
	check( near( first_ground.y, 0.0 ), "Ground acceleration direction" );

	const movement_input_t diagonal{
		.acceleration_cm           = { 2048.0, 2048.0, 0.0 },
		.max_input_acceleration_cm = std::hypot( 2048.0, 2048.0 ),
		.delta_seconds             = 0.01,
		.grounded                  = true,
		.jump_queued               = true,
	};
	const vec3_t diagonal_velocity = calculate_velocity( { }, diagonal, vars );
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
		.acceleration_cm           = { },
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

	const ladder_input_t climb_input{
		.view_forward  = { -1.0, 0.0, 0.0 },
		.view_right    = { 0.0, 1.0, 0.0 },
		.ladder_normal = { 1.0, 0.0, 0.0 },
		.forward_move  = 1.0,
	};
	const auto climb = calculate_ladder_velocity( climb_input, vars );
	check( near( cm_to_source( climb.velocity_cm.z ), 200.0 ), "GoldSrc ladder climb speed" );
	check( near( climb.velocity_cm.x, 0.0 ), "Ladder climb stays in ladder plane" );

	auto strafe_input         = climb_input;
	strafe_input.forward_move = 0.0;
	strafe_input.side_move    = 1.0;
	const auto strafe         = calculate_ladder_velocity( strafe_input, vars );
	check( near( cm_to_source( strafe.velocity_cm.y ), 200.0 ), "GoldSrc ladder strafing" );

	auto crouch_climb          = climb_input;
	crouch_climb.crouched      = true;
	const auto crouched_ladder = calculate_ladder_velocity( crouch_climb, vars );
	check( near( cm_to_source( crouched_ladder.velocity_cm.z ), 200.0 / 3.0 ), "Crouched ladder speed" );

	auto floor_climb         = crouch_climb;
	floor_climb.view_forward = { 1.0, 0.0, 0.0 };
	floor_climb.on_floor     = true;
	const auto floor_push    = calculate_ladder_velocity( floor_climb, vars );
	check( near( cm_to_source( floor_push.velocity_cm.x ), 200.0 ), "GoldSrc ladder floor push is not crouch-scaled" );

	auto jump_off          = climb_input;
	jump_off.jump_queued   = true;
	const auto ladder_jump = calculate_ladder_velocity( jump_off, vars );
	check( ladder_jump.detached, "Ladder jump detaches" );
	check( near( cm_to_source( ladder_jump.velocity_cm.x ), 270.0 ), "GoldSrc ladder jump-off speed" );

	const water_input_t idle_water{
		.view_forward  = { 1.0, 0.0, 0.0 },
		.view_right    = { 0.0, 1.0, 0.0 },
		.delta_seconds = 0.1,
	};
	const auto sinking = calculate_water_velocity( { }, idle_water, vars );
	check( near( cm_to_source( sinking.z ), -24.0 ), "GoldSrc idle water sinking" );

	const auto friction_only_water = calculate_water_velocity( { source_to_cm( 100.0 ), 0.0, 0.0 }, idle_water, vars );
	check( near( cm_to_source( friction_only_water.x ), 60.0 ), "GoldSrc full-vector water friction" );
	check( near( friction_only_water.z, 0.0 ), "Water acceleration compares against total speed" );

	auto forward_water         = idle_water;
	forward_water.forward_move = 1.0;
	const auto water_accel     = calculate_water_velocity( { }, forward_water, vars );
	check( near( cm_to_source( water_accel.x ), 100.0 ), "GoldSrc water acceleration" );

	auto swim_up       = idle_water;
	swim_up.swim_up    = true;
	const auto swimming_up = calculate_water_velocity( { }, swim_up, vars );
	check( near( cm_to_source( swimming_up.z ), 60.0 ), "GoldSrc water jump impulse precedes friction" );

	auto swim_down      = idle_water;
	swim_down.up_move   = -1.0;
	const auto swimming_down = calculate_water_velocity( { }, swim_down, vars );
	check( near( cm_to_source( swimming_down.z ), -100.0 ), "Water down input" );

	auto shark_input          = swim_up;
	shark_input.delta_seconds = 0.01;
	const auto sharking       = calculate_water_velocity( { source_to_cm( 500.0 ), 0.0, 0.0 }, shark_input, vars );
	check( near( cm_to_source( sharking.x ), 480.0 ), "Sharking retains horizontal speed through water friction" );
	check( near( cm_to_source( sharking.z ), 96.0 ), "Sharking repeats the water jump impulse" );

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
