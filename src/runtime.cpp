#include "hooks.h"
#include "mod.h"
#include "util/process.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <MinHook.h>
#include <Unreal/UObjectGlobals.hpp>

namespace bhop::native {
	auto c_etb_bhop_mod::on_engine_tick( ) -> void {
		std::vector< UObject* > characters;
		RC::Unreal::UObjectGlobals::FindAllOf(
		    STR( "BPCharacter_Demo_C" ),
		    characters );
		for ( UObject* character : characters ) {
			update_water_state( character );
			if ( config_.enabled && config_.auto_bhop &&
			     jump_held_[ character ] && character_properties_.pressed_jump ) {
				character_properties_.pressed_jump
				    ->SetPropertyValueInContainer( character, true );
			}

			const auto crouch = crouch_held_.find( character );
			if ( hook_ready_ && config_.enabled &&
			     crouch != crouch_held_.end( ) ) {
				auto** movement_storage =
				    character->GetValuePtrByPropertyNameInChain< UObject* >(
				        STR( "CharacterMovement" ) );
				UObject* movement =
				    movement_storage ? *movement_storage : nullptr;
				auto* mode = property_value< std::uint8_t >(
				    movement,
				    movement_properties_.movement_mode );
				if ( movement && mode &&
				     ( *mode == movement_walking ||
				       *mode == movement_falling ||
				       *mode == movement_swimming ||
				       ( *mode == movement_flying &&
				         ( ladder_states_.contains( character ) || water_states_.contains( character ) ) ) ) &&
				     bool_value(
				         character,
				         character_properties_.can_move,
				         false ) ) {
					movement_properties_.wants_to_crouch
					    ->SetPropertyValueInContainer(
					        movement,
					        crouch->second );
					character_properties_.wants_to_crouch_after_landing
					    ->SetPropertyValueInContainer( character, false );
				}
			}
		}

		if ( hook_ready_ ) {
			std::vector< UObject* > ladders;
			RC::Unreal::UObjectGlobals::FindAllOf( STR( "BP_Pool_Ladder_C" ), ladders );
			if ( !ladders.empty( ) ) {
				if ( ladder_hook_ids_.empty( ) ) {
					register_ladder_hook( ladders.front( ) );
				}
				for ( UObject* character : characters ) {
					if ( ladder_states_.contains( character ) ) {
						continue;
					}
					for ( UObject* ladder : ladders ) {
						if ( activate_ladder( ladder, character, true ) ) {
							break;
						}
					}
				}
			}
		}

		if ( hook_ready_ || install_attempted_ || characters.empty( ) ) {
			return;
		}
		install_attempted_ = true;
		install_hook( characters.front( ) );
	}

	auto c_etb_bhop_mod::install_hook( UObject* character ) -> void {
		character_class_ =
		    RC::Unreal::UObjectGlobals::StaticFindObject< UClass* >(
		        nullptr,
		        nullptr,
		        STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C" ) );
		auto** movement_storage =
		    character->GetValuePtrByPropertyNameInChain< UObject* >(
		        STR( "CharacterMovement" ) );
		UObject* movement =
		    movement_storage ? *movement_storage : nullptr;
		if ( !character_class_ || !movement ||
		     !cache_properties( movement, character ) ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Native hook disabled: ETB movement properties " )
			        STR( "did not match the supported build.\n" ) );
			return;
		}

		set_movement_mode_function_ =
		    RC::Unreal::UObjectGlobals::StaticFindObject< UFunction* >(
		        nullptr,
		        nullptr,
		        STR( "/Script/Engine.CharacterMovementComponent:" )
		            STR( "SetMovementMode" ) );
		if ( set_movement_mode_function_ ) {
			set_movement_mode_property_ =
			    set_movement_mode_function_->FindProperty(
			        RC::Unreal::FName(
			            STR( "NewMovementMode" ),
			            RC::Unreal::FNAME_Find ) );
		}
		if ( !set_movement_mode_property_ ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Native hook disabled: SetMovementMode could " )
			        STR( "not be validated.\n" ) );
			return;
		}
		register_crouch_hooks( );
		register_mouse_hooks( );
		register_movement_input_hooks( );

