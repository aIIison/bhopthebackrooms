#include "mod.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/FOutputDevice.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <algorithm>
#include <format>

namespace bhop::native {
	namespace {
		[[nodiscard]] auto first_token_lower( const RC::Unreal::TCHAR* command ) -> RC::StringType {
			RC::StringType result{ command ? command : STR( "" ) };
			const auto     space = result.find( STR( ' ' ) );
			if ( space != RC::StringType::npos ) {
				result.resize( space );
			}
			std::transform( result.begin( ), result.end( ), result.begin( ), []( auto character ) {
				if ( character >= STR( 'A' ) && character <= STR( 'Z' ) ) {
					return static_cast< decltype( character ) >( character + ( STR( 'a' ) - STR( 'A' ) ) );
				}
				return character;
			} );
			return result;
		}
	}  // namespace
	auto c_etb_bhop_mod::register_jump_hooks( ) -> void {
		const auto press = [ this ](
		                       RC::Unreal::UnrealScriptFunctionCallableContext&
		                           context,
		                       void* ) {
			if ( character_class_ && context.Context->IsA( character_class_ ) ) {
				jump_held_[ context.Context ] = true;
			}
		};
		const auto release = [ this ](
		                         RC::Unreal::UnrealScriptFunctionCallableContext&
		                             context,
		                         void* ) {
			if ( character_class_ && context.Context->IsA( character_class_ ) ) {
				jump_held_[ context.Context ] = false;
			}
		};
		jump_hook_ids_.push_back(
		    RC::Unreal::UObjectGlobals::RegisterHook(
		        STR( "/Script/Engine.Character:Jump" ),
		        press,
		        {},
		        nullptr ) );
		jump_hook_ids_.push_back(
		    RC::Unreal::UObjectGlobals::RegisterHook(
		        STR( "/Script/Engine.Character:StopJumping" ),
		        release,
		        {},
		        nullptr ) );
	}

