#include <algorithm>
#include <bhop/movement.h>
#include <bit>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace bhop {
	namespace {
		constexpr double epsilon = 1.0e-9;

		auto fnv_mix( std::uint64_t& hash, std::uint64_t value ) noexcept -> void {
			constexpr std::uint64_t prime = 1099511628211ULL;
			for ( int byte = 0; byte < 8; ++byte ) {
				hash ^= ( value >> ( byte * 8 ) ) & 0xffULL;
				hash *= prime;
			}
		}

		auto accelerate(
		    vec3_t        velocity,
		    const vec3_t& wish_direction,
		    double        wish_speed_cm,
		    double        acceleration,
		    double        delta_seconds ) noexcept -> vec3_t {
			const double current_speed =
			    velocity.x * wish_direction.x + velocity.y * wish_direction.y;
			const double add_speed = wish_speed_cm - current_speed;
			if ( add_speed <= 0.0 ) {
				return velocity;
			}

			const double acceleration_speed =
			    std::min( acceleration * delta_seconds * wish_speed_cm, add_speed );
			velocity.x += acceleration_speed * wish_direction.x;
			velocity.y += acceleration_speed * wish_direction.y;
			return velocity;
		}

		auto air_accelerate(
		    vec3_t             velocity,
		    const vec3_t&      wish_direction,
		    double             wish_speed_cm,
		    const move_vars_t& vars,
		    double             delta_seconds ) noexcept -> vec3_t {
			const double capped_wish_speed =
			    std::min( wish_speed_cm, source_to_cm( vars.air_wish_speed_cap ) );
			const double current_speed =
			    velocity.x * wish_direction.x + velocity.y * wish_direction.y;
			const double add_speed = capped_wish_speed - current_speed;
			if ( add_speed <= 0.0 ) {
				return velocity;
			}

			// GoldSrc deliberately uses the uncapped wishspeed in this product.
			const double acceleration_speed = std::min(
			    vars.air_accelerate * wish_speed_cm * delta_seconds,
			    add_speed );
			velocity.x += acceleration_speed * wish_direction.x;
			velocity.y += acceleration_speed * wish_direction.y;
			return velocity;
		}

		auto apply_friction(
		    vec3_t             velocity,
		    const move_vars_t& vars,
		    double             delta_seconds ) noexcept -> vec3_t {
			const double speed = velocity.horizontal_length( );
			if ( speed < 0.1 ) {
				return velocity;
			}

			const double control   = std::max( speed, source_to_cm( vars.stop_speed ) );
			const double drop      = control * vars.friction * delta_seconds;
			const double new_speed = std::max( 0.0, speed - drop );
			const double scale     = new_speed / speed;
			velocity.x *= scale;
			velocity.y *= scale;
			return velocity;
		}

		[[nodiscard]] auto dot( const vec3_t& left, const vec3_t& right ) noexcept -> double {
			return left.x * right.x + left.y * right.y + left.z * right.z;
		}

		[[nodiscard]] auto cross( const vec3_t& left, const vec3_t& right ) noexcept -> vec3_t {
			return {
				left.y * right.z - left.z * right.y,
				left.z * right.x - left.x * right.z,
				left.x * right.y - left.y * right.x,
			};
		}

		[[nodiscard]] auto normalized( vec3_t value ) noexcept -> vec3_t {
			const double length = std::sqrt( dot( value, value ) );
			if ( length <= epsilon ) {
				return {};
			}
			value.x /= length;
			value.y /= length;
			value.z /= length;
			return value;
		}
	}  // namespace

	auto vec3_t::horizontal_length( ) const noexcept -> double {
		return std::hypot( x, y );
	}

	auto source_to_cm( double source_units ) noexcept -> double {
		return source_units * centimeters_per_source_unit;
	}

	auto cm_to_source( double centimeters ) noexcept -> double {
		return centimeters / centimeters_per_source_unit;
	}

	auto calculate_velocity(
	    vec3_t                  velocity_cm,
	    const movement_input_t& input,
	    const move_vars_t&      vars ) noexcept -> vec3_t {
		const double dt = std::max( 0.0, input.delta_seconds );
		if ( dt <= 0.0 ) {
			return velocity_cm;
		}

		if ( input.grounded && !input.jump_queued ) {
			velocity_cm = apply_friction( velocity_cm, vars, dt );
		}

		const double raw_length =
		    std::hypot( input.acceleration_cm.x, input.acceleration_cm.y );
		if ( raw_length <= epsilon || input.max_input_acceleration_cm <= epsilon ) {
			return velocity_cm;
		}

		const vec3_t wish_direction{
			input.acceleration_cm.x / raw_length,
			input.acceleration_cm.y / raw_length,
			0.0,
		};
		const double input_amount =
		    std::clamp( raw_length / input.max_input_acceleration_cm, 0.0, 1.0 );
		const double wish_speed_source =
		    std::min( vars.move_speed * input_amount, vars.max_speed );
		const double wish_speed_cm = source_to_cm( wish_speed_source );

		if ( input.grounded ) {
			return accelerate(
			    velocity_cm,
			    wish_direction,
			    wish_speed_cm,
			    vars.accelerate,
			    dt );
		}
		return air_accelerate(
		    velocity_cm,
		    wish_direction,
		    wish_speed_cm,
		    vars,
		    dt );
	}

	auto apply_mega_bunny_cap( vec3_t velocity_cm, const move_vars_t& vars ) noexcept -> vec3_t {
		const double speed = velocity_cm.horizontal_length( );
		const double maximum =
		    source_to_cm( vars.bunny_max_speed_factor * vars.max_speed );
		if ( speed <= maximum || maximum <= 0.0 ) {
			return velocity_cm;
		}

		const double scale = ( maximum / speed ) * vars.bunny_speed_reduction;
		velocity_cm.x *= scale;
		velocity_cm.y *= scale;
		return velocity_cm;
	}

	auto calculate_ladder_velocity( const ladder_input_t& input, const move_vars_t& vars ) noexcept -> ladder_result_t {
		const vec3_t normal = normalized( input.ladder_normal );
		if ( dot( normal, normal ) <= epsilon ) {
			return {};
		}

		if ( input.jump_queued ) {
			const double jump_speed = source_to_cm( vars.ladder_jump_velocity );
			return {
				.velocity_cm = { normal.x * jump_speed, normal.y * jump_speed, normal.z * jump_speed },
				.detached    = true,
			};
		}

		double speed = std::min( vars.ladder_speed, vars.max_speed );
		if ( input.crouched ) {
			speed *= 1.0 / 3.0;
		}

		const double forward = std::clamp( input.forward_move, -1.0, 1.0 ) * speed;
		const double side    = std::clamp( input.side_move, -1.0, 1.0 ) * speed;
		const vec3_t wish{
			input.view_forward.x * forward + input.view_right.x * side,
			input.view_forward.y * forward + input.view_right.y * side,
			input.view_forward.z * forward + input.view_right.z * side,
		};

		const double normal_speed = dot( wish, normal );
		const vec3_t into_ladder{ normal.x * normal_speed, normal.y * normal_speed, normal.z * normal_speed };
		const vec3_t lateral{ wish.x - into_ladder.x, wish.y - into_ladder.y, wish.z - into_ladder.z };
		const vec3_t ladder_side = normalized( cross( { 0.0, 0.0, 1.0 }, normal ) );
		const vec3_t ladder_up   = cross( normal, ladder_side );
		vec3_t       velocity{
			lateral.x - ladder_up.x * normal_speed,
			lateral.y - ladder_up.y * normal_speed,
			lateral.z - ladder_up.z * normal_speed,
		};

		if ( input.on_floor && normal_speed > 0.0 ) {
			velocity.x += normal.x * vars.ladder_speed;
			velocity.y += normal.y * vars.ladder_speed;
			velocity.z += normal.z * vars.ladder_speed;
		}

		velocity.x = source_to_cm( velocity.x );
		velocity.y = source_to_cm( velocity.y );
		velocity.z = source_to_cm( velocity.z );
		return { .velocity_cm = velocity };
	}

	auto calculate_water_velocity( vec3_t velocity_cm, const water_input_t& input, const move_vars_t& vars ) noexcept -> vec3_t {
		const double dt = std::max( 0.0, input.delta_seconds );
		if ( dt <= 0.0 ) {
			return velocity_cm;
		}

		if ( input.swim_up ) {
			velocity_cm.z = source_to_cm( vars.water_swim_up_speed );
		}

		const double forward = std::clamp( input.forward_move, -1.0, 1.0 ) * vars.move_speed;
		const double side    = std::clamp( input.side_move, -1.0, 1.0 ) * vars.move_speed;
		const double up      = std::clamp( input.up_move, -1.0, 1.0 ) * vars.move_speed;
		vec3_t       wish_velocity{
			input.view_forward.x * forward + input.view_right.x * side,
			input.view_forward.y * forward + input.view_right.y * side,
			input.view_forward.z * forward + input.view_right.z * side,
		};
		if ( forward == 0.0 && side == 0.0 && up == 0.0 ) {
			wish_velocity.z -= vars.water_sink_speed;
		} else {
			wish_velocity.z += up;
		}

		double wish_speed = std::sqrt( dot( wish_velocity, wish_velocity ) );
		if ( wish_speed > vars.max_speed ) {
			const double scale = vars.max_speed / wish_speed;
			wish_velocity.x *= scale;
			wish_velocity.y *= scale;
			wish_velocity.z *= scale;
			wish_speed = vars.max_speed;
		}
		wish_speed *= vars.water_speed_factor;

		const double speed     = std::sqrt( dot( velocity_cm, velocity_cm ) );
		double       new_speed = speed;
		if ( speed > epsilon ) {
			new_speed          = std::max( 0.0, speed - dt * speed * vars.friction );
			const double scale = new_speed / speed;
			velocity_cm.x *= scale;
			velocity_cm.y *= scale;
			velocity_cm.z *= scale;
		}

		if ( wish_speed < 0.1 ) {
			return velocity_cm;
		}

		const double add_speed = source_to_cm( wish_speed ) - new_speed;
		if ( add_speed <= 0.0 ) {
			return velocity_cm;
		}

		const vec3_t wish_direction     = normalized( wish_velocity );
		const double acceleration_speed = std::min(
		    vars.accelerate * source_to_cm( wish_speed ) * dt,
		    add_speed );
		velocity_cm.x += acceleration_speed * wish_direction.x;
		velocity_cm.y += acceleration_speed * wish_direction.y;
		velocity_cm.z += acceleration_speed * wish_direction.z;
		return velocity_cm;
	}

	auto physics_checksum( const move_vars_t& vars ) -> std::uint64_t {
		std::uint64_t hash = 14695981039346656037ULL;
		const double  values[]{
			vars.gravity,
			vars.friction,
			vars.max_speed,
			vars.move_speed,
			vars.accelerate,
			vars.air_accelerate,
			vars.stop_speed,
			vars.jump_velocity,
			vars.air_wish_speed_cap,
			vars.bunny_max_speed_factor,
			vars.bunny_speed_reduction,
			vars.ladder_speed,
			vars.ladder_jump_velocity,
			vars.water_speed_factor,
			vars.water_sink_speed,
			vars.water_swim_up_speed,
		};
		for ( const double value : values ) {
			fnv_mix( hash, std::bit_cast< std::uint64_t >( value ) );
		}
		return hash;
	}

	auto checksum_hex( std::uint64_t checksum ) -> std::string {
		std::ostringstream output;
		output << std::hex << std::setfill( '0' ) << std::setw( 16 ) << checksum;
		return output.str( );
	}
}  // namespace bhop
