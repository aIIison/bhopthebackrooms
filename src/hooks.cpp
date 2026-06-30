#include "hooks.h"

#include "mod.h"
#include "util/process.h"
#include "util/scanner.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <algorithm>
#include <functional>
#include <vector>

namespace bhop::native {
	namespace {
		using util::e_pointer_kind;
		using util::instruction_view_t;
		using util::pointer_map_t;

		struct call_collector_t {
			std::vector< std::size_t >& slots;
			std::vector< void* >&       direct_targets;

			auto operator( )( const instruction_view_t& view, const pointer_map_t& pointers ) -> void {
				if ( const auto slot = util::virtual_call_slot( view, pointers ) ) {
					slots.push_back( *slot );
				} else if ( void* target = util::direct_call_target( view ) ) {
					direct_targets.push_back( target );
				}
			}
		};

		struct falling_matcher_t {
			std::int64_t movement_mode_offset;
			bool         reads_mode{};
			bool         compares_falling{};

			auto operator( )( const instruction_view_t& view, const pointer_map_t& pointers ) -> void {
				reads_mode |= util::reads_field( view, pointers, e_pointer_kind::context, movement_mode_offset );
				compares_falling |= util::uses_immediate( view, movement_falling, ZYDIS_MNEMONIC_CMP );
			}

			[[nodiscard]] auto matches( const std::uint8_t* code ) -> bool {
				pointer_map_t pointers{};
				pointers[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context;
				return util::scan_function( code, 64, pointers, std::ref( *this ) ) &&
				    reads_mode && compares_falling;
			}
		};

		struct can_crouch_matcher_t {
			void**       vtable;
			std::size_t  falling_slot;
			std::size_t  ground_slot;
			std::int64_t updated_component_offset;
			bool         calls_falling{};
			bool         calls_ground{};
			bool         reads_component{};

			auto operator( )( const instruction_view_t& view, const pointer_map_t& pointers ) -> void {
				reads_component |= util::reads_field( view, pointers, e_pointer_kind::context, updated_component_offset );
				if ( const auto slot = util::virtual_call_slot( view, pointers ) ) {
					calls_falling |= *slot == falling_slot;
					calls_ground |= *slot == ground_slot;
				} else if ( void* target = util::direct_call_target( view ) ) {
					calls_falling |= target == vtable[ falling_slot ];
					calls_ground |= target == vtable[ ground_slot ];
				}
			}

			[[nodiscard]] auto matches( const std::uint8_t* code ) -> bool {
				pointer_map_t pointers{};
				pointers[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context;
				return util::scan_function( code, 192, pointers, std::ref( *this ) ) &&
				    calls_falling && calls_ground && reads_component;
			}
		};

		struct physics_volume_matcher_t {
			std::int64_t water_volume_offset;
			bool         reads_water{};
			bool         uses_swimming{};

			auto operator( )( const instruction_view_t& view, const pointer_map_t& pointers ) -> void {
				reads_water |= util::reads_field( view, pointers, e_pointer_kind::volume, water_volume_offset );
				uses_swimming |= util::uses_immediate( view, movement_swimming );
			}

			[[nodiscard]] auto matches( const std::uint8_t* code ) -> bool {
				pointer_map_t pointers{};
				pointers[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context;
				pointers[ ZYDIS_REGISTER_RDX ] = e_pointer_kind::volume;
				return util::scan_function( code, 768, pointers, std::ref( *this ) ) &&
				    reads_water && uses_swimming;
			}
		};
	}  // namespace

