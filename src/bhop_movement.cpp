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