		UFunction* wrapper =
		    RC::Unreal::UObjectGlobals::StaticFindObject< UFunction* >(
		        nullptr,
		        nullptr,
		        STR( "/Script/Engine.CharacterMovementComponent:CalcVelocity" ) );
		const auto slot = resolve_virtual_slot( wrapper );
		if ( !slot ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Native hook disabled: CalcVelocity virtual slot " )
			        STR( "could not be validated.\n" ) );
			return;
		}

		auto*** object_as_vtable = reinterpret_cast< void*** >( movement );
		if ( !object_as_vtable || !*object_as_vtable ) {
			return;
		}
		void* target = ( *object_as_vtable )[ *slot ];
		if ( !util::is_executable_game_address( target ) ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Native hook disabled: CalcVelocity slot {} " )
			        STR( "did not point into executable game code.\n" ),
			    *slot );
			return;
		}
		FProperty* updated_component_property = movement->GetPropertyByNameInChain( STR( "UpdatedComponent" ) );
		const auto can_crouch_slot            = updated_component_property
		    ? resolve_can_crouch_slot(
		          *object_as_vtable,
		          movement_properties_.movement_mode->GetOffset_Internal( ),
		          updated_component_property->GetOffset_Internal( ) )
		    : std::nullopt;
		if ( !can_crouch_slot ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Native hook disabled: CanCrouchInCurrentState could not be validated.\n" ) );
			return;
		}
		void* can_crouch_target = ( *object_as_vtable )[ *can_crouch_slot ];
		if ( !util::is_executable_game_address( can_crouch_target ) || can_crouch_target == target ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Native hook disabled: CanCrouchInCurrentState slot {} did not validate.\n" ),
			    *can_crouch_slot );
			return;
		}

		UFunction* physics_volume_wrapper =
		    RC::Unreal::UObjectGlobals::StaticFindObject< UFunction* >(
		        nullptr,
		        nullptr,
		        STR( "/Script/Engine.CharacterMovementComponent:PhysicsVolumeChanged" ) );
		auto physics_volume_slot = resolve_virtual_slot( physics_volume_wrapper, *object_as_vtable, false );
		if ( !physics_volume_slot ) {
			UClass* physics_volume_class =
			    RC::Unreal::UObjectGlobals::StaticFindObject< UClass* >(
			        nullptr,
			        nullptr,
			        STR( "/Script/Engine.PhysicsVolume" ) );
			FBoolProperty* water_volume_property =
			    bool_property( physics_volume_class, STR( "bWaterVolume" ) );
			if ( water_volume_property ) {
				physics_volume_slot = resolve_physics_volume_changed_slot(
				    *object_as_vtable,
				    water_volume_property->GetOffset_Internal( ) );
			} else {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] PhysicsVolumeChanged semantic scan unavailable: bWaterVolume was not found.\n" ) );
			}
		}
		void* physics_volume_target = physics_volume_slot
		    ? ( *object_as_vtable )[ *physics_volume_slot ]
		    : nullptr;
		if ( physics_volume_target && !util::is_executable_game_address( physics_volume_target ) ) {
			physics_volume_target = nullptr;
		}

		if ( MH_Initialize( ) != MH_OK ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Native hook disabled: MinHook initialization " )
			        STR( "failed.\n" ) );
			return;
		}
		minhook_initialized_ = true;
		bool hook_failed =
		    MH_CreateHook( target, reinterpret_cast< void* >( &calc_velocity_detour ), reinterpret_cast< void** >( &g_original_calc_velocity ) ) != MH_OK ||
		    MH_CreateHook( can_crouch_target, reinterpret_cast< void* >( &can_crouch_detour ), reinterpret_cast< void** >( &g_original_can_crouch ) ) != MH_OK;
		if ( physics_volume_target ) {
			hook_failed |= MH_CreateHook(
			                   physics_volume_target,
			                   reinterpret_cast< void* >( &physics_volume_changed_detour ),
			                   reinterpret_cast< void** >( &g_original_physics_volume_changed ) ) != MH_OK;
		}
		hook_failed |= MH_EnableHook( target ) != MH_OK ||
		    MH_EnableHook( can_crouch_target ) != MH_OK;
		if ( physics_volume_target ) {
			hook_failed |= MH_EnableHook( physics_volume_target ) != MH_OK;
		}
		if ( hook_failed ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Native hook disabled: movement detours could " )
			        STR( "not be installed.\n" ) );
			return;
		}

		hook_target_                   = target;
		can_crouch_target_             = can_crouch_target;
		physics_volume_changed_target_ = physics_volume_target;
		hook_ready_                    = true;
		RC::Output::send< RC::LogLevel::Normal >(
		    STR( "[bhop] Native movement hooks active (CalcVelocity slot {}, CanCrouch slot {}, PhysicsVolume slot {}).\n" ),
		    *slot,
		    *can_crouch_slot,
		    physics_volume_slot ? std::to_wstring( *physics_volume_slot ) : STR( "unavailable" ) );
	}
}  // namespace bhop::native