	auto resolve_virtual_slot( UFunction* function, void** vtable, bool log_failure )
	    -> std::optional< std::size_t > {
		if ( !function ) {
			return std::nullopt;
		}
		const auto* code = reinterpret_cast< const std::uint8_t* >( function->GetFuncPtr( ) );
		if ( !util::is_executable_game_address( code ) ) {
			return std::nullopt;
		}

		// Reflected exec wrappers eventually dispatch through `this` or call a
		// compiler thunk directly. Supporting both layouts avoids hard-coding a
		// slot while still requiring one unambiguous target.
		pointer_map_t pointers{};
		pointers[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context;
		std::vector< std::size_t > slots;
		std::vector< void* >       direct_targets;
		call_collector_t           collector{ slots, direct_targets };
		if ( !util::scan_function( code, 1024, pointers, std::ref( collector ) ) ) {
			return std::nullopt;
		}

		if ( slots.empty( ) && vtable ) {
			for ( void* target : direct_targets ) {
				for ( std::size_t slot = 0; slot < util::vtable_slot_count; ++slot ) {
					if ( util::canonical_function_address( vtable[ slot ] ) ==
					     util::canonical_function_address( target ) ) {
						slots.push_back( slot );
					}
				}
			}
			std::sort( slots.begin( ), slots.end( ) );
			slots.erase( std::unique( slots.begin( ), slots.end( ) ), slots.end( ) );
		}

		if ( slots.size( ) != 1 ) {
			if ( log_failure ) {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] Refused virtual hook: exec wrapper yielded {} context-vtable call candidates.\n" ),
				    slots.size( ) );
			}
			return std::nullopt;
		}
		return slots.front( );
	}

	auto resolve_can_crouch_slot(
	    void**       vtable,
	    std::int64_t movement_mode_offset,
	    std::int64_t updated_component_offset ) -> std::optional< std::size_t > {
		if ( !vtable || movement_mode_offset < 0 || updated_component_offset < 0 ) {
			return std::nullopt;
		}

		const auto is_falling = [ movement_mode_offset ]( const std::uint8_t* code ) {
			return falling_matcher_t{ movement_mode_offset }.matches( code );
		};
		const auto falling_slots = util::find_vtable_matches( vtable, false, false, is_falling );
		if ( falling_slots.size( ) != 1 || falling_slots.front( ) + 1 >= util::vtable_slot_count ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Refused CanCrouch hook: IsFalling scan yielded {} candidates.\n" ),
			    falling_slots.size( ) );
			return std::nullopt;
		}

		// UE 4.27 places IsMovingOnGround immediately after IsFalling. The
		// supported build is accepted only when CanCrouch calls both predicates
		// and also reads UpdatedComponent.
		const auto falling_slot  = falling_slots.front( );
		const auto ground_slot   = falling_slot + 1;
		const auto is_can_crouch = [ = ]( const std::uint8_t* code ) {
			auto matcher = can_crouch_matcher_t{
				vtable,
				falling_slot,
				ground_slot,
				updated_component_offset,
			};
			return matcher.matches( code );
		};
		const auto candidates = util::find_vtable_matches( vtable, false, true, is_can_crouch );
		if ( candidates.size( ) != 1 ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Refused CanCrouch hook: semantic scan yielded {} candidates.\n" ),
			    candidates.size( ) );
			return std::nullopt;
		}
		return candidates.front( );
	}

	auto resolve_physics_volume_changed_slot(
	    void**       vtable,
	    std::int64_t water_volume_offset ) -> std::optional< std::size_t > {
		if ( !vtable || water_volume_offset < 0 ) {
			return std::nullopt;
		}

		// PhysicsVolumeChanged is the only movement virtual that reads the new
		// volume's bWaterVolume flag and selects MOVE_Swimming.
		const auto is_physics_volume_changed = [ water_volume_offset ]( const std::uint8_t* code ) {
			auto matcher = physics_volume_matcher_t{ water_volume_offset };
			return matcher.matches( code );
		};
		const auto candidates = util::find_vtable_matches( vtable, true, true, is_physics_volume_changed );
		if ( candidates.size( ) != 1 ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Refused PhysicsVolumeChanged hook: semantic scan yielded {} candidates.\n" ),
			    candidates.size( ) );
			return std::nullopt;
		}
		return candidates.front( );
	}
}  // namespace bhop::native