	auto c_etb_bhop_mod::register_crouch_hooks( ) -> void {
		register_crouch_event( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpActEvt_Crouch_K2Node_InputActionEvent_1" ), 1 );
		register_crouch_event( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpActEvt_Crouch_K2Node_InputActionEvent_0" ), 0 );
	}

	auto c_etb_bhop_mod::register_crouch_event( const RC::StringType& path, int event_id ) -> void {
		const auto callback = [ this, event_id ]( RC::Unreal::UnrealScriptFunctionCallableContext& context, void* ) {
			if ( !character_class_ || !context.Context->IsA( character_class_ ) ) {
				return;
			}
			if ( !crouch_press_event_ ) {
				crouch_press_event_ = event_id;
			}
			if ( event_id == *crouch_press_event_ ) {
				crouch_held_[ context.Context ]            = true;
				crouch_release_pending_[ context.Context ] = false;
			} else if ( ladder_states_.contains( context.Context ) || water_states_.contains( context.Context ) || character_is_swimming( context.Context ) ) {
				crouch_held_[ context.Context ]            = false;
				crouch_release_pending_[ context.Context ] = false;
			} else {
				// Preserve +duck for one predicted move so short taps are
				// represented in compressed client moves.
				crouch_release_pending_[ context.Context ] = true;
			}
		};
		const auto no_op = []( RC::Unreal::UnrealScriptFunctionCallableContext&, void* ) {};
		crouch_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook( path, callback, no_op, nullptr ) );
	}

	auto c_etb_bhop_mod::register_mouse_hooks( ) -> void {
		const auto callback = [ this ]( RC::Unreal::UnrealScriptFunctionCallableContext& context, void* ) { correct_mouse_input( context ); };
		const auto no_op    = []( RC::Unreal::UnrealScriptFunctionCallableContext&, void* ) {};
		mouse_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_Turn_K2Node_InputAxisEvent_157" ), callback, no_op, nullptr ) );
		mouse_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_LookUp_K2Node_InputAxisEvent_172" ), callback, no_op, nullptr ) );
	}

	auto c_etb_bhop_mod::register_movement_input_hooks( ) -> void {
		const auto forward = [ this ]( RC::Unreal::UnrealScriptFunctionCallableContext& context, void* ) {
			forward_input_[ context.Context ] = context.GetParams< axis_input_params_t >( ).axis_value;
		};
		const auto side = [ this ]( RC::Unreal::UnrealScriptFunctionCallableContext& context, void* ) {
			side_input_[ context.Context ] = context.GetParams< axis_input_params_t >( ).axis_value;
		};
		const auto no_op = []( RC::Unreal::UnrealScriptFunctionCallableContext&, void* ) {};
		movement_input_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook(
		    STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_MoveForward_K2Node_InputAxisEvent_181" ),
		    forward,
		    no_op,
		    nullptr ) );
		movement_input_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook(
		    STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_MoveRight_K2Node_InputAxisEvent_192" ),
		    side,
		    no_op,
		    nullptr ) );
	}

	auto c_etb_bhop_mod::register_ladder_hook( UObject* ladder ) -> void {
		ladder_overlap_function_ = ladder->GetFunctionByNameInChain( STR( "BndEvt__BP_Ladder_Box1_K2Node_ComponentBoundEvent_2_ComponentBeginOverlapSignature__DelegateSignature" ) );
		if ( !ladder_overlap_function_ ) {
			return;
		}
		const auto callback = [ this ]( RC::Unreal::UnrealScriptFunctionCallableContext& context, void* ) {
			if ( is_native_transition_ladder( context.Context ) ) {
				return;
			}
			auto&      parameters         = context.GetParams< ladder_overlap_params_t >( );
			const bool already_attached   = parameters.other_actor && ladder_states_.contains( parameters.other_actor );
			const bool ordinary_character = config_.enabled &&
			    parameters.other_actor &&
			    character_class_ &&
			    parameters.other_actor->IsA( character_class_ ) &&
			    !character_movement_blocked( parameters.other_actor );
			if ( already_attached || ordinary_character ) {
				if ( !already_attached ) {
					static_cast< void >( activate_ladder( context.Context, parameters.other_actor, true ) );
				}
				// Make the Blueprint cast fail so ETB never starts its
				// snap-to-ladder timeline. The continuous contact scan
				// attaches once the capsule actually reaches the surface.
				parameters.other_actor = nullptr;
			}
		};
		const auto no_op = []( RC::Unreal::UnrealScriptFunctionCallableContext&, void* ) {};
		ladder_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook( ladder_overlap_function_, callback, no_op, nullptr ) );
		RC::Output::send< RC::LogLevel::Default >( STR( "[bhop] GoldSrc ladder hook active.\n" ) );
	}

	auto c_etb_bhop_mod::correct_mouse_input( RC::Unreal::UnrealScriptFunctionCallableContext& context ) -> void {
		if ( !config_.raw_mouse_input ) {
			return;
		}
		if ( !mouse_delta_seconds_property_ ) {
			mouse_delta_seconds_property_ = context.Context->GetPropertyByNameInChain( STR( "Delta Seconds" ) );
		}

		const auto*  blueprint_delta = property_value< float >( context.Context, mouse_delta_seconds_property_ );
		const double delta_seconds   = blueprint_delta && *blueprint_delta > 1.0e-6F ? *blueprint_delta : frame_delta_seconds_;
		if ( delta_seconds <= 1.0e-6 ) {
			return;
		}

		auto&        parameters = context.GetParams< axis_input_params_t >( );
		const double scale      = std::clamp( 1.0 / ( mouse_reference_fps * delta_seconds ), 0.05, 20.0 );
		parameters.axis_value *= static_cast< float >( scale );
	}

	auto c_etb_bhop_mod::register_commands( ) -> void {
		RC::Unreal::Hook::FCallbackOptions options{};
		options.OwnerModName = STR( "bhop" );
		options.HookName     = STR( "ConsoleCommands" );

		const auto callback = [ this ]( auto& information, UObject*, const RC::Unreal::TCHAR* raw_command, RC::Unreal::FOutputDevice& output, UObject* ) {
			const auto command = first_token_lower( raw_command );
			if ( command == STR( "bhop.toggle" ) ) {
				config_.enabled = !config_.enabled;
			} else if ( command == STR( "bhop.autobhop" ) ) {
				config_.auto_bhop = !config_.auto_bhop;
			} else if ( command == STR( "bhop.speedcap" ) ) {
				config_.bunnyhop_speed_cap = !config_.bunnyhop_speed_cap;
			} else if ( command == STR( "bhop.duckroll" ) ) {
				config_.duck_roll = !config_.duck_roll;
			} else if ( command == STR( "bhop.rawmouse" ) ) {
				config_.raw_mouse_input = !config_.raw_mouse_input;
				apply_mouse_settings( );
			} else if ( command == STR( "bhop.reload" ) ) {
				if ( !reload_config( ) ) {
					output.Log( STR( "[bhop] configuration reload failed" ) );
					information.TrySetReturnValue( true );
					return;
				}
				apply_mouse_settings( );
			} else if ( command != STR( "bhop.status" ) ) {
				return;
			}

			const auto checksum = RC::to_wstring( bhop::checksum_hex( bhop::config_checksum( config_ ) ) );
			const auto status   = std::format( STR( "[bhop] enabled={} autobhop={} speedcap={} duckroll={} rawmouse={} checksum={} native={}\n" ), config_.enabled, config_.auto_bhop, config_.bunnyhop_speed_cap, config_.duck_roll, config_.raw_mouse_input, checksum, hook_ready_ );
			output.Log( status.c_str( ) );
			information.TrySetReturnValue( true );
		};
		console_hook_id_ = RC::Unreal::Hook::RegisterProcessConsoleExecCallback( callback, std::move( options ) );
	}
}  // namespace bhop::native
