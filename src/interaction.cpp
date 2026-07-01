#include "mod.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Input/Handler.hpp>
#include <Input/KeyDef.hpp>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

namespace bhop::native {
	namespace {
		[[nodiscard]] auto input_key_from_unreal_name( const std::string& name ) -> std::optional< RC::Input::Key > {
			std::string normalized;
			if ( name == "SpaceBar" ) {
				normalized = "space";
			} else if ( name == "Enter" ) {
				normalized = "return";
			} else {
				for ( std::size_t index{}; index < name.size( ); ++index ) {
					const auto character = static_cast< unsigned char >( name[ index ] );
					if ( std::isupper( character ) && index > 0 && name[ index - 1 ] != '_' &&
					     std::islower( static_cast< unsigned char >( name[ index - 1 ] ) ) ) {
						normalized.push_back( '_' );
					}
					normalized.push_back( static_cast< char >( std::tolower( character ) ) );
				}
			}

			try {
				return RC::Input::string_to_key( RC::to_wstring( normalized ) );
			} catch ( const std::runtime_error& ) {
				return std::nullopt;
			}
		}

		[[nodiscard]] auto configured_interact_key( ) -> std::pair< RC::Input::Key, std::string > {
			wchar_t*    local_app_data{ };
			std::size_t local_app_data_length{ };
			if ( _wdupenv_s( &local_app_data, &local_app_data_length, L"LOCALAPPDATA" ) == 0 &&
			     local_app_data ) {
				auto path = std::filesystem::path{ local_app_data } /
				    "EscapeTheBackrooms" / "Saved" / "Config" /
				    "WindowsNoEditor" / "Input.ini";
				std::free( local_app_data );
				std::ifstream input{ path };
				for ( std::string line; std::getline( input, line ); ) {
					if ( line.find( "ActionName=\"Interact\"" ) == std::string::npos ) {
						continue;
					}
					const auto key_begin = line.find( "Key=" );
					if ( key_begin == std::string::npos ) {
						continue;
					}
					const auto value_begin = key_begin + 4;
					const auto value_end   = line.find_first_of( ",)", value_begin );
					const auto key_name    = line.substr( value_begin, value_end - value_begin );
					if ( const auto key = input_key_from_unreal_name( key_name ) ) {
						return { *key, key_name };
					}
				}
			}
			return { RC::Input::F, "F" };
		}
	}  // namespace

	auto c_etb_bhop_mod::register_interaction_input( ) -> void {
		const auto [ key, name ] = configured_interact_key( );
		const auto callback      = [ this ] {
            // UE4SS pumps physical input outside Unreal's game thread.
            // Defer all UObject access to the next engine tick.
            interaction_pressed_.store( true );
		};
		register_keydown_event( key, callback );
		for ( const auto modifiers : {
		          RC::Input::Handler::ModifierKeyArray{ RC::Input::CONTROL },
		          RC::Input::Handler::ModifierKeyArray{ RC::Input::SHIFT },
		          RC::Input::Handler::ModifierKeyArray{ RC::Input::ALT },
		          RC::Input::Handler::ModifierKeyArray{ RC::Input::CONTROL, RC::Input::SHIFT },
		          RC::Input::Handler::ModifierKeyArray{ RC::Input::CONTROL, RC::Input::ALT },
		          RC::Input::Handler::ModifierKeyArray{ RC::Input::SHIFT, RC::Input::ALT },
		          RC::Input::Handler::ModifierKeyArray{ RC::Input::CONTROL, RC::Input::SHIFT, RC::Input::ALT },
		      } ) {
			register_keydown_event( key, modifiers, callback );
		}
		RC::Output::send< RC::LogLevel::Normal >(
		    STR( "[bhop] Capturing the Interact action on key {} with any modifier state.\n" ),
		    RC::to_wstring( name ) );
	}

	auto c_etb_bhop_mod::register_interaction_hooks( UObject* character ) -> void {
		current_interactable_property_ = character->GetPropertyByNameInChain(
		    STR( "CurrentInteractableActor" ) );
		interact_function_ = character->GetFunctionByNameInChain( STR( "Interact" ) );
		if ( interact_function_ ) {
			for ( FProperty* property : RC::Unreal::TFieldRange< FProperty >(
			          interact_function_,
			          RC::Unreal::EFieldIterationFlags::IncludeDeprecated ) ) {
				if ( property->HasAnyPropertyFlags( RC::Unreal::CPF_Parm ) &&
				     !property->HasAnyPropertyFlags( RC::Unreal::CPF_ReturnParm ) ) {
					interact_actor_property_ = property;
					break;
				}
			}
		}
		if ( !current_interactable_property_ || !interact_actor_property_ ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Airborne interaction unavailable: ETB interaction functions could not be validated.\n" ) );
			return;
		}

		const auto interact_pre = [ this ]( RC::Unreal::UnrealScriptFunctionCallableContext& context, void* ) {
			begin_interaction_override( context );
		};
		const auto interact_post = [ this ]( RC::Unreal::UnrealScriptFunctionCallableContext& context, void* ) {
			end_interaction_override( context );
		};
		interaction_hook_ids_.push_back(
		    RC::Unreal::UObjectGlobals::RegisterHook(
		        interact_function_,
		        interact_pre,
		        interact_post,
		        nullptr ) );

		RC::Output::send< RC::LogLevel::Normal >(
		    STR( "[bhop] Airborne interaction override active.\n" ) );
	}

