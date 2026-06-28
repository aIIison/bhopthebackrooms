#define NOMINMAX
#include <DynamicOutput/DynamicOutput.hpp>
#include <MinHook.h>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FOutputDevice.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Windows.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <array>
#include <bhop/config.h>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
	using RC::Unreal::FBoolProperty;
	using RC::Unreal::FProperty;
	using RC::Unreal::FVector;
	using RC::Unreal::UClass;
	using RC::Unreal::UFunction;
	using RC::Unreal::UObject;

	constexpr std::uint32_t expected_time_date_stamp         = 0x6A291D66;
	constexpr std::uint32_t expected_size_of_image           = 0x05233000;
	constexpr std::uint8_t  movement_walking                 = 1;
	constexpr std::uint8_t  movement_falling                 = 3;
	constexpr double        gold_src_crouch_speed_multiplier = 1.0 / 3.0;
	constexpr double        mouse_reference_fps              = 120.0;

	using calc_velocity_fn_t = void ( * )( UObject*, float, float, bool, float );

	class c_etb_bhop_mod;
	c_etb_bhop_mod*    g_mod{};
	calc_velocity_fn_t g_original_calc_velocity{};

	enum class e_pointer_kind : std::uint8_t {
		unknown,
		context_object,
		context_vtable,
	};

	struct movement_properties_t {
		FProperty*     character_owner{};
		FProperty*     velocity{};
		FProperty*     acceleration{};
		FProperty*     max_acceleration{};
		FProperty*     movement_mode{};
		FProperty*     gravity_scale{};
		FProperty*     jump_z_velocity{};
		FProperty*     max_walk_speed{};
		FProperty*     max_walk_speed_crouched{};
		FProperty*     max_sprint_speed{};
		FBoolProperty* wants_to_crouch{};

		[[nodiscard]] auto complete( ) const noexcept -> bool {
			return character_owner && velocity && acceleration && max_acceleration &&
			    movement_mode && gravity_scale && jump_z_velocity &&
			    max_walk_speed && max_walk_speed_crouched && max_sprint_speed &&
			    wants_to_crouch;
		}
	};

	struct character_properties_t {
		FBoolProperty* pressed_jump{};
		FProperty*     jump_key_hold_time{};
		FBoolProperty* client_updating{};
		FBoolProperty* was_jumping{};
		FBoolProperty* can_move{};
		FBoolProperty* is_dead{};
		FBoolProperty* is_climbing{};
		FBoolProperty* is_climbing_ladder{};
		FBoolProperty* is_balancing{};
		FBoolProperty* is_falling_balance{};
		FBoolProperty* is_pushing{};
		FBoolProperty* is_crouched{};
		FBoolProperty* wants_to_crouch_after_landing{};

		[[nodiscard]] auto complete( ) const noexcept -> bool {
			return pressed_jump && jump_key_hold_time && client_updating &&
			    was_jumping && can_move && is_dead && is_climbing &&
			    is_climbing_ladder && is_balancing && is_falling_balance &&
			    is_pushing && is_crouched &&
			    wants_to_crouch_after_landing;
		}
	};

	struct movement_state_t {
		bool   overriding{};
		bool   replaying{};
		bool   has_crouch_state{};
		bool   last_wants_crouch{};
		double crouch_hold_seconds{};
		float  gravity_scale{};
		float  jump_z_velocity{};
		float  max_walk_speed{};
		float  max_walk_speed_crouched{};
		float  max_sprint_speed{};
	};

	struct axis_input_params_t {
		float axis_value{};
	};

	template < typename T >
	[[nodiscard]] auto property_value( UObject* object, FProperty* property ) noexcept -> T* {
		if ( !object || !property ) {
			return nullptr;
		}
		return static_cast< T* >( property->ContainerPtrToValuePtr< void >( object ) );
	}

	[[nodiscard]] auto bool_property( UObject* object, const RC::Unreal::TCHAR* name )
	    -> FBoolProperty* {
		if ( !object ) {
			return nullptr;
		}
		return RC::Unreal::CastField< FBoolProperty >(
		    object->GetPropertyByNameInChain( name ) );
	}

	[[nodiscard]] auto bool_value(
	    UObject*       object,
	    FBoolProperty* property,
	    bool           fallback = false ) noexcept -> bool {
		return object && property
		    ? property->GetPropertyValueInContainer( object )
		    : fallback;
	}

	[[nodiscard]] auto first_token_lower( const RC::Unreal::TCHAR* command )
	    -> RC::StringType {
		RC::StringType result{ command ? command : STR( "" ) };
		const auto     space = result.find( STR( ' ' ) );
		if ( space != RC::StringType::npos ) {
			result.resize( space );
		}
		std::transform( result.begin( ), result.end( ), result.begin( ), []( auto character ) {
			if ( character >= STR( 'A' ) && character <= STR( 'Z' ) ) {
				return static_cast< decltype( character ) >(
				    character + ( STR( 'a' ) - STR( 'A' ) ) );
			}
			return character;
		} );
		return result;
	}

	[[nodiscard]] auto validate_game_image( ) noexcept -> bool {
		const auto module = reinterpret_cast< std::byte* >( GetModuleHandleW( nullptr ) );
		if ( !module ) {
			return false;
		}
		const auto* dos = reinterpret_cast< const IMAGE_DOS_HEADER* >( module );
		if ( dos->e_magic != IMAGE_DOS_SIGNATURE ) {
			return false;
		}
		const auto* nt = reinterpret_cast< const IMAGE_NT_HEADERS64* >(
		    module + dos->e_lfanew );
		return nt->Signature == IMAGE_NT_SIGNATURE &&
		    nt->FileHeader.TimeDateStamp == expected_time_date_stamp &&
		    nt->OptionalHeader.SizeOfImage == expected_size_of_image;
	}

	[[nodiscard]] auto is_executable_game_address( const void* address ) noexcept -> bool {
		MEMORY_BASIC_INFORMATION information{};
		if ( !address ||
		     VirtualQuery( address, &information, sizeof( information ) ) !=
		         sizeof( information ) ) {
			return false;
		}

		const DWORD protection = information.Protect & 0xff;
		const bool  executable =
		    protection == PAGE_EXECUTE ||
		    protection == PAGE_EXECUTE_READ ||
		    protection == PAGE_EXECUTE_READWRITE ||
		    protection == PAGE_EXECUTE_WRITECOPY;
		return executable && information.Type == MEM_IMAGE &&
		    information.AllocationBase == GetModuleHandleW( nullptr );
	}

	auto clear_volatile_pointer_kinds(
	    std::array< e_pointer_kind, ZYDIS_REGISTER_MAX_VALUE + 1 >& kinds ) noexcept -> void {
		constexpr ZydisRegister volatile_registers[]{
			ZYDIS_REGISTER_RAX,
			ZYDIS_REGISTER_RCX,
			ZYDIS_REGISTER_RDX,
			ZYDIS_REGISTER_R8,
			ZYDIS_REGISTER_R9,
			ZYDIS_REGISTER_R10,
			ZYDIS_REGISTER_R11,
		};
		for ( const auto reg : volatile_registers ) {
			kinds[ reg ] = e_pointer_kind::unknown;
		}
	}

	// UE's generated execCalcVelocity wrapper eventually performs
	// P_THIS->CalcVelocity(...). Track the context pointer through simple MOVs
	// and accept a slot only when the wrapper contains exactly one indirect call
	// through that context's vtable.
	[[nodiscard]] auto resolve_calc_velocity_slot( UFunction* function )
	    -> std::optional< std::size_t > {
		if ( !function ) {
			return std::nullopt;
		}

		const auto* code = reinterpret_cast< const std::uint8_t* >(
		    function->GetFuncPtr( ) );
		if ( !is_executable_game_address( code ) ) {
			return std::nullopt;
		}

		ZydisDecoder decoder{};
		if ( !ZYAN_SUCCESS( ZydisDecoderInit(
		         &decoder,
		         ZYDIS_MACHINE_MODE_LONG_64,
		         ZYDIS_STACK_WIDTH_64 ) ) ) {
			return std::nullopt;
		}

		std::array< e_pointer_kind, ZYDIS_REGISTER_MAX_VALUE + 1 > kinds{};
		kinds[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context_object;
		std::vector< std::size_t > candidates;
		std::size_t                offset{};

		while ( offset < 1024 ) {
			ZydisDecodedInstruction instruction{};
			ZydisDecodedOperand     operands[ ZYDIS_MAX_OPERAND_COUNT ]{};
			if ( !ZYAN_SUCCESS( ZydisDecoderDecodeFull(
			         &decoder,
			         code + offset,
			         1024 - offset,
			         &instruction,
			         operands ) ) ) {
				return std::nullopt;
			}

			if ( instruction.mnemonic == ZYDIS_MNEMONIC_CALL ) {
				const auto& operand = operands[ 0 ];
				if ( operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
				     operand.mem.base <= ZYDIS_REGISTER_MAX_VALUE &&
				     kinds[ operand.mem.base ] == e_pointer_kind::context_vtable &&
				     operand.mem.index == ZYDIS_REGISTER_NONE &&
				     operand.mem.disp.has_displacement &&
				     operand.mem.disp.value >= 0 &&
				     ( operand.mem.disp.value % sizeof( void* ) ) == 0 ) {
					candidates.push_back(
					    static_cast< std::size_t >( operand.mem.disp.value ) /
					    sizeof( void* ) );
				}
				clear_volatile_pointer_kinds( kinds );
			} else if ( instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
			            operands[ 0 ].type == ZYDIS_OPERAND_TYPE_REGISTER ) {
				const auto     destination = operands[ 0 ].reg.value;
				e_pointer_kind new_kind    = e_pointer_kind::unknown;
				const auto&    source      = operands[ 1 ];
				if ( source.type == ZYDIS_OPERAND_TYPE_REGISTER ) {
					new_kind = kinds[ source.reg.value ];
				} else if ( source.type == ZYDIS_OPERAND_TYPE_MEMORY &&
				            source.mem.base <= ZYDIS_REGISTER_MAX_VALUE &&
				            kinds[ source.mem.base ] == e_pointer_kind::context_object &&
				            source.mem.index == ZYDIS_REGISTER_NONE &&
				            ( !source.mem.disp.has_displacement ||
				              source.mem.disp.value == 0 ) ) {
					new_kind = e_pointer_kind::context_vtable;
				}
				kinds[ destination ] = new_kind;
			} else {
				for ( std::uint8_t index = 0;
				      index < instruction.operand_count_visible;
				      ++index ) {
					const auto& operand = operands[ index ];
					if ( operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
					     operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE ) {
						kinds[ operand.reg.value ] = e_pointer_kind::unknown;
					}
				}
			}

			offset += instruction.length;
			if ( instruction.mnemonic == ZYDIS_MNEMONIC_RET ) {
				break;
			}
		}

		if ( candidates.size( ) != 1 ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Refused CalcVelocity hook: exec wrapper yielded {} " )
			        STR( "context-vtable call candidates.\n" ),
			    candidates.size( ) );
			return std::nullopt;
		}
		return candidates.front( );
	}

	void calc_velocity_detour(
	    UObject* movement,
	    float    delta_seconds,
	    float    friction,
	    bool     fluid,
	    float    braking_deceleration );

	class c_etb_bhop_mod final : public RC::CppUserModBase {
	public:
		c_etb_bhop_mod( ) {
			ModName               = STR( "bhop" );
			ModVersion            = STR( "1.0.0" );
			ModDescription        = STR( "GoldSrc movement for Escape the Backrooms" );
			ModAuthors            = STR( "hero" );
			ModIntendedSDKVersion = STR( "1.3.0" );
			g_mod                 = this;
		}

		~c_etb_bhop_mod( ) override {
			restore_mouse_settings( );
			if ( install_tick_id_ != RC::Unreal::Hook::ERROR_ID ) {
				RC::Unreal::Hook::UnregisterCallback( install_tick_id_ );
			}
			if ( console_hook_id_ != RC::Unreal::Hook::ERROR_ID ) {
				RC::Unreal::Hook::UnregisterCallback( console_hook_id_ );
			}
			if ( !jump_hook_ids_.empty( ) ) {
				RC::Unreal::UObjectGlobals::UnregisterHook(
				    STR( "/Script/Engine.Character:Jump" ),
				    jump_hook_ids_[ 0 ] );
			}
			if ( jump_hook_ids_.size( ) > 1 ) {
				RC::Unreal::UObjectGlobals::UnregisterHook(
				    STR( "/Script/Engine.Character:StopJumping" ),
				    jump_hook_ids_[ 1 ] );
			}
			if ( !crouch_hook_ids_.empty( ) ) {
				RC::Unreal::UObjectGlobals::UnregisterHook(
				    STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:" )
				        STR( "InpActEvt_Crouch_K2Node_InputActionEvent_1" ),
				    crouch_hook_ids_[ 0 ] );
			}
			if ( crouch_hook_ids_.size( ) > 1 ) {
				RC::Unreal::UObjectGlobals::UnregisterHook(
				    STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:" )
				        STR( "InpActEvt_Crouch_K2Node_InputActionEvent_0" ),
				    crouch_hook_ids_[ 1 ] );
			}
			if ( !mouse_hook_ids_.empty( ) ) {
				RC::Unreal::UObjectGlobals::UnregisterHook(
				    STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:" )
				        STR( "InpAxisEvt_Turn_K2Node_InputAxisEvent_157" ),
				    mouse_hook_ids_[ 0 ] );
			}
			if ( mouse_hook_ids_.size( ) > 1 ) {
				RC::Unreal::UObjectGlobals::UnregisterHook(
				    STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:" )
				        STR( "InpAxisEvt_LookUp_K2Node_InputAxisEvent_172" ),
				    mouse_hook_ids_[ 1 ] );
			}
			if ( hook_target_ ) {
				MH_DisableHook( hook_target_ );
				MH_RemoveHook( hook_target_ );
			}
			if ( minhook_initialized_ ) {
				MH_Uninitialize( );
			}
			g_mod = nullptr;
		}

		auto on_unreal_init( ) -> void override {
			config_path_ =
			    std::filesystem::path{
				    RC::StringType{
				        RC::UE4SSProgram::get_program( ).get_working_directory( ) }
			    } /
			    STR( "Mods" ) / STR( "bhop" ) / STR( "bhop.ini" );
			if ( !reload_config( ) ) {
				return;
			}
			if ( !validate_game_image( ) ) {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] Native hook disabled: Backrooms executable does " )
				        STR( "not match Steam build 23657885.\n" ) );
				return;
			}

			apply_mouse_settings( );
			register_commands( );
			register_jump_hooks( );
			RC::Unreal::Hook::FCallbackOptions options{};
			options.OwnerModName = STR( "bhop" );
			options.HookName     = STR( "InstallCalcVelocity" );
			install_tick_id_ =
			    RC::Unreal::Hook::RegisterEngineTickPreCallback(
			        [ this ](
			            auto&,
			            RC::Unreal::UEngine*,
			            float delta_seconds,
			            bool ) {
				frame_delta_seconds_ = delta_seconds;
				on_engine_tick( );
			},
			        std::move( options ) );

			RC::Output::send< RC::LogLevel::Normal >(
			    STR( "[bhop] Native adapter loaded; checksum={}. Waiting for a " )
			        STR( "live ETB character to validate CalcVelocity.\n" ),
			    RC::to_wstring( bhop::checksum_hex( bhop::config_checksum( config_ ) ) ) );
		}

		auto handle_calc_velocity(
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

			const bool ordinary_mode =
			    *mode == movement_walking || *mode == movement_falling;
			const bool special =
			    fluid || !ordinary_mode ||
			    !bool_value( owner, character_properties_.can_move, false ) ||
			    bool_value( owner, character_properties_.is_dead ) ||
			    bool_value( owner, character_properties_.is_climbing ) ||
			    bool_value( owner, character_properties_.is_climbing_ladder ) ||
			    bool_value( owner, character_properties_.is_balancing ) ||
			    bool_value( owner, character_properties_.is_falling_balance ) ||
			    bool_value( owner, character_properties_.is_pushing );

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

	private:
		auto call_original(
		    UObject* movement,
		    float    delta_seconds,
		    float    friction,
		    bool     fluid,
		    float    braking_deceleration ) const -> void {
			if ( g_original_calc_velocity ) {
				g_original_calc_velocity(
				    movement,
				    delta_seconds,
				    friction,
				    fluid,
				    braking_deceleration );
			}
		}

		[[nodiscard]] auto reload_config( ) -> bool {
			const bhop::config_result_t loaded = bhop::load_config( config_path_ );
			if ( !loaded.loaded ) {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] Configuration rejected: {}\n" ),
				    RC::to_wstring( loaded.error ) );
				return false;
			}
			config_ = loaded.value;
			return true;
		}

		auto apply_mouse_settings( ) -> void {
			if ( !input_settings_ ) {
				input_settings_ =
				    RC::Unreal::UObjectGlobals::StaticFindObject< UObject* >(
				        nullptr,
				        nullptr,
				        STR( "/Script/Engine.Default__InputSettings" ) );
			}
			if ( !mouse_smoothing_property_ && input_settings_ ) {
				mouse_smoothing_property_ =
				    bool_property( input_settings_, STR( "bEnableMouseSmoothing" ) );
			}
			if ( !input_settings_ || !mouse_smoothing_property_ ) {
				RC::Output::send< RC::LogLevel::Warning >(
				    STR( "[bhop] Raw mouse input unavailable: InputSettings could not be validated.\n" ) );
				return;
			}

			if ( !has_original_mouse_smoothing_ ) {
				original_mouse_smoothing_ =
				    mouse_smoothing_property_->GetPropertyValueInContainer( input_settings_ );
				has_original_mouse_smoothing_ = true;
			}
			mouse_smoothing_property_->SetPropertyValueInContainer(
			    input_settings_,
			    config_.raw_mouse_input ? false : original_mouse_smoothing_ );

			if ( !clear_mouse_smoothing_function_ ) {
				clear_mouse_smoothing_function_ =
				    RC::Unreal::UObjectGlobals::StaticFindObject< UFunction* >(
				        nullptr,
				        nullptr,
				        STR( "/Script/Engine.PlayerInput:ClearSmoothing" ) );
			}
			if ( clear_mouse_smoothing_function_ ) {
				std::vector< UObject* > player_inputs;
				RC::Unreal::UObjectGlobals::FindAllOf( STR( "PlayerInput" ), player_inputs );
				for ( UObject* player_input : player_inputs ) {
					player_input->ProcessEvent( clear_mouse_smoothing_function_, nullptr );
				}
			}
		}

		auto restore_mouse_settings( ) -> void {
			if ( input_settings_ && mouse_smoothing_property_ &&
			     has_original_mouse_smoothing_ ) {
				mouse_smoothing_property_->SetPropertyValueInContainer(
				    input_settings_,
				    original_mouse_smoothing_ );
			}
		}

		auto cache_properties( UObject* movement, UObject* owner ) -> bool {
			movement_properties_ = {
				.character_owner =
				    movement->GetPropertyByNameInChain( STR( "CharacterOwner" ) ),
				.velocity = movement->GetPropertyByNameInChain( STR( "Velocity" ) ),
				.acceleration =
				    movement->GetPropertyByNameInChain( STR( "Acceleration" ) ),
				.max_acceleration =
				    movement->GetPropertyByNameInChain( STR( "MaxAcceleration" ) ),
				.movement_mode =
				    movement->GetPropertyByNameInChain( STR( "MovementMode" ) ),
				.gravity_scale =
				    movement->GetPropertyByNameInChain( STR( "GravityScale" ) ),
				.jump_z_velocity =
				    movement->GetPropertyByNameInChain( STR( "JumpZVelocity" ) ),
				.max_walk_speed =
				    movement->GetPropertyByNameInChain( STR( "MaxWalkSpeed" ) ),
				.max_walk_speed_crouched =
				    movement->GetPropertyByNameInChain(
				        STR( "MaxWalkSpeedCrouched" ) ),
				.max_sprint_speed =
				    movement->GetPropertyByNameInChain( STR( "MaxSprintSpeed" ) ),
				.wants_to_crouch =
				    bool_property( movement, STR( "bWantsToCrouch" ) ),
			};
			character_properties_ = {
				.pressed_jump = bool_property( owner, STR( "bPressedJump" ) ),
				.jump_key_hold_time =
				    owner->GetPropertyByNameInChain( STR( "JumpKeyHoldTime" ) ),
				.client_updating = bool_property( owner, STR( "bClientUpdating" ) ),
				.was_jumping     = bool_property( owner, STR( "bWasJumping" ) ),
				.can_move        = bool_property( owner, STR( "CanMove" ) ),
				.is_dead         = bool_property( owner, STR( "IsDead" ) ),
				.is_climbing     = bool_property( owner, STR( "IsClimbing" ) ),
				.is_climbing_ladder =
				    bool_property( owner, STR( "IsClimbingLadder" ) ),
				.is_balancing = bool_property( owner, STR( "IsBalancing" ) ),
				.is_falling_balance =
				    bool_property( owner, STR( "IsFallingBalance" ) ),
				.is_pushing  = bool_property( owner, STR( "IsPushing" ) ),
				.is_crouched = bool_property( owner, STR( "bIsCrouched" ) ),
				.wants_to_crouch_after_landing =
				    bool_property( owner, STR( "WantsToCrouchAfterLanding" ) ),
			};
			return movement_properties_.complete( ) &&
			    character_properties_.complete( );
		}

		auto set_movement_mode(
		    UObject*     movement,
		    std::uint8_t mode ) const -> bool {
			if ( !movement || !set_movement_mode_function_ ||
			     !set_movement_mode_property_ ) {
				return false;
			}

			std::array< std::uint8_t, 16 > parameters{};
			*set_movement_mode_property_
			     ->ContainerPtrToValuePtr< std::uint8_t >( parameters.data( ) ) =
			    mode;
			movement->ProcessEvent(
			    set_movement_mode_function_,
			    parameters.data( ) );
			return true;
		}

		auto on_engine_tick( ) -> void {
			std::vector< UObject* > characters;
			RC::Unreal::UObjectGlobals::FindAllOf(
			    STR( "BPCharacter_Demo_C" ),
			    characters );
			for ( UObject* character : characters ) {
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
					       *mode == movement_falling ) &&
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

			if ( hook_ready_ || install_attempted_ || characters.empty( ) ) {
				return;
			}
			install_attempted_ = true;
			install_hook( characters.front( ) );
		}

		auto install_hook( UObject* character ) -> void {
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

			UFunction* wrapper =
			    RC::Unreal::UObjectGlobals::StaticFindObject< UFunction* >(
			        nullptr,
			        nullptr,
			        STR( "/Script/Engine.CharacterMovementComponent:CalcVelocity" ) );
			const auto slot = resolve_calc_velocity_slot( wrapper );
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
			if ( !is_executable_game_address( target ) ) {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] Native hook disabled: CalcVelocity slot {} " )
				        STR( "did not point into executable game code.\n" ),
				    *slot );
				return;
			}

			if ( MH_Initialize( ) != MH_OK ) {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] Native hook disabled: MinHook initialization " )
				        STR( "failed.\n" ) );
				return;
			}
			minhook_initialized_ = true;
			if ( MH_CreateHook(
			         target,
			         reinterpret_cast< void* >( &calc_velocity_detour ),
			         reinterpret_cast< void** >( &g_original_calc_velocity ) ) != MH_OK ||
			     MH_EnableHook( target ) != MH_OK ) {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] Native hook disabled: CalcVelocity detour could " )
				        STR( "not be installed.\n" ) );
				return;
			}

			hook_target_ = target;
			hook_ready_  = true;
			RC::Output::send< RC::LogLevel::Normal >(
			    STR( "[bhop] Native CalcVelocity hook active (slot {}, target {}).\n" ),
			    *slot,
			    target );
		}

		auto apply_properties( UObject* movement, movement_state_t& state ) -> void {
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
			if ( !gravity || !jump || !walk || !crouch || !sprint ) {
				return;
			}
			if ( !state.overriding ) {
				state.overriding              = true;
				state.gravity_scale           = *gravity;
				state.jump_z_velocity         = *jump;
				state.max_walk_speed          = *walk;
				state.max_walk_speed_crouched = *crouch;
				state.max_sprint_speed        = *sprint;
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
		}

		auto restore_properties( UObject* movement, movement_state_t& state ) -> void {
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
			state.overriding = false;
		}

		auto register_jump_hooks( ) -> void {
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

		auto register_crouch_hooks( ) -> void {
			register_crouch_event( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpActEvt_Crouch_K2Node_InputActionEvent_1" ), 1 );
			register_crouch_event( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpActEvt_Crouch_K2Node_InputActionEvent_0" ), 0 );
		}

		auto register_crouch_event( const RC::StringType& path, int event_id ) -> void {
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
				} else {
					// Preserve +duck for one predicted move so short taps are
					// represented in compressed client moves.
					crouch_release_pending_[ context.Context ] = true;
				}
			};
			const auto no_op = []( RC::Unreal::UnrealScriptFunctionCallableContext&, void* ) {};
			crouch_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook( path, callback, no_op, nullptr ) );
		}

		auto register_mouse_hooks( ) -> void {
			const auto callback = [ this ]( RC::Unreal::UnrealScriptFunctionCallableContext& context, void* ) { correct_mouse_input( context ); };
			const auto no_op    = []( RC::Unreal::UnrealScriptFunctionCallableContext&, void* ) {};
			mouse_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_Turn_K2Node_InputAxisEvent_157" ), callback, no_op, nullptr ) );
			mouse_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_LookUp_K2Node_InputAxisEvent_172" ), callback, no_op, nullptr ) );
		}

		auto correct_mouse_input( RC::Unreal::UnrealScriptFunctionCallableContext& context ) -> void {
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

		auto register_commands( ) -> void {
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

	private:
		bhop::config_t                                   config_{};
		std::filesystem::path                            config_path_{};
		movement_properties_t                            movement_properties_{};
		character_properties_t                           character_properties_{};
		UClass*                                          character_class_{};
		UFunction*                                       set_movement_mode_function_{};
		FProperty*                                       set_movement_mode_property_{};
		FProperty*                                       mouse_delta_seconds_property_{};
		UObject*                                         input_settings_{};
		FBoolProperty*                                   mouse_smoothing_property_{};
		UFunction*                                       clear_mouse_smoothing_function_{};
		std::unordered_map< UObject*, movement_state_t > states_{};
		std::unordered_map< UObject*, bool >             jump_held_{};
		std::unordered_map< UObject*, bool >             crouch_held_{};
		std::unordered_map< UObject*, bool >             crouch_release_pending_{};
		std::optional< int >                             crouch_press_event_{};
		std::vector< std::pair< int, int > >             jump_hook_ids_{};
		std::vector< std::pair< int, int > >             crouch_hook_ids_{};
		std::vector< std::pair< int, int > >             mouse_hook_ids_{};
		RC::Unreal::Hook::GlobalCallbackId               install_tick_id_{};
		RC::Unreal::Hook::GlobalCallbackId               console_hook_id_{};
		void*                                            hook_target_{};
		bool                                             install_attempted_{};
		bool                                             hook_ready_{};
		bool                                             minhook_initialized_{};
		bool                                             original_mouse_smoothing_{};
		bool                                             has_original_mouse_smoothing_{};
		double                                           frame_delta_seconds_{ 1.0 / 60.0 };
	};

	void calc_velocity_detour(
	    UObject* movement,
	    float    delta_seconds,
	    float    friction,
	    bool     fluid,
	    float    braking_deceleration ) {
		if ( g_mod ) {
			g_mod->handle_calc_velocity(
			    movement,
			    delta_seconds,
			    friction,
			    fluid,
			    braking_deceleration );
		} else if ( g_original_calc_velocity ) {
			g_original_calc_velocity(
			    movement,
			    delta_seconds,
			    friction,
			    fluid,
			    braking_deceleration );
		}
	}
}  // namespace

extern "C" {
__declspec( dllexport ) RC::CppUserModBase* start_mod( ) {
	return new c_etb_bhop_mod( );
}

__declspec( dllexport ) void uninstall_mod( RC::CppUserModBase* mod ) {
	delete mod;
}
}
