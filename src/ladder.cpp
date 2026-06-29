#include "mod.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace bhop::native {
	[[nodiscard]] auto c_etb_bhop_mod::read_vector_return( UObject* object, UFunction*& function, FProperty*& return_property, const RC::Unreal::TCHAR* function_name ) -> std::optional< bhop::vec3_t > {
		if ( !function ) {
			function = object->GetFunctionByNameInChain( function_name );
			if ( function ) {
				return_property = function->GetPropertyByNameInChain( STR( "ReturnValue" ) );
			}
		}
		if ( !function || !return_property ) {
			return std::nullopt;
		}

		alignas( 16 ) std::array< std::uint8_t, 32 > parameters{};
		object->ProcessEvent( function, parameters.data( ) );
		const auto* value = return_property->ContainerPtrToValuePtr< FVector >( parameters.data( ) );
		if ( !value ) {
			return std::nullopt;
		}
		return bhop::vec3_t{ value->X( ), value->Y( ), value->Z( ) };
	}

	[[nodiscard]] auto c_etb_bhop_mod::ladder_contains_player( const ladder_state_t& state, const bhop::vec3_t& player, const bhop::vec3_t& normal ) const -> bool {
		const bhop::vec3_t segment{
			state.end.x - state.start.x,
			state.end.y - state.start.y,
			state.end.z - state.start.z,
		};
		const double length = std::sqrt( segment.x * segment.x + segment.y * segment.y + segment.z * segment.z );
		if ( length <= 1.0 ) {
			return false;
		}

		const bhop::vec3_t axis{ segment.x / length, segment.y / length, segment.z / length };
		bhop::vec3_t       side{
			axis.y * normal.z - axis.z * normal.y,
			axis.z * normal.x - axis.x * normal.z,
			axis.x * normal.y - axis.y * normal.x,
		};
		const double side_length = std::sqrt( side.x * side.x + side.y * side.y + side.z * side.z );
		if ( side_length <= 1.0e-6 ) {
			return false;
		}
		side.x /= side_length;
		side.y /= side_length;
		side.z /= side_length;

		const bhop::vec3_t relative{
			player.x - state.start.x,
			player.y - state.start.y,
			player.z - state.start.z,
		};
		const double along        = relative.x * axis.x + relative.y * axis.y + relative.z * axis.z;
		const double from_plane   = relative.x * normal.x + relative.y * normal.y + relative.z * normal.z;
		const double from_center  = relative.x * side.x + relative.y * side.y + relative.z * side.z;
		const double axial_margin = bhop::source_to_cm( 36.0 );
		const double hull_radius  = bhop::source_to_cm( 16.0 );

		return along >= -axial_margin &&
		    along <= length + axial_margin &&
		    std::abs( from_plane ) <= state.contact_depth_cm + hull_radius &&
		    std::abs( from_center ) <= state.half_width_cm + hull_radius;
	}

	auto c_etb_bhop_mod::detach_from_ladder( UObject* movement, UObject* owner ) -> void {
		ladder_states_.erase( owner );
		set_movement_mode( movement, movement_falling );
	}

	[[nodiscard]] auto c_etb_bhop_mod::is_native_transition_ladder( UObject* ladder ) const -> bool {
		if ( !ladder ) {
			return false;
		}
		const auto path = ladder->GetPathName( );
		return path.find( STR( "BP_Pool_Ladder_ClimbUp" ) ) != RC::StringType::npos ||
		    path.find( STR( "BP_Pool_Ladder_Transition" ) ) != RC::StringType::npos;
	}

	[[nodiscard]] auto c_etb_bhop_mod::ladder_normal_for_player( UObject* ladder, UObject* owner ) const -> bhop::vec3_t {
		auto*        ladder_actor    = static_cast< RC::Unreal::AActor* >( ladder );
		auto*        player_actor    = static_cast< RC::Unreal::AActor* >( owner );
		const auto   ladder_location = ladder_actor->K2_GetActorLocation( );
		const auto   player_location = player_actor->K2_GetActorLocation( );
		const auto   ladder_rotation = ladder_actor->K2_GetActorRotation( );
		const auto   pitch           = ladder_rotation.GetPitch( ) * std::numbers::pi / 180.0;
		const auto   yaw             = ladder_rotation.GetYaw( ) * std::numbers::pi / 180.0;
		bhop::vec3_t normal{
			std::cos( pitch ) * std::cos( yaw ),
			std::cos( pitch ) * std::sin( yaw ),
			std::sin( pitch ),
		};
		const double player_side = normal.x * ( player_location.X( ) - ladder_location.X( ) ) +
		    normal.y * ( player_location.Y( ) - ladder_location.Y( ) ) +
		    normal.z * ( player_location.Z( ) - ladder_location.Z( ) );
		if ( player_side < 0.0 ) {
			normal.x = -normal.x;
			normal.y = -normal.y;
			normal.z = -normal.z;
		}
		return normal;
	}

	auto c_etb_bhop_mod::handle_ladder_velocity( UObject* movement, UObject* owner, std::uint8_t, ladder_state_t& state ) -> void {
		auto* velocity         = property_value< FVector >( movement, movement_properties_.velocity );
		auto* acceleration     = property_value< FVector >( movement, movement_properties_.acceleration );
		auto* max_acceleration = property_value< float >( movement, movement_properties_.max_acceleration );
		if ( !velocity || !acceleration || !max_acceleration ) {
			detach_from_ladder( movement, owner );
			return;
		}

		auto*              ladder_actor    = static_cast< RC::Unreal::AActor* >( state.ladder );
		auto*              player_actor    = static_cast< RC::Unreal::AActor* >( owner );
		const auto         player_location = player_actor->K2_GetActorLocation( );
		const auto         ladder_normal   = ladder_normal_for_player( ladder_actor, player_actor );
		const bhop::vec3_t player_position{ player_location.X( ), player_location.Y( ), player_location.Z( ) };
		if ( !ladder_contains_player( state, player_position, ladder_normal ) ) {
			detach_from_ladder( movement, owner );
			return;
		}

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

		const double forward_acceleration = acceleration->X( ) * std::cos( view_yaw ) + acceleration->Y( ) * std::sin( view_yaw );
		const double side_acceleration    = acceleration->X( ) * -std::sin( view_yaw ) + acceleration->Y( ) * std::cos( view_yaw );
		const double input_threshold      = std::max( 1.0, static_cast< double >( *max_acceleration ) * 0.01 );
		const double forward_move         = std::abs( forward_acceleration ) >= input_threshold ? std::copysign( 1.0, forward_acceleration ) : 0.0;
		const double side_move            = std::abs( side_acceleration ) >= input_threshold ? std::copysign( 1.0, side_acceleration ) : 0.0;
		const auto*  jump_key_hold_time   = property_value< float >( owner, character_properties_.jump_key_hold_time );
		const bool   jump_queued          = jump_held_[ owner ] ||
		    bool_value( owner, character_properties_.pressed_jump ) ||
		    ( jump_key_hold_time && *jump_key_hold_time > 0.0F );

		const auto         result = bhop::calculate_ladder_velocity( {
		                                                                 .view_forward  = view_forward,
		                                                                 .view_right    = view_right,
		                                                                 .ladder_normal = ladder_normal,
		                                                                 .forward_move  = forward_move,
		                                                                 .side_move     = side_move,
		                                                                 .crouched      = bool_value( owner, character_properties_.is_crouched ) || bool_value( movement, movement_properties_.wants_to_crouch ),
		                                                                 .on_floor      = state.on_floor,
		                                                                 .jump_queued   = jump_queued,
		                                                             },
		                                                             config_.move );
		const bhop::vec3_t ladder_segment{
			state.end.x - state.start.x,
			state.end.y - state.start.y,
			state.end.z - state.start.z,
		};
		const double ladder_length = std::sqrt(
		    ladder_segment.x * ladder_segment.x +
		    ladder_segment.y * ladder_segment.y +
		    ladder_segment.z * ladder_segment.z );
		const bhop::vec3_t from_start{
			player_position.x - state.start.x,
			player_position.y - state.start.y,
			player_position.z - state.start.z,
		};
		const double progress        = ladder_length > 1.0
		    ? ( from_start.x * ladder_segment.x +
		        from_start.y * ladder_segment.y +
		        from_start.z * ladder_segment.z ) /
		        ladder_length
		    : 0.0;
		const double ascending_speed = ladder_length > 1.0
		    ? ( result.velocity_cm.x * ladder_segment.x +
		        result.velocity_cm.y * ladder_segment.y +
		        result.velocity_cm.z * ladder_segment.z ) /
		        ladder_length
		    : 0.0;
		if ( !result.detached &&
		     progress >= ladder_length - bhop::source_to_cm( 12.0 ) &&
		     ascending_speed > 1.0 ) {
			velocity->SetX( result.velocity_cm.x );
			velocity->SetY( result.velocity_cm.y );
			velocity->SetZ( result.velocity_cm.z );
			ladder_states_.erase( owner );
			set_movement_mode( movement, movement_falling );
			return;
		}

		velocity->SetX( result.velocity_cm.x );
		velocity->SetY( result.velocity_cm.y );
		velocity->SetZ( result.velocity_cm.z );
		state.on_floor = false;

		if ( result.detached ) {
			detach_from_ladder( movement, owner );
		}
	}

	[[nodiscard]] auto c_etb_bhop_mod::activate_ladder( UObject* ladder, UObject* owner, bool require_contact ) -> bool {
		if ( !hook_ready_ || !config_.enabled || is_native_transition_ladder( ladder ) ||
		     !owner || !owner->IsA( character_class_ ) || character_movement_blocked( owner ) ) {
			return false;
		}

		auto**   movement_storage = owner->GetValuePtrByPropertyNameInChain< UObject* >( STR( "CharacterMovement" ) );
		UObject* movement         = movement_storage ? *movement_storage : nullptr;
		auto*    mode             = property_value< std::uint8_t >( movement, movement_properties_.movement_mode );
		if ( !movement || !mode || ( *mode != movement_walking && *mode != movement_falling ) ) {
			return false;
		}

		const auto start = read_vector_return( ladder, ladder_start_function_, ladder_start_return_property_, STR( "GetStartPoint" ) );
		const auto end   = read_vector_return( ladder, ladder_end_function_, ladder_end_return_property_, STR( "GetHeightPoint" ) );
		if ( !start || !end ) {
			return false;
		}

		double   half_width_cm    = bhop::source_to_cm( 32.0 );
		double   contact_depth_cm = bhop::source_to_cm( 2.0 );
		auto**   box_storage      = ladder->GetValuePtrByPropertyNameInChain< UObject* >( STR( "Box1" ) );
		UObject* box              = box_storage ? *box_storage : nullptr;
		if ( box ) {
			if ( const auto extent = read_vector_return( box, box_extent_function_, box_extent_return_property_, STR( "GetScaledBoxExtent" ) ) ) {
				half_width_cm = std::clamp(
				    std::abs( extent->y ),
				    bhop::source_to_cm( 24.0 ),
				    bhop::source_to_cm( 64.0 ) );
			}
		}

		ladder_state_t ladder_state{
			.ladder           = ladder,
			.start            = *start,
			.end              = *end,
			.half_width_cm    = half_width_cm,
			.contact_depth_cm = contact_depth_cm,
			.on_floor         = *mode == movement_walking,
		};
		if ( require_contact ) {
			auto*              player_actor    = static_cast< RC::Unreal::AActor* >( owner );
			const auto         player_location = player_actor->K2_GetActorLocation( );
			const bhop::vec3_t player_position{ player_location.X( ), player_location.Y( ), player_location.Z( ) };
			const auto         normal = ladder_normal_for_player( ladder, owner );
			if ( !ladder_contains_player( ladder_state, player_position, normal ) ) {
				return false;
			}
		}

		auto& movement_state = states_[ movement ];
		restore_properties( movement, movement_state );
		water_states_.erase( owner );
		ladder_states_[ owner ] = ladder_state;
		if ( !set_movement_mode( movement, movement_flying ) ) {
			ladder_states_.erase( owner );
			return false;
		}
		return true;
	}
}  // namespace bhop::native