	auto c_etb_bhop_mod::begin_interaction_override( RC::Unreal::UnrealScriptFunctionCallableContext& context ) -> void {
		const auto existing = interaction_overrides_.find( context.Context );
		if ( existing != interaction_overrides_.end( ) ) {
			++existing->second.depth;
			return;
		}

		auto**     movement_storage = context.Context->GetValuePtrByPropertyNameInChain< UObject* >( STR( "CharacterMovement" ) );
		UObject*   movement         = movement_storage ? *movement_storage : nullptr;
		auto*      movement_mode    = property_value< std::uint8_t >( movement, movement_properties_.movement_mode );
		const bool is_crouched      = bool_value( context.Context, character_properties_.is_crouched );
		const bool wants_crouch     = bool_value( movement, movement_properties_.wants_to_crouch );
		const auto held             = crouch_held_.find( context.Context );
		const bool crouch_held      = held != crouch_held_.end( ) && held->second;
		if ( !config_.enabled || !movement_mode ||
		     ( *movement_mode != movement_falling && !is_crouched && !wants_crouch && !crouch_held ) ) {
			return;
		}

		auto& state = interaction_overrides_[ context.Context ];
		state.depth               = 1;
		state.movement            = movement;
		state.movement_mode       = *movement_mode;
		state.pressed_jump        = bool_value( context.Context, character_properties_.pressed_jump );
		state.crouched            = is_crouched;
		state.wants_to_crouch     = wants_crouch;
		state.should_long_crouch  = bool_value( context.Context, character_properties_.should_long_crouch );
		state.should_scale_crouch = bool_value( context.Context, character_properties_.should_scale_crouch );
		auto* crouch_amount       = property_value< float >( context.Context, character_properties_.crouch_amount );
		state.crouch_amount       = crouch_amount ? *crouch_amount : 0.0F;

		// Interact runs synchronously. No movement update can observe these
		// temporary values before the post-hook restores them.
		if ( *movement_mode == movement_falling ) {
			*movement_mode = movement_walking;
		}
		character_properties_.pressed_jump->SetPropertyValueInContainer( context.Context, false );
		character_properties_.is_crouched->SetPropertyValueInContainer( context.Context, false );
		movement_properties_.wants_to_crouch->SetPropertyValueInContainer( movement, false );
		character_properties_.should_long_crouch->SetPropertyValueInContainer( context.Context, false );
		character_properties_.should_scale_crouch->SetPropertyValueInContainer( context.Context, false );
		if ( crouch_amount ) {
			*crouch_amount = 0.0F;
		}
	}

	auto c_etb_bhop_mod::end_interaction_override( RC::Unreal::UnrealScriptFunctionCallableContext& context ) -> void {
		const auto state = interaction_overrides_.find( context.Context );
		if ( state == interaction_overrides_.end( ) ) {
			return;
		}
		if ( state->second.depth > 1 ) {
			--state->second.depth;
			return;
		}

		auto* movement_mode = property_value< std::uint8_t >( state->second.movement, movement_properties_.movement_mode );
		if ( movement_mode ) {
			*movement_mode = state->second.movement_mode;
		}
		character_properties_.pressed_jump->SetPropertyValueInContainer( context.Context, state->second.pressed_jump );
		character_properties_.is_crouched->SetPropertyValueInContainer( context.Context, state->second.crouched );
		movement_properties_.wants_to_crouch->SetPropertyValueInContainer( state->second.movement, state->second.wants_to_crouch );
		character_properties_.should_long_crouch->SetPropertyValueInContainer( context.Context, state->second.should_long_crouch );
		character_properties_.should_scale_crouch->SetPropertyValueInContainer( context.Context, state->second.should_scale_crouch );
		auto* crouch_amount = property_value< float >( context.Context, character_properties_.crouch_amount );
		if ( crouch_amount ) {
			*crouch_amount = state->second.crouch_amount;
		}
		interaction_overrides_.erase( state );
	}

	auto c_etb_bhop_mod::try_airborne_interaction( UObject* character ) -> bool {
		auto** movement_storage = character->GetValuePtrByPropertyNameInChain< UObject* >(
		    STR( "CharacterMovement" ) );
		UObject*    movement      = movement_storage ? *movement_storage : nullptr;
		const auto* movement_mode = property_value< std::uint8_t >(
		    movement,
		    movement_properties_.movement_mode );
		if ( !movement_mode || *movement_mode != movement_falling ) {
			return false;
		}

		auto** target_storage = property_value< UObject* >(
		    character,
		    current_interactable_property_ );
		UObject* target = target_storage ? *target_storage : nullptr;
		if ( !target ) {
			return false;
		}

		std::array< std::uint8_t, 16 > parameters{ };
		*interact_actor_property_->ContainerPtrToValuePtr< UObject* >(
		    parameters.data( ) ) = target;
		character->ProcessEvent( interact_function_, parameters.data( ) );
		return true;
	}
}  // namespace bhop::native
