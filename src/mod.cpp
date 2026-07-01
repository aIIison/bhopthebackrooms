#define NOMINMAX
#include "mod.h"

#include "hooks.h"
#include "util/process.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <MinHook.h>
#include <UE4SSProgram.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace bhop::native {
	calc_velocity_fn_t          g_original_calc_velocity{};
	can_crouch_fn_t             g_original_can_crouch{};
	physics_volume_changed_fn_t g_original_physics_volume_changed{};

	namespace {
		c_etb_bhop_mod* g_mod{};
	}

	auto movement_properties_t::complete( ) const noexcept -> bool {
		return character_owner && velocity && acceleration && max_acceleration &&
		    movement_mode && gravity_scale && jump_z_velocity &&
		    max_walk_speed && max_walk_speed_crouched && max_sprint_speed &&
		    max_swim_speed && wants_to_crouch;
	}

	auto character_properties_t::complete( ) const noexcept -> bool {
		return pressed_jump && jump_key_hold_time && controller && client_updating &&
		    was_jumping && can_move && is_dead && is_climbing &&
		    is_climbing_ladder && is_balancing && is_falling_balance &&
		    is_pushing && is_crouched && should_long_crouch &&
		    should_scale_crouch && crouch_amount && has_water_physics &&
		    wants_to_crouch_after_landing;
	}

	c_etb_bhop_mod::c_etb_bhop_mod( ) {
		ModName               = STR( "bhop" );
		ModVersion            = STR( "1.0.0" );
		ModDescription        = STR( "GoldSrc movement for Escape the Backrooms" );
		ModAuthors            = STR( "hero" );
		ModIntendedSDKVersion = STR( "1.3.0" );
		g_mod                 = this;
	}

	c_etb_bhop_mod::~c_etb_bhop_mod( ) {
		restore_mouse_settings( );
		const auto unregister_hook = []( const RC::Unreal::TCHAR* path, const auto& hooks, std::size_t index ) {
			if ( index < hooks.size( ) ) {
				RC::Unreal::UObjectGlobals::UnregisterHook( path, hooks[ index ] );
			}
		};

		if ( install_tick_id_ != RC::Unreal::Hook::ERROR_ID ) {
			RC::Unreal::Hook::UnregisterCallback( install_tick_id_ );
		}
		if ( console_hook_id_ != RC::Unreal::Hook::ERROR_ID ) {
			RC::Unreal::Hook::UnregisterCallback( console_hook_id_ );
		}

		unregister_hook( STR( "/Script/Engine.Character:Jump" ), jump_hook_ids_, 0 );
		unregister_hook( STR( "/Script/Engine.Character:StopJumping" ), jump_hook_ids_, 1 );
		unregister_hook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpActEvt_Crouch_K2Node_InputActionEvent_1" ), crouch_hook_ids_, 0 );
		unregister_hook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpActEvt_Crouch_K2Node_InputActionEvent_0" ), crouch_hook_ids_, 1 );
		unregister_hook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_Turn_K2Node_InputAxisEvent_157" ), mouse_hook_ids_, 0 );
		unregister_hook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_LookUp_K2Node_InputAxisEvent_172" ), mouse_hook_ids_, 1 );
		unregister_hook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_MoveForward_K2Node_InputAxisEvent_181" ), movement_input_hook_ids_, 0 );
		unregister_hook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_MoveRight_K2Node_InputAxisEvent_192" ), movement_input_hook_ids_, 1 );
		if ( interact_function_ && !interaction_hook_ids_.empty( ) ) {
			RC::Unreal::UObjectGlobals::UnregisterHook(
			    interact_function_,
			    interaction_hook_ids_.front( ) );
		}
		if ( !ladder_hook_ids_.empty( ) ) {
			RC::Unreal::UObjectGlobals::UnregisterHook( ladder_overlap_function_, ladder_hook_ids_.front( ) );
		}

		for ( void* target : { hook_target_, can_crouch_target_, physics_volume_changed_target_ } ) {
			if ( target ) {
				MH_DisableHook( target );
				MH_RemoveHook( target );
			}
		}
		if ( minhook_initialized_ ) {
			MH_Uninitialize( );
		}
		g_mod = nullptr;
	}

	auto c_etb_bhop_mod::on_unreal_init( ) -> void {
		config_path_ = std::filesystem::path{ RC::StringType{ RC::UE4SSProgram::get_program( ).get_working_directory( ) } } /
		    STR( "Mods" ) / STR( "bhop" ) / STR( "bhop.ini" );
		if ( !reload_config( ) ) {
			return;
		}
		if ( !util::validate_game_image( ) ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Native hook disabled: Backrooms executable does not match Steam build 23657885.\n" ) );
			return;
		}

		apply_mouse_settings( );
		register_commands( );
		register_jump_hooks( );
		register_interaction_input( );

		RC::Unreal::Hook::FCallbackOptions options{};
		options.OwnerModName = STR( "bhop" );
		options.HookName     = STR( "InstallCalcVelocity" );
		install_tick_id_     = RC::Unreal::Hook::RegisterEngineTickPreCallback(
		    [ this ]( auto&, RC::Unreal::UEngine*, float delta_seconds, bool ) {
			    frame_delta_seconds_ = delta_seconds;
			    on_engine_tick( );
		    },
		    std::move( options ) );

		RC::Output::send< RC::LogLevel::Normal >(
		    STR( "[bhop] Native adapter loaded; checksum={}. Waiting for a live ETB character to validate CalcVelocity.\n" ),
		    RC::to_wstring( checksum_hex( config_checksum( config_ ) ) ) );
	}

	auto c_etb_bhop_mod::call_original( UObject* movement, float delta_seconds, float friction, bool fluid, float braking_deceleration ) const -> void {
		if ( g_original_calc_velocity ) {
			g_original_calc_velocity( movement, delta_seconds, friction, fluid, braking_deceleration );
		}
	}

	auto c_etb_bhop_mod::reload_config( ) -> bool {
		const config_result_t loaded = load_config( config_path_ );
		if ( !loaded.loaded ) {
			RC::Output::send< RC::LogLevel::Error >( STR( "[bhop] Configuration rejected: {}\n" ), RC::to_wstring( loaded.error ) );
			return false;
		}
		config_ = loaded.value;
		return true;
	}

	auto c_etb_bhop_mod::apply_mouse_settings( ) -> void {
		if ( !input_settings_ ) {
			input_settings_ = RC::Unreal::UObjectGlobals::StaticFindObject< UObject* >(
			    nullptr, nullptr, STR( "/Script/Engine.Default__InputSettings" ) );
		}
		if ( !mouse_smoothing_property_ && input_settings_ ) {
			mouse_smoothing_property_ = bool_property( input_settings_, STR( "bEnableMouseSmoothing" ) );
		}
		if ( !input_settings_ || !mouse_smoothing_property_ ) {
			RC::Output::send< RC::LogLevel::Warning >(
			    STR( "[bhop] Raw mouse input unavailable: InputSettings could not be validated.\n" ) );
			return;
		}

		if ( !has_original_mouse_smoothing_ ) {
			original_mouse_smoothing_     = mouse_smoothing_property_->GetPropertyValueInContainer( input_settings_ );
			has_original_mouse_smoothing_ = true;
		}
		mouse_smoothing_property_->SetPropertyValueInContainer(
		    input_settings_, config_.raw_mouse_input ? false : original_mouse_smoothing_ );

		if ( !clear_mouse_smoothing_function_ ) {
			clear_mouse_smoothing_function_ = RC::Unreal::UObjectGlobals::StaticFindObject< UFunction* >(
			    nullptr, nullptr, STR( "/Script/Engine.PlayerInput:ClearSmoothing" ) );
		}
		if ( clear_mouse_smoothing_function_ ) {
			std::vector< UObject* > player_inputs;
			RC::Unreal::UObjectGlobals::FindAllOf( STR( "PlayerInput" ), player_inputs );
			for ( UObject* player_input : player_inputs ) {
				player_input->ProcessEvent( clear_mouse_smoothing_function_, nullptr );
			}
		}
	}

	auto c_etb_bhop_mod::restore_mouse_settings( ) -> void {
		if ( input_settings_ && mouse_smoothing_property_ && has_original_mouse_smoothing_ ) {
			mouse_smoothing_property_->SetPropertyValueInContainer( input_settings_, original_mouse_smoothing_ );
		}
	}

	auto c_etb_bhop_mod::cache_properties( UObject* movement, UObject* owner ) -> bool {
		movement_properties_ = {
			.character_owner         = movement->GetPropertyByNameInChain( STR( "CharacterOwner" ) ),
			.velocity                = movement->GetPropertyByNameInChain( STR( "Velocity" ) ),
			.acceleration            = movement->GetPropertyByNameInChain( STR( "Acceleration" ) ),
			.max_acceleration        = movement->GetPropertyByNameInChain( STR( "MaxAcceleration" ) ),
			.movement_mode           = movement->GetPropertyByNameInChain( STR( "MovementMode" ) ),
			.gravity_scale           = movement->GetPropertyByNameInChain( STR( "GravityScale" ) ),
			.jump_z_velocity         = movement->GetPropertyByNameInChain( STR( "JumpZVelocity" ) ),
			.max_walk_speed          = movement->GetPropertyByNameInChain( STR( "MaxWalkSpeed" ) ),
			.max_walk_speed_crouched = movement->GetPropertyByNameInChain( STR( "MaxWalkSpeedCrouched" ) ),
			.max_sprint_speed        = movement->GetPropertyByNameInChain( STR( "MaxSprintSpeed" ) ),
			.max_swim_speed          = movement->GetPropertyByNameInChain( STR( "MaxSwimSpeed" ) ),
			.wants_to_crouch         = bool_property( movement, STR( "bWantsToCrouch" ) ),
		};
		character_properties_ = {
			.pressed_jump                  = bool_property( owner, STR( "bPressedJump" ) ),
			.jump_key_hold_time            = owner->GetPropertyByNameInChain( STR( "JumpKeyHoldTime" ) ),
			.controller                    = owner->GetPropertyByNameInChain( STR( "Controller" ) ),
			.client_updating               = bool_property( owner, STR( "bClientUpdating" ) ),
			.was_jumping                   = bool_property( owner, STR( "bWasJumping" ) ),
			.can_move                      = bool_property( owner, STR( "CanMove" ) ),
			.is_dead                       = bool_property( owner, STR( "IsDead" ) ),
			.is_climbing                   = bool_property( owner, STR( "IsClimbing" ) ),
			.is_climbing_ladder            = bool_property( owner, STR( "IsClimbingLadder" ) ),
			.is_balancing                  = bool_property( owner, STR( "IsBalancing" ) ),
			.is_falling_balance            = bool_property( owner, STR( "IsFallingBalance" ) ),
			.is_pushing                    = bool_property( owner, STR( "IsPushing" ) ),
			.is_crouched                   = bool_property( owner, STR( "bIsCrouched" ) ),
			.should_long_crouch            = bool_property( owner, STR( "ShouldLongCrouch" ) ),
			.should_scale_crouch           = bool_property( owner, STR( "ShouldScaleCrouch" ) ),
			.crouch_amount                 = owner->GetPropertyByNameInChain( STR( "CrouchAmount" ) ),
			.has_water_physics             = bool_property( owner, STR( "HasWaterPhysics" ) ),
			.wants_to_crouch_after_landing = bool_property( owner, STR( "WantsToCrouchAfterLanding" ) ),
		};
		return movement_properties_.complete( ) && character_properties_.complete( );
	}

	auto c_etb_bhop_mod::set_movement_mode( UObject* movement, std::uint8_t mode ) const -> bool {
		if ( !movement || !set_movement_mode_function_ || !set_movement_mode_property_ ) {
			return false;
		}
		std::array< std::uint8_t, 16 > parameters{};
		*set_movement_mode_property_->ContainerPtrToValuePtr< std::uint8_t >( parameters.data( ) ) = mode;
		movement->ProcessEvent( set_movement_mode_function_, parameters.data( ) );
		return true;
	}

	void calc_velocity_detour( UObject* movement, float delta_seconds, float friction, bool fluid, float braking_deceleration ) {
		if ( g_mod ) {
			g_mod->handle_calc_velocity( movement, delta_seconds, friction, fluid, braking_deceleration );
		} else if ( g_original_calc_velocity ) {
			g_original_calc_velocity( movement, delta_seconds, friction, fluid, braking_deceleration );
		}
	}

	bool can_crouch_detour( UObject* movement ) {
		return g_mod && g_mod->should_allow_goldsrc_crouch( movement )
		    ? true
		    : g_original_can_crouch && g_original_can_crouch( movement );
	}

	void physics_volume_changed_detour( UObject* movement, UObject* new_volume ) {
		if ( g_mod && g_mod->begin_water_movement( movement, new_volume ) ) {
			return;
		}
		if ( g_original_physics_volume_changed ) {
			g_original_physics_volume_changed( movement, new_volume );
		}
	}
}  // namespace bhop::native

extern "C" {
__declspec( dllexport ) RC::CppUserModBase* start_mod( ) {
	return new bhop::native::c_etb_bhop_mod( );
}

__declspec( dllexport ) void uninstall_mod( RC::CppUserModBase* mod ) {
	delete mod;
}
}
