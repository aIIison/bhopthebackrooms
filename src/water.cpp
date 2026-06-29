#include "mod.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace bhop::native {
	[[nodiscard]] auto c_etb_bhop_mod::movement_is_in_water( UObject* movement, UObject* owner ) -> bool {
		if ( bool_value( owner, character_properties_.has_water_physics ) ) {
			return true;
		}
		if ( !get_physics_volume_function_ ) {
			get_physics_volume_function_ = movement->GetFunctionByNameInChain( STR( "GetPhysicsVolume" ) );
			if ( get_physics_volume_function_ ) {
				get_physics_volume_return_property_ = get_physics_volume_function_->GetPropertyByNameInChain( STR( "ReturnValue" ) );
			}
		}
		if ( !get_physics_volume_function_ || !get_physics_volume_return_property_ ) {
			return false;
		}

		alignas( 16 ) std::array< std::uint8_t, 16 > parameters{};
		movement->ProcessEvent( get_physics_volume_function_, parameters.data( ) );
		auto**   volume_storage = get_physics_volume_return_property_->ContainerPtrToValuePtr< UObject* >( parameters.data( ) );
		UObject* volume         = volume_storage ? *volume_storage : nullptr;
		if ( !volume ) {
			return false;
		}
		if ( !water_volume_property_ ) {
			water_volume_property_ = bool_property( volume, STR( "bWaterVolume" ) );
		}
		return bool_value( volume, water_volume_property_ );
	}

	auto c_etb_bhop_mod::handle_water_velocity( UObject* movement, UObject* owner, double delta_seconds ) -> void {
		auto* velocity         = property_value< FVector >( movement, movement_properties_.velocity );
		auto* acceleration     = property_value< FVector >( movement, movement_properties_.acceleration );
		auto* max_acceleration = property_value< float >( movement, movement_properties_.max_acceleration );
		if ( !velocity || !acceleration || !max_acceleration ) {
			return;
		}

		auto*              player_actor       = static_cast< RC::Unreal::AActor* >( owner );
		auto**             controller_storage = property_value< UObject* >( owner, character_properties_.controller );
		UObject*           controller         = controller_storage ? *controller_storage : nullptr;
		const auto*        control_rotation   = controller
		    ? controller->GetValuePtrByPropertyNameInChain< FRotator >( STR( "ControlRotation" ) )
		    : nullptr;
		const auto         view_rotation      = control_rotation ? *control_rotation : player_actor->K2_GetActorRotation( );
		const double       view_pitch         = view_rotation.GetPitch( ) * std::numbers::pi / 180.0;
		const double       view_yaw           = view_rotation.GetYaw( ) * std::numbers::pi / 180.0;
		const bhop::vec3_t view_forward{
			std::cos( view_pitch ) * std::cos( view_yaw ),
			std::cos( view_pitch ) * std::sin( view_yaw ),
			std::sin( view_pitch ),
		};
		const bhop::vec3_t view_right{ -std::sin( view_yaw ), std::cos( view_yaw ), 0.0 };

		const double forward_acceleration =
		    acceleration->X( ) * view_forward.x +
		    acceleration->Y( ) * view_forward.y +
		    acceleration->Z( ) * view_forward.z;
		const double side_acceleration =
		    acceleration->X( ) * view_right.x +
		    acceleration->Y( ) * view_right.y;
		const double input_threshold    = std::max( 1.0, static_cast< double >( *max_acceleration ) * 0.01 );
		const auto   forward_input      = forward_input_.find( owner );
		const auto   side_input         = side_input_.find( owner );
		const double forward_move       = forward_input != forward_input_.end( )
		    ? std::clamp( static_cast< double >( forward_input->second ), -1.0, 1.0 )
		    : ( std::abs( forward_acceleration ) >= input_threshold ? std::copysign( 1.0, forward_acceleration ) : 0.0 );
		const double side_move          = side_input != side_input_.end( )
		    ? std::clamp( static_cast< double >( side_input->second ), -1.0, 1.0 )
		    : ( std::abs( side_acceleration ) >= input_threshold ? std::copysign( 1.0, side_acceleration ) : 0.0 );
		const auto*  jump_key_hold_time = property_value< float >( owner, character_properties_.jump_key_hold_time );
		const bool   swim_up            = jump_held_[ owner ] ||
		    bool_value( owner, character_properties_.pressed_jump ) ||
		    ( jump_key_hold_time && *jump_key_hold_time > 0.0F );
		const bool swim_down = !swim_up && bool_value( movement, movement_properties_.wants_to_crouch );

		const auto result = bhop::calculate_water_velocity(
		    { velocity->X( ), velocity->Y( ), velocity->Z( ) },
		    {
		        .view_forward  = view_forward,
		        .view_right    = view_right,
		        .forward_move  = forward_move,
		        .side_move     = side_move,
		        .up_move       = swim_down ? -1.0 : 0.0,
		        .delta_seconds = delta_seconds,
		        .swim_up       = swim_up,
		    },
		    config_.move );
		velocity->SetX( result.x );
		velocity->SetY( result.y );
		velocity->SetZ( result.z );
	}

	auto c_etb_bhop_mod::handle_water_surface_velocity( UObject* movement, UObject* owner, double delta_seconds ) -> void {
		auto* velocity         = property_value< FVector >( movement, movement_properties_.velocity );
		auto* acceleration     = property_value< FVector >( movement, movement_properties_.acceleration );
		auto* max_acceleration = property_value< float >( movement, movement_properties_.max_acceleration );
		if ( !velocity || !acceleration || !max_acceleration ) {
			return;
		}

		auto*        player_actor       = static_cast< RC::Unreal::AActor* >( owner );
		auto**       controller_storage = property_value< UObject* >( owner, character_properties_.controller );
		UObject*     controller         = controller_storage ? *controller_storage : nullptr;
		const auto*  control_rotation   = controller
		    ? controller->GetValuePtrByPropertyNameInChain< FRotator >( STR( "ControlRotation" ) )
		    : nullptr;
		const auto   view_rotation      = control_rotation ? *control_rotation : player_actor->K2_GetActorRotation( );
		const double yaw                = view_rotation.GetYaw( ) * std::numbers::pi / 180.0;

		bhop::vec3_t input_acceleration{ acceleration->X( ), acceleration->Y( ), 0.0 };
		const auto   forward_input = forward_input_.find( owner );
		const auto   side_input    = side_input_.find( owner );
		if ( forward_input != forward_input_.end( ) && side_input != side_input_.end( ) ) {
			const double forward = std::clamp( static_cast< double >( forward_input->second ), -1.0, 1.0 );
			const double side    = std::clamp( static_cast< double >( side_input->second ), -1.0, 1.0 );
			input_acceleration.x = ( std::cos( yaw ) * forward - std::sin( yaw ) * side ) * *max_acceleration;
			input_acceleration.y = ( std::sin( yaw ) * forward + std::cos( yaw ) * side ) * *max_acceleration;
		}

		auto result = bhop::calculate_velocity(
		    { velocity->X( ), velocity->Y( ), velocity->Z( ) },
		    {
		        .acceleration_cm           = input_acceleration,
		        .max_input_acceleration_cm = *max_acceleration,
		        .delta_seconds             = delta_seconds,
		        .grounded                  = false,
		        .jump_queued               = false,
		    },
		    config_.move );
		result.z -= bhop::source_to_cm( config_.move.gravity ) * delta_seconds;
		velocity->SetX( result.x );
		velocity->SetY( result.y );
		velocity->SetZ( result.z );
	}

	auto c_etb_bhop_mod::update_water_state( UObject* owner ) -> void {
		if ( !hook_ready_ || !owner ) {
			return;
		}
		auto**   movement_storage = owner->GetValuePtrByPropertyNameInChain< UObject* >( STR( "CharacterMovement" ) );
		UObject* movement         = movement_storage ? *movement_storage : nullptr;
		auto*    mode             = property_value< std::uint8_t >( movement, movement_properties_.movement_mode );
		if ( !movement || !mode ) {
			return;
		}

		const bool blocked  = character_movement_blocked( owner );
		const bool in_water = movement_is_in_water( movement, owner );
		if ( const auto water = water_states_.find( owner ); water != water_states_.end( ) ) {
			auto& water_state = water->second;
			if ( !config_.enabled || blocked ) {
				water_states_.erase( owner );
				restore_properties( movement, states_[ movement ] );
				if ( *mode == movement_flying ) {
					set_movement_mode( movement, movement_falling );
				}
				return;
			}

			if ( in_water ) {
				water_state.submerged             = true;
				water_state.outside_seconds       = 0.0;
				water_state.outside_limit_seconds = 0.0;
			} else {
				if ( water_state.submerged ) {
					const auto*  velocity     = property_value< FVector >( movement, movement_properties_.velocity );
					const double upward_speed = velocity ? std::max( 0.0, bhop::cm_to_source( velocity->Z( ) ) ) : 0.0;
					water_state.outside_limit_seconds =
					    std::max( 0.1, ( 2.0 * upward_speed / config_.move.gravity ) + 0.05 );
				}
				water_state.submerged = false;
				water_state.outside_seconds += frame_delta_seconds_;
				if ( water_state.outside_seconds > water_state.outside_limit_seconds ) {
					water_states_.erase( owner );
					restore_properties( movement, states_[ movement ] );
					if ( *mode == movement_flying ) {
						set_movement_mode( movement, movement_falling );
					}
					return;
				}
			}

			if ( *mode != movement_flying ) {
				set_movement_mode( movement, movement_flying );
			}
			return;
		}

		const bool entered_water =
		    *mode == movement_swimming ||
		    bool_value( owner, character_properties_.has_water_physics );
		if ( !config_.enabled || blocked || !in_water || !entered_water || ladder_states_.contains( owner ) ) {
			return;
		}

		restore_properties( movement, states_[ movement ] );
		water_states_[ owner ] = {};
		if ( set_movement_mode( movement, movement_flying ) && !water_logged_ ) {
			RC::Output::send< RC::LogLevel::Normal >( STR( "[bhop] GoldSrc water state active.\n" ) );
			water_logged_ = true;
		}
	}
}  // namespace bhop::native
