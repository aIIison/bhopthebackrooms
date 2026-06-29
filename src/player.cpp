#include "mod.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace bhop::native {
	[[nodiscard]] auto c_etb_bhop_mod::character_is_swimming( UObject* owner ) const -> bool {
		if ( !owner ) {
			return false;
		}
		auto**      movement_storage = owner->GetValuePtrByPropertyNameInChain< UObject* >( STR( "CharacterMovement" ) );
		UObject*    movement         = movement_storage ? *movement_storage : nullptr;
		const auto* mode             = property_value< std::uint8_t >( movement, movement_properties_.movement_mode );
		return water_states_.contains( owner ) ||
		    ( mode && *mode == movement_swimming ) ||
		    bool_value( owner, character_properties_.has_water_physics );
	}

	[[nodiscard]] auto c_etb_bhop_mod::character_movement_blocked( UObject* owner ) const -> bool {
		return !owner ||
		    !bool_value( owner, character_properties_.can_move, false ) ||
		    bool_value( owner, character_properties_.is_dead ) ||
		    bool_value( owner, character_properties_.is_climbing ) ||
		    bool_value( owner, character_properties_.is_climbing_ladder ) ||
		    bool_value( owner, character_properties_.is_balancing ) ||
		    bool_value( owner, character_properties_.is_falling_balance ) ||
		    bool_value( owner, character_properties_.is_pushing );
	}

	[[nodiscard]] auto c_etb_bhop_mod::should_allow_goldsrc_crouch( UObject* movement ) const -> bool {
		if ( !config_.enabled || !movement || !movement_properties_.character_owner ) {
			return false;
		}
		auto** owner_storage = property_value< UObject* >( movement, movement_properties_.character_owner );
		return owner_storage && *owner_storage &&
		    ( ladder_states_.contains( *owner_storage ) || character_is_swimming( *owner_storage ) );
	}

	[[nodiscard]] auto c_etb_bhop_mod::begin_water_movement( UObject* movement, UObject* new_volume ) -> bool {
		if ( !config_.enabled || !movement || !new_volume || !movement_properties_.character_owner ) {
			return false;
		}
		auto**   owner_storage = property_value< UObject* >( movement, movement_properties_.character_owner );
		UObject* owner         = owner_storage ? *owner_storage : nullptr;
		if ( owner && water_states_.contains( owner ) ) {
			return true;
		}

		if ( !water_volume_property_ ) {
			water_volume_property_ = bool_property( new_volume, STR( "bWaterVolume" ) );
		}
		if ( !bool_value( new_volume, water_volume_property_ ) ) {
			return false;
		}

		if ( character_movement_blocked( owner ) || ladder_states_.contains( owner ) ) {
			return false;
		}

		restore_properties( movement, states_[ movement ] );
		water_states_[ owner ] = {};
		if ( !set_movement_mode( movement, movement_flying ) ) {
			water_states_.erase( owner );
			return false;
		}
		if ( !water_logged_ ) {
			RC::Output::send< RC::LogLevel::Normal >( STR( "[bhop] GoldSrc water state active.\n" ) );
			water_logged_ = true;
		}
		return true;
	}

	auto c_etb_bhop_mod::handle_calc_velocity(
	    UObject* movement,
	    float    delta_seconds,
	    float    friction,
	    bool     fluid,
	    float    braking_deceleration ) -> void {
		if ( !hook_ready_ || !movement_properties_.complete( ) ||
		     !character_properties_.complete( ) ) {
			return call_original(
			    movement,
			    delta_seconds,
			    friction,
			    fluid,
			    braking_deceleration );
		}

		auto** owner_storage =
		    property_value< UObject* >( movement, movement_properties_.character_owner );
		UObject* owner = owner_storage ? *owner_storage : nullptr;
		auto*    mode =
		    property_value< std::uint8_t >( movement, movement_properties_.movement_mode );
		if ( !owner || !mode || !owner->IsA( character_class_ ) ) {
			return call_original(
			    movement,
			    delta_seconds,
			    friction,
			    fluid,
			    braking_deceleration );
		}

		auto&      state = states_[ movement ];
		const bool replaying =
		    bool_value( owner, character_properties_.client_updating );
		if ( replaying && !state.replaying ) {
			// UE is about to replay FSavedMove_Character records following
			// a server correction. Our transient crouch edge/timer is not
			// part of those records, so restart it at the replay boundary
			// and reconstruct it from each move's compressed crouch flag.
			state.has_crouch_state    = false;
			state.crouch_hold_seconds = 0.0;
		}
		state.replaying = replaying;

		if ( const auto ladder = ladder_states_.find( owner ); ladder != ladder_states_.end( ) ) {
			const bool native_ladder_transition = character_movement_blocked( owner );
			if ( config_.enabled && !native_ladder_transition ) {
				restore_properties( movement, state );
				handle_ladder_velocity( movement, owner, *mode, ladder->second );
				return;
			}
			ladder_states_.erase( ladder );
			restore_properties( movement, state );
			if ( native_ladder_transition ) {
				return call_original( movement, delta_seconds, friction, fluid, braking_deceleration );
			}
			set_movement_mode( movement, movement_falling );
		}

		const bool blocked = character_movement_blocked( owner );
		if ( const auto water = water_states_.find( owner ); water != water_states_.end( ) ) {
			if ( config_.enabled && !blocked ) {
				apply_properties( movement, state );
				if ( water->second.submerged ) {
					handle_water_velocity( movement, owner, delta_seconds );
				} else {
					handle_water_surface_velocity( movement, owner, delta_seconds );
				}
				return;
			}
			water_states_.erase( owner );
			set_movement_mode( movement, movement_falling );
		}

		const bool ordinary_mode =
		    *mode == movement_walking || *mode == movement_falling;
		const bool special =
		    fluid || !ordinary_mode || blocked;

		if ( !config_.enabled || special ) {
			restore_properties( movement, state );
			state.has_crouch_state = false;
			return call_original(
			    movement,
			    delta_seconds,
			    friction,
			    fluid,
			    braking_deceleration );
		}

		auto* velocity =
		    property_value< FVector >( movement, movement_properties_.velocity );
		auto* acceleration =
		    property_value< FVector >( movement, movement_properties_.acceleration );
		auto* max_acceleration =
		    property_value< float >( movement, movement_properties_.max_acceleration );
		if ( !velocity || !acceleration || !max_acceleration ) {
			restore_properties( movement, state );
			return call_original(
			    movement,
			    delta_seconds,
			    friction,
			    fluid,
			    braking_deceleration );
		}

		apply_properties( movement, state );
		if ( !replaying && config_.auto_bhop && jump_held_[ owner ] ) {
			character_properties_.pressed_jump->SetPropertyValueInContainer(
			    owner,
			    true );
		}
		const auto* jump_key_hold_time = property_value< float >(
		    owner,
		    character_properties_.jump_key_hold_time );
		// UE clears bPressedJump before CalcVelocity when
		// JumpMaxHoldTime is zero. JumpKeyHoldTime was captured from the
		// same compressed client move immediately before that clear, so
		// it also carries crouched jump requests on authority pawns.
		const bool jump_queued =
		    bool_value( owner, character_properties_.pressed_jump ) ||
		    ( jump_key_hold_time && *jump_key_hold_time > 0.0F );

		bhop::vec3_t current{
			velocity->X( ),
			velocity->Y( ),
			velocity->Z( ),
		};
		bool       vertical_velocity_overridden = false;
		bool       crouched_jump_started        = false;
		const bool wants_crouch                 = bool_value(
		    movement,
		    movement_properties_.wants_to_crouch );
		const bool released_quick_crouch =
		    state.has_crouch_state &&
		    state.last_wants_crouch &&
		    !wants_crouch &&
		    state.crouch_hold_seconds <= config_.duck_roll_window;

		if ( wants_crouch ) {
			state.crouch_hold_seconds =
			    state.has_crouch_state && state.last_wants_crouch
			    ? state.crouch_hold_seconds + delta_seconds
			    : 0.0;
		}

		if ( config_.duck_roll && released_quick_crouch &&
		     *mode == movement_walking ) {
			auto*        actor             = static_cast< RC::Unreal::AActor* >( owner );
			auto         original_location = actor->K2_GetActorLocation( );
			auto         target_location   = original_location;
			const double lift =
			    bhop::source_to_cm( config_.duck_roll_height );
			target_location.SetZ( target_location.Z( ) + lift );
			RC::Unreal::FHitResult sweep_result{};
			actor->K2_SetActorLocation(
			    target_location,
			    true,
			    sweep_result,
			    false );
			const auto actual_location = actor->K2_GetActorLocation( );
			if ( actual_location.Z( ) - original_location.Z( ) >=
			         lift - 0.1 &&
			     set_movement_mode( movement, movement_falling ) ) {
				// GoldSrc PM_UnDuck changes the origin/hull and leaves
				// vertical velocity untouched. The subsequent falling
				// frame starts at the raised origin under normal gravity.
				current.z                    = 0.0;
				vertical_velocity_overridden = true;
			} else {
				actor->K2_SetActorLocation(
				    original_location,
				    false,
				    sweep_result,
				    true );
			}
		}

		// bIsCrouched is maintained by CharacterMovementComponent and is
		// reproduced by UE's saved-move replay. Never gate movement speed
		// on our duck-roll timer: that timer is intentionally transient
		// and can have a different history after a server correction.
		const bool crouched =
		    bool_value( owner, character_properties_.is_crouched );
		if ( jump_queued && crouched && *mode == movement_walking &&
		     set_movement_mode( movement, movement_falling ) ) {
			current.z =
			    bhop::source_to_cm( config_.move.jump_velocity );
			vertical_velocity_overridden = true;
			crouched_jump_started        = true;
		}

		// bWasJumping is captured by FSavedMove_Character and restored by
		// PrepMoveFor, unlike our old previous-mode bookkeeping. This
		// keeps cap application identical during client correction replay.
		if ( config_.bunnyhop_speed_cap &&
		     *mode == movement_falling &&
		     ( bool_value( owner, character_properties_.was_jumping ) ||
		       crouched_jump_started ) ) {
			current = bhop::apply_mega_bunny_cap( current, config_.move );
		}

		const double acceleration_length = std::hypot(
		    acceleration->X( ),
		    acceleration->Y( ) );
		// PhysFalling scales Acceleration by Unreal's AirControl before it
		// invokes CalcVelocity. Preserve that replicated world-space
		// direction, but reconstruct the full keyboard wish magnitude so
		// ETB's native air-control scalar is not applied on top of GoldSrc
		// air acceleration. Ground movement retains analog magnitude.
		const double input_acceleration =
		    *mode == movement_falling && acceleration_length > 1.0e-6
		    ? acceleration_length
		    : *max_acceleration;
		const double crouch_input_scale =
		    crouched ? gold_src_crouch_speed_multiplier : 1.0;
		const bhop::movement_input_t input{
			.acceleration_cm = {
			    acceleration->X( ) * crouch_input_scale,
			    acceleration->Y( ) * crouch_input_scale,
			    acceleration->Z( ),
			},
			.max_input_acceleration_cm = input_acceleration,
			.delta_seconds             = delta_seconds,
			.grounded                  = *mode == movement_walking,
			.jump_queued               = jump_queued,
		};
		const auto result =
		    bhop::calculate_velocity( current, input, config_.move );
		velocity->SetX( result.x );
		velocity->SetY( result.y );
		if ( vertical_velocity_overridden ) {
			velocity->SetZ( result.z );
		}

		state.has_crouch_state  = true;
		state.last_wants_crouch = wants_crouch;
		if ( wants_crouch && crouch_release_pending_[ owner ] ) {
			crouch_held_[ owner ]            = false;
			crouch_release_pending_[ owner ] = false;
		}
	}

	auto c_etb_bhop_mod::apply_properties( UObject* movement, movement_state_t& state ) -> void {
		auto* gravity =
		    property_value< float >( movement, movement_properties_.gravity_scale );
		auto* jump =
		    property_value< float >( movement, movement_properties_.jump_z_velocity );
		auto* walk =
		    property_value< float >( movement, movement_properties_.max_walk_speed );
		auto* crouch = property_value< float >(
		    movement,
		    movement_properties_.max_walk_speed_crouched );
		auto* sprint = property_value< float >(
		    movement,
		    movement_properties_.max_sprint_speed );
		auto* swim = property_value< float >(
		    movement,
		    movement_properties_.max_swim_speed );
		if ( !gravity || !jump || !walk || !crouch || !sprint || !swim ) {
			return;
		}
		if ( !state.overriding ) {
			state.overriding              = true;
			state.gravity_scale           = *gravity;
			state.jump_z_velocity         = *jump;
			state.max_walk_speed          = *walk;
			state.max_walk_speed_crouched = *crouch;
			state.max_sprint_speed        = *sprint;
			state.max_swim_speed          = *swim;
		}

		*gravity = static_cast< float >(
		    bhop::source_to_cm( config_.move.gravity ) / 980.0 );
		*jump = static_cast< float >(
		    bhop::source_to_cm( config_.move.jump_velocity ) );
		const auto fixed_speed =
		    static_cast< float >( bhop::source_to_cm( config_.move.move_speed ) );
		*walk   = fixed_speed;
		*crouch = static_cast< float >(
		    fixed_speed * gold_src_crouch_speed_multiplier );
		*sprint = fixed_speed;
		*swim   = static_cast< float >( bhop::source_to_cm( config_.move.max_speed ) );
	}

	auto c_etb_bhop_mod::restore_properties( UObject* movement, movement_state_t& state ) -> void {
		if ( !state.overriding ) {
			return;
		}
		if ( auto* value = property_value< float >(
		         movement,
		         movement_properties_.gravity_scale ) ) {
			*value = state.gravity_scale;
		}
		if ( auto* value = property_value< float >(
		         movement,
		         movement_properties_.jump_z_velocity ) ) {
			*value = state.jump_z_velocity;
		}
		if ( auto* value = property_value< float >(
		         movement,
		         movement_properties_.max_walk_speed ) ) {
			*value = state.max_walk_speed;
		}
		if ( auto* value = property_value< float >(
		         movement,
		         movement_properties_.max_walk_speed_crouched ) ) {
			*value = state.max_walk_speed_crouched;
		}
		if ( auto* value = property_value< float >(
		         movement,
		         movement_properties_.max_sprint_speed ) ) {
			*value = state.max_sprint_speed;
		}
		if ( auto* value = property_value< float >(
		         movement,
		         movement_properties_.max_swim_speed ) ) {
			*value = state.max_swim_speed;
		}
		state.overriding = false;
	}
}  // namespace bhop::native
