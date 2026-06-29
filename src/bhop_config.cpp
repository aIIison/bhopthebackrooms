#include <algorithm>
#include <bhop/config.h>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace bhop {
	namespace {
		auto trim( std::string value ) -> std::string {
			const auto is_space = []( unsigned char c ) { return std::isspace( c ) != 0; };
			value.erase( value.begin( ), std::find_if_not( value.begin( ), value.end( ), is_space ) );
			value.erase( std::find_if_not( value.rbegin( ), value.rend( ), is_space ).base( ), value.end( ) );
			return value;
		}

		auto lower( std::string value ) -> std::string {
			std::transform( value.begin( ), value.end( ), value.begin( ), []( unsigned char c ) {
				return static_cast< char >( std::tolower( c ) );
			} );
			return value;
		}

		auto parse_bool( std::string value ) -> std::optional< bool > {
			value = lower( trim( std::move( value ) ) );
			if ( value == "true" || value == "1" || value == "yes" || value == "on" ) {
				return true;
			}
			if ( value == "false" || value == "0" || value == "no" || value == "off" ) {
				return false;
			}
			return std::nullopt;
		}

		auto parse_double( const std::string& value ) -> std::optional< double > {
			double      result{};
			const auto* begin  = value.data( );
			const auto* end    = begin + value.size( );
			const auto  parsed = std::from_chars( begin, end, result );
			if ( parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite( result ) ) {
				return std::nullopt;
			}
			return result;
		}

		auto set_bool(
		    bool&                                                 target,
		    const std::unordered_map< std::string, std::string >& entries,
		    std::string_view                                      key,
		    std::string&                                          error ) -> void {
			const auto found = entries.find( std::string{ key } );
			if ( found == entries.end( ) ) {
				return;
			}
			const auto value = parse_bool( found->second );
			if ( !value ) {
				error = "invalid boolean for " + std::string{ key };
				return;
			}
			target = *value;
		}

		auto set_double(
		    double&                                               target,
		    const std::unordered_map< std::string, std::string >& entries,
		    std::string_view                                      key,
		    std::string&                                          error ) -> void {
			const auto found = entries.find( std::string{ key } );
			if ( found == entries.end( ) ) {
				return;
			}
			const auto value = parse_double( trim( found->second ) );
			if ( !value ) {
				error = "invalid number for " + std::string{ key };
				return;
			}
			target = *value;
		}

		auto hash_bool( std::uint64_t& hash, bool value ) noexcept -> void {
			constexpr std::uint64_t prime = 1099511628211ULL;
			hash ^= value ? 1ULL : 0ULL;
			hash *= prime;
		}

		auto hash_double( std::uint64_t& hash, double value ) noexcept -> void {
			constexpr std::uint64_t prime = 1099511628211ULL;
			const auto              bits  = std::bit_cast< std::uint64_t >( value );
			for ( int byte = 0; byte < 8; ++byte ) {
				hash ^= ( bits >> ( byte * 8 ) ) & 0xffULL;
				hash *= prime;
			}
		}
	}  // namespace

	auto validate_config( const config_t& config ) -> std::string {
		const move_vars_t& v = config.move;
		if ( v.gravity <= 0.0 || v.friction < 0.0 || v.max_speed <= 0.0 ||
		     v.move_speed <= 0.0 || v.accelerate < 0.0 || v.air_accelerate < 0.0 ||
		     v.stop_speed < 0.0 || v.jump_velocity <= 0.0 ||
		     v.air_wish_speed_cap <= 0.0 || v.bunny_max_speed_factor <= 0.0 ||
		     v.bunny_speed_reduction <= 0.0 || v.bunny_speed_reduction > 1.0 ||
		     v.ladder_speed <= 0.0 || v.ladder_jump_velocity <= 0.0 ||
		     v.water_speed_factor <= 0.0 || v.water_speed_factor > 1.0 ||
		     v.water_sink_speed < 0.0 || v.water_swim_up_speed <= 0.0 ) {
			return "physics values are outside their supported range";
		}
		if ( config.duck_roll_window <= 0.0 ||
		     config.duck_roll_window > 1.0 ||
		     config.duck_roll_height <= 0.0 ||
		     config.duck_roll_height > 72.0 ) {
			return "duck-roll window or height is outside its supported range";
		}
		return {};
	}

	auto load_config( const std::filesystem::path& path ) -> config_result_t {
		config_result_t result{};
		std::ifstream   input{ path };
		if ( !input ) {
			result.error = "could not open " + path.string( );
			return result;
		}

		std::unordered_map< std::string, std::string > entries;
		std::string                                    section;
		std::string                                    line;
		std::size_t                                    line_number{};
		while ( std::getline( input, line ) ) {
			++line_number;
			line = trim( std::move( line ) );
			if ( line.empty( ) || line.front( ) == ';' || line.front( ) == '#' ) {
				continue;
			}
			if ( line.front( ) == '[' && line.back( ) == ']' ) {
				section = lower( trim( line.substr( 1, line.size( ) - 2 ) ) );
				continue;
			}
			const auto equals = line.find( '=' );
			if ( equals == std::string::npos ) {
				result.error = "malformed INI line " + std::to_string( line_number );
				return result;
			}
			const std::string key          = lower( trim( line.substr( 0, equals ) ) );
			const std::string value        = trim( line.substr( equals + 1 ) );
			entries[ section + "." + key ] = value;
		}

		std::string error;
		set_bool( result.value.enabled, entries, "general.enabled", error );
		set_bool( result.value.auto_bhop, entries, "general.autobhop", error );
		set_bool( result.value.bunnyhop_speed_cap, entries, "general.bunnyhopspeedcap", error );
		set_bool( result.value.duck_roll, entries, "general.duckroll", error );
		set_bool( result.value.raw_mouse_input, entries, "general.rawmouseinput", error );
		set_double(
		    result.value.duck_roll_window,
		    entries,
		    "duckroll.window",
		    error );
		set_double(
		    result.value.duck_roll_height,
		    entries,
		    "duckroll.height",
		    error );

		set_double( result.value.move.gravity, entries, "movevars.gravity", error );
		set_double( result.value.move.friction, entries, "movevars.friction", error );
		set_double( result.value.move.max_speed, entries, "movevars.maxspeed", error );
		set_double( result.value.move.move_speed, entries, "movevars.movespeed", error );
		set_double( result.value.move.accelerate, entries, "movevars.accelerate", error );
		set_double( result.value.move.air_accelerate, entries, "movevars.airaccelerate", error );
		set_double( result.value.move.stop_speed, entries, "movevars.stopspeed", error );
		set_double( result.value.move.jump_velocity, entries, "movevars.jumpvelocity", error );
		set_double( result.value.move.air_wish_speed_cap, entries, "movevars.airwishspeedcap", error );
		set_double( result.value.move.bunny_max_speed_factor, entries, "movevars.bunnymaxspeedfactor", error );
		set_double( result.value.move.bunny_speed_reduction, entries, "movevars.bunnyspeedreduction", error );
		set_double( result.value.move.ladder_speed, entries, "movevars.ladderspeed", error );
		set_double( result.value.move.ladder_jump_velocity, entries, "movevars.ladderjumpvelocity", error );
		set_double( result.value.move.water_speed_factor, entries, "movevars.waterspeedfactor", error );
		set_double( result.value.move.water_sink_speed, entries, "movevars.watersinkspeed", error );
		set_double( result.value.move.water_swim_up_speed, entries, "movevars.waterswimupspeed", error );

		if ( !error.empty( ) ) {
			result.error = std::move( error );
			return result;
		}
		result.error  = validate_config( result.value );
		result.loaded = result.error.empty( );
		return result;
	}

	auto config_checksum( const config_t& config ) -> std::uint64_t {
		std::uint64_t hash = physics_checksum( config.move );
		hash_bool( hash, config.enabled );
		hash_bool( hash, config.auto_bhop );
		hash_bool( hash, config.bunnyhop_speed_cap );
		hash_bool( hash, config.duck_roll );
		hash_double( hash, config.duck_roll_window );
		hash_double( hash, config.duck_roll_height );
		return hash;
	}
}  // namespace bhop
