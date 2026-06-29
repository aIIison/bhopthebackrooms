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
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
	using RC::Unreal::FBoolProperty;
	using RC::Unreal::FProperty;
	using RC::Unreal::FRotator;
	using RC::Unreal::FVector;
	using RC::Unreal::UClass;
	using RC::Unreal::UFunction;
	using RC::Unreal::UObject;

	constexpr std::uint32_t expected_time_date_stamp         = 0x6A291D66;
	constexpr std::uint32_t expected_size_of_image           = 0x05233000;
	constexpr std::uint8_t  movement_walking                 = 1;
	constexpr std::uint8_t  movement_falling                 = 3;
	constexpr std::uint8_t  movement_swimming                = 4;
	constexpr std::uint8_t  movement_flying                  = 5;
	constexpr double        gold_src_crouch_speed_multiplier = 1.0 / 3.0;
	constexpr double        mouse_reference_fps              = 120.0;

	using calc_velocity_fn_t = void ( * )( UObject*, float, float, bool, float );
	using can_crouch_fn_t     = bool ( * )( UObject* );

	class c_etb_bhop_mod;
	c_etb_bhop_mod*    g_mod{ };
	calc_velocity_fn_t g_original_calc_velocity{ };
	can_crouch_fn_t     g_original_can_crouch{ };

	enum class e_pointer_kind : std::uint8_t {
		unknown,
		context_object,
		context_vtable,
	};

	struct movement_properties_t {
		FProperty*     character_owner{ };
		FProperty*     velocity{ };
		FProperty*     acceleration{ };
		FProperty*     max_acceleration{ };
		FProperty*     movement_mode{ };
		FProperty*     gravity_scale{ };
		FProperty*     jump_z_velocity{ };
		FProperty*     max_walk_speed{ };
		FProperty*     max_walk_speed_crouched{ };
		FProperty*     max_sprint_speed{ };
		FProperty*     max_swim_speed{ };
		FBoolProperty* wants_to_crouch{ };

		[[nodiscard]] auto complete( ) const noexcept -> bool {
			return character_owner && velocity && acceleration && max_acceleration &&
			    movement_mode && gravity_scale && jump_z_velocity &&
			    max_walk_speed && max_walk_speed_crouched && max_sprint_speed &&
			    max_swim_speed && wants_to_crouch;
		}
	};

	struct character_properties_t {
		FBoolProperty* pressed_jump{ };
		FProperty*     jump_key_hold_time{ };
		FProperty*     controller{ };
		FBoolProperty* client_updating{ };
		FBoolProperty* was_jumping{ };
		FBoolProperty* can_move{ };
		FBoolProperty* is_dead{ };
		FBoolProperty* is_climbing{ };
		FBoolProperty* is_climbing_ladder{ };
		FBoolProperty* is_balancing{ };
		FBoolProperty* is_falling_balance{ };
		FBoolProperty* is_pushing{ };
		FBoolProperty* is_crouched{ };
		FBoolProperty* has_water_physics{ };
		FBoolProperty* wants_to_crouch_after_landing{ };

		[[nodiscard]] auto complete( ) const noexcept -> bool {
			return pressed_jump && jump_key_hold_time && controller && client_updating &&
			    was_jumping && can_move && is_dead && is_climbing &&
			    is_climbing_ladder && is_balancing && is_falling_balance &&
			    is_pushing && is_crouched && has_water_physics &&
			    wants_to_crouch_after_landing;
		}
	};

	struct movement_state_t {
		bool   overriding{ };
		bool   replaying{ };
		bool   has_crouch_state{ };
		bool   last_wants_crouch{ };
		double crouch_hold_seconds{ };
		float  gravity_scale{ };
		float  jump_z_velocity{ };
		float  max_walk_speed{ };
		float  max_walk_speed_crouched{ };
		float  max_sprint_speed{ };
		float  max_swim_speed{ };
	};

	struct axis_input_params_t {
		float axis_value{ };
	};

	struct ladder_overlap_params_t {
		UObject* overlapped_component{ };
		UObject* other_actor{ };
	};

	struct ladder_state_t {
		UObject* ladder{ };
		bhop::vec3_t start{ };
		bhop::vec3_t end{ };
		double half_width_cm{ };
		double contact_depth_cm{ };
		bool     on_floor{ };
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
		MEMORY_BASIC_INFORMATION information{ };
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
	[[nodiscard]] auto resolve_virtual_slot( UFunction* function, void** vtable = nullptr, bool log_failure = true )
	    -> std::optional< std::size_t > {
		if ( !function ) {
			return std::nullopt;
		}

		const auto* code = reinterpret_cast< const std::uint8_t* >(
		    function->GetFuncPtr( ) );
		if ( !is_executable_game_address( code ) ) {
			return std::nullopt;
		}

		ZydisDecoder decoder{ };
		if ( !ZYAN_SUCCESS( ZydisDecoderInit(
		         &decoder,
		         ZYDIS_MACHINE_MODE_LONG_64,
		         ZYDIS_STACK_WIDTH_64 ) ) ) {
			return std::nullopt;
		}

		std::array< e_pointer_kind, ZYDIS_REGISTER_MAX_VALUE + 1 > kinds{ };
		kinds[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context_object;
		std::vector< std::size_t > candidates;
		std::vector< void* >       direct_targets;
		std::size_t                offset{ };

		while ( offset < 1024 ) {
			ZydisDecodedInstruction instruction{ };
			ZydisDecodedOperand     operands[ ZYDIS_MAX_OPERAND_COUNT ]{ };
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
				} else if ( operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative ) {
					ZyanU64 absolute_address{ };
					if ( ZYAN_SUCCESS( ZydisCalcAbsoluteAddress(
					         &instruction,
					         &operand,
					         reinterpret_cast< ZyanU64 >( code + offset ),
					         &absolute_address ) ) ) {
						direct_targets.push_back( reinterpret_cast< void* >( absolute_address ) );
					}
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

		if ( candidates.empty( ) && vtable ) {
			for ( void* target : direct_targets ) {
				for ( std::size_t slot = 0; slot < 384; ++slot ) {
					if ( vtable[ slot ] == target ) {
						candidates.push_back( slot );
					}
				}
			}
			std::sort( candidates.begin( ), candidates.end( ) );
			candidates.erase( std::unique( candidates.begin( ), candidates.end( ) ), candidates.end( ) );
		}

		if ( candidates.size( ) != 1 ) {
			if ( !log_failure ) {
				return std::nullopt;
			}
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Refused virtual hook: exec wrapper yielded {} " )
			        STR( "context-vtable call candidates.\n" ),
			    candidates.size( ) );
			return std::nullopt;
		}
		return candidates.front( );
	}

	[[nodiscard]] auto resolve_can_crouch_slot( void** vtable, std::int64_t movement_mode_offset, std::int64_t updated_component_offset ) -> std::optional< std::size_t > {
		if ( !vtable || movement_mode_offset < 0 || updated_component_offset < 0 ) {
			return std::nullopt;
		}

		ZydisDecoder decoder{ };
		if ( !ZYAN_SUCCESS( ZydisDecoderInit( &decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64 ) ) ) {
			return std::nullopt;
		}

		std::vector< std::size_t > falling_predicates;
		for ( std::size_t slot = 0; slot < 384; ++slot ) {
			auto* code = static_cast< const std::uint8_t* >( vtable[ slot ] );
			if ( !is_executable_game_address( code ) ) {
				continue;
			}

			std::array< e_pointer_kind, ZYDIS_REGISTER_MAX_VALUE + 1 > kinds{ };
			kinds[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context_object;
			bool        reads_movement_mode{ };
			bool        compares_falling{ };
			bool        reached_return{ };
			std::size_t offset{ };
			while ( offset < 64 ) {
				ZydisDecodedInstruction instruction{ };
				ZydisDecodedOperand     operands[ ZYDIS_MAX_OPERAND_COUNT ]{ };
				if ( !ZYAN_SUCCESS( ZydisDecoderDecodeFull( &decoder, code + offset, 64 - offset, &instruction, operands ) ) ) {
					break;
				}

				for ( std::uint8_t index = 0; index < instruction.operand_count_visible; ++index ) {
					const auto& operand = operands[ index ];
					if ( operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
					     operand.mem.base <= ZYDIS_REGISTER_MAX_VALUE &&
					     kinds[ operand.mem.base ] == e_pointer_kind::context_object &&
					     operand.mem.disp.has_displacement &&
					     operand.mem.disp.value == movement_mode_offset ) {
						reads_movement_mode = true;
					}
					if ( instruction.mnemonic == ZYDIS_MNEMONIC_CMP && operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE ) {
						compares_falling |= operand.imm.value.u == movement_falling;
					}
				}

				if ( instruction.mnemonic == ZYDIS_MNEMONIC_MOV && operands[ 0 ].type == ZYDIS_OPERAND_TYPE_REGISTER ) {
					const auto     destination = operands[ 0 ].reg.value;
					e_pointer_kind new_kind    = e_pointer_kind::unknown;
					if ( operands[ 1 ].type == ZYDIS_OPERAND_TYPE_REGISTER ) {
						new_kind = kinds[ operands[ 1 ].reg.value ];
					}
					kinds[ destination ] = new_kind;
				} else {
					for ( std::uint8_t index = 0; index < instruction.operand_count_visible; ++index ) {
						const auto& operand = operands[ index ];
						if ( operand.type == ZYDIS_OPERAND_TYPE_REGISTER && operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE ) {
							kinds[ operand.reg.value ] = e_pointer_kind::unknown;
						}
					}
				}

				offset += instruction.length;
				if ( instruction.mnemonic == ZYDIS_MNEMONIC_RET ) {
					reached_return = true;
					break;
				}
			}

			if ( reached_return && reads_movement_mode && compares_falling ) {
				falling_predicates.push_back( slot );
			}
		}

		if ( falling_predicates.size( ) != 1 || falling_predicates.front( ) + 1 >= 384 ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Refused CanCrouch hook: IsFalling scan yielded {} candidates.\n" ),
			    falling_predicates.size( ) );
			return std::nullopt;
		}
		const auto falling_slot = falling_predicates.front( );
		const auto ground_slot  = falling_slot + 1;

		std::vector< std::size_t > candidates;
		std::vector< void* >      visited;
		for ( std::size_t slot = 0; slot < 384; ++slot ) {
			auto* code = static_cast< const std::uint8_t* >( vtable[ slot ] );
			if ( !is_executable_game_address( code ) || std::find( visited.begin( ), visited.end( ), vtable[ slot ] ) != visited.end( ) ) {
				continue;
			}
			visited.push_back( vtable[ slot ] );

			std::array< e_pointer_kind, ZYDIS_REGISTER_MAX_VALUE + 1 > kinds{ };
			kinds[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context_object;
			bool        calls_falling{ };
			bool        calls_ground{ };
			bool        reads_updated_component{ };
			bool        reached_return{ };
			std::size_t offset{ };
			while ( offset < 192 ) {
				ZydisDecodedInstruction instruction{ };
				ZydisDecodedOperand     operands[ ZYDIS_MAX_OPERAND_COUNT ]{ };
				if ( !ZYAN_SUCCESS( ZydisDecoderDecodeFull( &decoder, code + offset, 192 - offset, &instruction, operands ) ) ) {
					break;
				}

				for ( std::uint8_t index = 0; index < instruction.operand_count_visible; ++index ) {
					const auto& operand = operands[ index ];
					if ( operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
					     operand.mem.base <= ZYDIS_REGISTER_MAX_VALUE &&
					     kinds[ operand.mem.base ] == e_pointer_kind::context_object &&
					     operand.mem.disp.has_displacement &&
					     operand.mem.disp.value == updated_component_offset ) {
						reads_updated_component = true;
					}
				}

				if ( instruction.mnemonic == ZYDIS_MNEMONIC_CALL ) {
					const auto& operand = operands[ 0 ];
					if ( operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
					     operand.mem.base <= ZYDIS_REGISTER_MAX_VALUE &&
					     kinds[ operand.mem.base ] == e_pointer_kind::context_vtable &&
					     operand.mem.disp.has_displacement ) {
						const auto called_slot = static_cast< std::size_t >( operand.mem.disp.value ) / sizeof( void* );
						calls_falling |= called_slot == falling_slot;
						calls_ground |= called_slot == ground_slot;
					} else if ( operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative ) {
						ZyanU64 target{ };
						if ( ZYAN_SUCCESS( ZydisCalcAbsoluteAddress( &instruction, &operand, reinterpret_cast< ZyanU64 >( code + offset ), &target ) ) ) {
							calls_falling |= reinterpret_cast< void* >( target ) == vtable[ falling_slot ];
							calls_ground |= reinterpret_cast< void* >( target ) == vtable[ ground_slot ];
						}
					}
					clear_volatile_pointer_kinds( kinds );
				} else if ( instruction.mnemonic == ZYDIS_MNEMONIC_MOV && operands[ 0 ].type == ZYDIS_OPERAND_TYPE_REGISTER ) {
					const auto     destination = operands[ 0 ].reg.value;
					e_pointer_kind new_kind    = e_pointer_kind::unknown;
					const auto&    source      = operands[ 1 ];
					if ( source.type == ZYDIS_OPERAND_TYPE_REGISTER ) {
						new_kind = kinds[ source.reg.value ];
					} else if ( source.type == ZYDIS_OPERAND_TYPE_MEMORY &&
					            source.mem.base <= ZYDIS_REGISTER_MAX_VALUE &&
					            kinds[ source.mem.base ] == e_pointer_kind::context_object &&
					            source.mem.index == ZYDIS_REGISTER_NONE &&
					            ( !source.mem.disp.has_displacement || source.mem.disp.value == 0 ) ) {
						new_kind = e_pointer_kind::context_vtable;
					}
					kinds[ destination ] = new_kind;
				} else {
					for ( std::uint8_t index = 0; index < instruction.operand_count_visible; ++index ) {
						const auto& operand = operands[ index ];
						if ( operand.type == ZYDIS_OPERAND_TYPE_REGISTER && operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE ) {
							kinds[ operand.reg.value ] = e_pointer_kind::unknown;
						}
					}
				}

				offset += instruction.length;
				if ( instruction.mnemonic == ZYDIS_MNEMONIC_RET ) {
					reached_return = true;
					break;
				}
			}
			if ( reached_return && calls_falling && calls_ground && reads_updated_component ) {
				candidates.push_back( slot );
			}
		}

		if ( candidates.size( ) != 1 ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Refused CanCrouch hook: call-graph scan yielded {} candidates.\n" ),
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
	bool can_crouch_detour( UObject* movement );

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
			if ( !movement_input_hook_ids_.empty( ) ) {
				RC::Unreal::UObjectGlobals::UnregisterHook(
				    STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_MoveForward_K2Node_InputAxisEvent_181" ),
				    movement_input_hook_ids_[ 0 ] );
			}
			if ( movement_input_hook_ids_.size( ) > 1 ) {
				RC::Unreal::UObjectGlobals::UnregisterHook(
				    STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_MoveRight_K2Node_InputAxisEvent_192" ),
				    movement_input_hook_ids_[ 1 ] );
			}
			if ( !ladder_hook_ids_.empty( ) ) {
				RC::Unreal::UObjectGlobals::UnregisterHook(
				    ladder_overlap_function_,
				    ladder_hook_ids_[ 0 ] );
			}
			if ( hook_target_ ) {
				MH_DisableHook( hook_target_ );
				MH_RemoveHook( hook_target_ );
			}
			if ( can_crouch_target_ ) {
				MH_DisableHook( can_crouch_target_ );
				MH_RemoveHook( can_crouch_target_ );
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
			RC::Unreal::Hook::FCallbackOptions options{ };
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

			if ( const auto ladder = ladder_states_.find( owner ); ladder != ladder_states_.end( ) ) {
				if ( config_.enabled ) {
					restore_properties( movement, state );
					handle_ladder_velocity( movement, owner, *mode, ladder->second );
					return;
				}
				ladder_states_.erase( ladder );
				set_movement_mode( movement, movement_falling );
			}

			const bool blocked =
			    !bool_value( owner, character_properties_.can_move, false ) ||
			    bool_value( owner, character_properties_.is_dead ) ||
			    bool_value( owner, character_properties_.is_climbing ) ||
			    bool_value( owner, character_properties_.is_climbing_ladder ) ||
			    bool_value( owner, character_properties_.is_balancing ) ||
			    bool_value( owner, character_properties_.is_falling_balance ) ||
			    bool_value( owner, character_properties_.is_pushing );
			if ( water_states_.contains( owner ) ) {
				if ( config_.enabled && !blocked ) {
					apply_properties( movement, state );
					handle_water_velocity( movement, owner, delta_seconds );
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
				RC::Unreal::FHitResult sweep_result{ };
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

		[[nodiscard]] auto character_is_swimming( UObject* owner ) const -> bool {
			if ( !owner ) {
				return false;
			}
			auto** movement_storage = owner->GetValuePtrByPropertyNameInChain< UObject* >( STR( "CharacterMovement" ) );
			UObject* movement       = movement_storage ? *movement_storage : nullptr;
			const auto* mode        = property_value< std::uint8_t >( movement, movement_properties_.movement_mode );
			return water_states_.contains( owner ) ||
			    ( mode && *mode == movement_swimming ) ||
			    bool_value( owner, character_properties_.has_water_physics );
		}

		[[nodiscard]] auto should_allow_goldsrc_crouch( UObject* movement ) const -> bool {
			if ( !config_.enabled || !movement || !movement_properties_.character_owner ) {
				return false;
			}
			auto** owner_storage = property_value< UObject* >( movement, movement_properties_.character_owner );
			return owner_storage && *owner_storage &&
			    ( ladder_states_.contains( *owner_storage ) || character_is_swimming( *owner_storage ) );
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
				.max_swim_speed =
				    movement->GetPropertyByNameInChain( STR( "MaxSwimSpeed" ) ),
				.wants_to_crouch =
				    bool_property( movement, STR( "bWantsToCrouch" ) ),
			};
			character_properties_ = {
				.pressed_jump = bool_property( owner, STR( "bPressedJump" ) ),
				.jump_key_hold_time =
				    owner->GetPropertyByNameInChain( STR( "JumpKeyHoldTime" ) ),
				.controller      = owner->GetPropertyByNameInChain( STR( "Controller" ) ),
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
				.has_water_physics =
				    bool_property( owner, STR( "HasWaterPhysics" ) ),
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

			std::array< std::uint8_t, 16 > parameters{ };
			*set_movement_mode_property_
			     ->ContainerPtrToValuePtr< std::uint8_t >( parameters.data( ) ) =
			    mode;
			movement->ProcessEvent(
			    set_movement_mode_function_,
			    parameters.data( ) );
			return true;
		}

		[[nodiscard]] auto movement_is_in_water( UObject* movement, UObject* owner ) -> bool {
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

			alignas( 16 ) std::array< std::uint8_t, 16 > parameters{ };
			movement->ProcessEvent( get_physics_volume_function_, parameters.data( ) );
			auto** volume_storage = get_physics_volume_return_property_->ContainerPtrToValuePtr< UObject* >( parameters.data( ) );
			UObject* volume       = volume_storage ? *volume_storage : nullptr;
			if ( !volume ) {
				return false;
			}
			if ( !water_volume_property_ ) {
				water_volume_property_ = bool_property( volume, STR( "bWaterVolume" ) );
			}
			return bool_value( volume, water_volume_property_ );
		}

		auto handle_water_velocity( UObject* movement, UObject* owner, double delta_seconds ) -> void {
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
			const double input_threshold      = std::max( 1.0, static_cast< double >( *max_acceleration ) * 0.01 );
			const auto   forward_input        = forward_input_.find( owner );
			const auto   side_input           = side_input_.find( owner );
			const double forward_move         = forward_input != forward_input_.end( )
			             ? std::clamp( static_cast< double >( forward_input->second ), -1.0, 1.0 )
			             : ( std::abs( forward_acceleration ) >= input_threshold ? std::copysign( 1.0, forward_acceleration ) : 0.0 );
			const double side_move = side_input != side_input_.end( )
			             ? std::clamp( static_cast< double >( side_input->second ), -1.0, 1.0 )
			             : ( std::abs( side_acceleration ) >= input_threshold ? std::copysign( 1.0, side_acceleration ) : 0.0 );
			const auto*  jump_key_hold_time   = property_value< float >( owner, character_properties_.jump_key_hold_time );
			const bool   swim_up              = jump_held_[ owner ] ||
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

		[[nodiscard]] auto read_vector_return( UObject* object, UFunction*& function, FProperty*& return_property, const RC::Unreal::TCHAR* function_name ) -> std::optional< bhop::vec3_t > {
			if ( !function ) {
				function = object->GetFunctionByNameInChain( function_name );
				if ( function ) {
					return_property = function->GetPropertyByNameInChain( STR( "ReturnValue" ) );
				}
			}
			if ( !function || !return_property ) {
				return std::nullopt;
			}

			alignas( 16 ) std::array< std::uint8_t, 32 > parameters{ };
			object->ProcessEvent( function, parameters.data( ) );
			const auto* value = return_property->ContainerPtrToValuePtr< FVector >( parameters.data( ) );
			if ( !value ) {
				return std::nullopt;
			}
			return bhop::vec3_t{ value->X( ), value->Y( ), value->Z( ) };
		}

		[[nodiscard]] auto ladder_contains_player( const ladder_state_t& state, const bhop::vec3_t& player, const bhop::vec3_t& normal ) const -> bool {
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
			bhop::vec3_t side{
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
			const double along         = relative.x * axis.x + relative.y * axis.y + relative.z * axis.z;
			const double from_plane    = relative.x * normal.x + relative.y * normal.y + relative.z * normal.z;
			const double from_center   = relative.x * side.x + relative.y * side.y + relative.z * side.z;
			const double axial_margin  = bhop::source_to_cm( 36.0 );
			const double hull_radius   = bhop::source_to_cm( 16.0 );

			return along >= -axial_margin &&
			    along <= length + axial_margin &&
			    std::abs( from_plane ) <= state.contact_depth_cm + hull_radius &&
			    std::abs( from_center ) <= state.half_width_cm + hull_radius;
		}

		auto detach_from_ladder( UObject* movement, UObject* owner ) -> void {
			ladder_states_.erase( owner );
			set_movement_mode( movement, movement_falling );
		}

		[[nodiscard]] auto ladder_normal_for_player( UObject* ladder, UObject* owner ) const -> bhop::vec3_t {
			auto*      ladder_actor    = static_cast< RC::Unreal::AActor* >( ladder );
			auto*      player_actor    = static_cast< RC::Unreal::AActor* >( owner );
			const auto ladder_location = ladder_actor->K2_GetActorLocation( );
			const auto player_location = player_actor->K2_GetActorLocation( );
			const auto ladder_rotation = ladder_actor->K2_GetActorRotation( );
			const auto pitch           = ladder_rotation.GetPitch( ) * std::numbers::pi / 180.0;
			const auto yaw             = ladder_rotation.GetYaw( ) * std::numbers::pi / 180.0;
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

		auto handle_ladder_velocity( UObject* movement, UObject* owner, std::uint8_t, ladder_state_t& state ) -> void {
			auto* velocity         = property_value< FVector >( movement, movement_properties_.velocity );
			auto* acceleration     = property_value< FVector >( movement, movement_properties_.acceleration );
			auto* max_acceleration = property_value< float >( movement, movement_properties_.max_acceleration );
			if ( !velocity || !acceleration || !max_acceleration ) {
				detach_from_ladder( movement, owner );
				return;
			}

			auto*        ladder_actor    = static_cast< RC::Unreal::AActor* >( state.ladder );
			auto*        player_actor    = static_cast< RC::Unreal::AActor* >( owner );
			const auto   player_location = player_actor->K2_GetActorLocation( );
			const auto   ladder_normal  = ladder_normal_for_player( ladder_actor, player_actor );
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

			const auto result = bhop::calculate_ladder_velocity( {
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
			velocity->SetX( result.velocity_cm.x );
			velocity->SetY( result.velocity_cm.y );
			velocity->SetZ( result.velocity_cm.z );
			state.on_floor = false;

			if ( result.detached ) {
				detach_from_ladder( movement, owner );
			}
		}

		[[nodiscard]] auto activate_ladder( UObject* ladder, UObject* owner, bool require_contact = false ) -> bool {
			if ( !hook_ready_ || !config_.enabled || !owner || !owner->IsA( character_class_ ) ||
			     !bool_value( owner, character_properties_.can_move, false ) ||
			     bool_value( owner, character_properties_.is_dead ) ) {
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

			double half_width_cm     = bhop::source_to_cm( 32.0 );
			double contact_depth_cm = bhop::source_to_cm( 16.0 );
			auto** box_storage       = ladder->GetValuePtrByPropertyNameInChain< UObject* >( STR( "Box1" ) );
			UObject* box             = box_storage ? *box_storage : nullptr;
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
				auto*      player_actor    = static_cast< RC::Unreal::AActor* >( owner );
				const auto player_location = player_actor->K2_GetActorLocation( );
				const bhop::vec3_t player_position{ player_location.X( ), player_location.Y( ), player_location.Z( ) };
				const auto normal = ladder_normal_for_player( ladder, owner );
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

		auto update_water_state( UObject* owner ) -> void {
			if ( !hook_ready_ || !owner ) {
				return;
			}
			auto**   movement_storage = owner->GetValuePtrByPropertyNameInChain< UObject* >( STR( "CharacterMovement" ) );
			UObject* movement         = movement_storage ? *movement_storage : nullptr;
			auto*    mode             = property_value< std::uint8_t >( movement, movement_properties_.movement_mode );
			if ( !movement || !mode ) {
				return;
			}

			const bool blocked =
			    !bool_value( owner, character_properties_.can_move, false ) ||
			    bool_value( owner, character_properties_.is_dead ) ||
			    bool_value( owner, character_properties_.is_climbing ) ||
			    bool_value( owner, character_properties_.is_climbing_ladder ) ||
			    bool_value( owner, character_properties_.is_balancing ) ||
			    bool_value( owner, character_properties_.is_falling_balance ) ||
			    bool_value( owner, character_properties_.is_pushing );
			const bool in_water = movement_is_in_water( movement, owner );
			if ( water_states_.contains( owner ) ) {
				if ( !config_.enabled || blocked || !in_water ) {
					water_states_.erase( owner );
					restore_properties( movement, states_[ movement ] );
					if ( *mode == movement_flying ) {
						set_movement_mode( movement, movement_falling );
					}
				} else if ( *mode != movement_flying ) {
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
			water_states_[ owner ] = true;
			if ( set_movement_mode( movement, movement_flying ) && !water_logged_ ) {
				RC::Output::send< RC::LogLevel::Normal >( STR( "[bhop] GoldSrc water state active.\n" ) );
				water_logged_ = true;
			}
		}

		auto on_engine_tick( ) -> void {
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
			if ( !is_executable_game_address( target ) ) {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] Native hook disabled: CalcVelocity slot {} " )
				        STR( "did not point into executable game code.\n" ),
				    *slot );
				return;
			}
			FProperty* updated_component_property = movement->GetPropertyByNameInChain( STR( "UpdatedComponent" ) );
			const auto can_crouch_slot = updated_component_property
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
			if ( !is_executable_game_address( can_crouch_target ) || can_crouch_target == target ) {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] Native hook disabled: CanCrouchInCurrentState slot {} did not validate.\n" ),
				    *can_crouch_slot );
				return;
			}

			if ( MH_Initialize( ) != MH_OK ) {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] Native hook disabled: MinHook initialization " )
				        STR( "failed.\n" ) );
				return;
			}
			minhook_initialized_ = true;
			if ( MH_CreateHook( target, reinterpret_cast< void* >( &calc_velocity_detour ), reinterpret_cast< void** >( &g_original_calc_velocity ) ) != MH_OK ||
			     MH_CreateHook( can_crouch_target, reinterpret_cast< void* >( &can_crouch_detour ), reinterpret_cast< void** >( &g_original_can_crouch ) ) != MH_OK ||
			     MH_EnableHook( target ) != MH_OK ||
			     MH_EnableHook( can_crouch_target ) != MH_OK ) {
				RC::Output::send< RC::LogLevel::Error >(
				    STR( "[bhop] Native hook disabled: movement detours could " )
				        STR( "not be installed.\n" ) );
				return;
			}

			hook_target_       = target;
			can_crouch_target_ = can_crouch_target;
			hook_ready_        = true;
			RC::Output::send< RC::LogLevel::Normal >(
			    STR( "[bhop] Native movement hooks active (CalcVelocity slot {}, CanCrouch slot {}).\n" ),
			    *slot,
			    *can_crouch_slot );
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
			if ( auto* value = property_value< float >(
			         movement,
			         movement_properties_.max_swim_speed ) ) {
				*value = state.max_swim_speed;
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
			        { },
			        nullptr ) );
			jump_hook_ids_.push_back(
			    RC::Unreal::UObjectGlobals::RegisterHook(
			        STR( "/Script/Engine.Character:StopJumping" ),
			        release,
			        { },
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

		auto register_mouse_hooks( ) -> void {
			const auto callback = [ this ]( RC::Unreal::UnrealScriptFunctionCallableContext& context, void* ) { correct_mouse_input( context ); };
			const auto no_op    = []( RC::Unreal::UnrealScriptFunctionCallableContext&, void* ) {};
			mouse_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_Turn_K2Node_InputAxisEvent_157" ), callback, no_op, nullptr ) );
			mouse_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook( STR( "/Game/Game/BPCharacter_Demo.BPCharacter_Demo_C:InpAxisEvt_LookUp_K2Node_InputAxisEvent_172" ), callback, no_op, nullptr ) );
		}

		auto register_movement_input_hooks( ) -> void {
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

		auto register_ladder_hook( UObject* ladder ) -> void {
			ladder_overlap_function_ = ladder->GetFunctionByNameInChain( STR( "BndEvt__BP_Ladder_Box1_K2Node_ComponentBoundEvent_2_ComponentBeginOverlapSignature__DelegateSignature" ) );
			if ( !ladder_overlap_function_ ) {
				return;
			}
			const auto callback = [ this ]( RC::Unreal::UnrealScriptFunctionCallableContext& context, void* ) {
				auto& parameters = context.GetParams< ladder_overlap_params_t >( );
				const bool already_attached = parameters.other_actor && ladder_states_.contains( parameters.other_actor );
				if ( already_attached || activate_ladder( context.Context, parameters.other_actor ) ) {
					// Make the Blueprint cast fail so ETB never starts its
					// snap-to-ladder timeline or scripted transition.
					parameters.other_actor = nullptr;
				}
			};
			const auto no_op = []( RC::Unreal::UnrealScriptFunctionCallableContext&, void* ) {};
			ladder_hook_ids_.push_back( RC::Unreal::UObjectGlobals::RegisterHook( ladder_overlap_function_, callback, no_op, nullptr ) );
			Output::send< LogLevel::Default >( STR( "[bhop] GoldSrc ladder hook active.\n" ) );
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
			RC::Unreal::Hook::FCallbackOptions options{ };
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
		bhop::config_t                                   config_{ };
		std::filesystem::path                            config_path_{ };
		movement_properties_t                            movement_properties_{ };
		character_properties_t                           character_properties_{ };
		UClass*                                          character_class_{ };
		UFunction*                                       set_movement_mode_function_{ };
		FProperty*                                       set_movement_mode_property_{ };
		FProperty*                                       mouse_delta_seconds_property_{ };
		UFunction*                                       ladder_start_function_{ };
		FProperty*                                       ladder_start_return_property_{ };
		UFunction*                                       ladder_end_function_{ };
		FProperty*                                       ladder_end_return_property_{ };
		UFunction*                                       box_extent_function_{ };
		FProperty*                                       box_extent_return_property_{ };
		UFunction*                                       get_physics_volume_function_{ };
		FProperty*                                       get_physics_volume_return_property_{ };
		FBoolProperty*                                   water_volume_property_{ };
		UObject*                                         input_settings_{ };
		FBoolProperty*                                   mouse_smoothing_property_{ };
		UFunction*                                       clear_mouse_smoothing_function_{ };
		UFunction*                                       ladder_overlap_function_{ };
		std::unordered_map< UObject*, movement_state_t > states_{ };
		std::unordered_map< UObject*, ladder_state_t >   ladder_states_{ };
		std::unordered_map< UObject*, bool >             water_states_{ };
		std::unordered_map< UObject*, bool >             jump_held_{ };
		std::unordered_map< UObject*, bool >             crouch_held_{ };
		std::unordered_map< UObject*, bool >             crouch_release_pending_{ };
		std::unordered_map< UObject*, float >            forward_input_{ };
		std::unordered_map< UObject*, float >            side_input_{ };
		std::optional< int >                             crouch_press_event_{ };
		std::vector< std::pair< int, int > >             jump_hook_ids_{ };
		std::vector< std::pair< int, int > >             crouch_hook_ids_{ };
		std::vector< std::pair< int, int > >             mouse_hook_ids_{ };
		std::vector< std::pair< int, int > >             movement_input_hook_ids_{ };
		std::vector< std::pair< int, int > >             ladder_hook_ids_{ };
		RC::Unreal::Hook::GlobalCallbackId               install_tick_id_{ };
		RC::Unreal::Hook::GlobalCallbackId               console_hook_id_{ };
		void*                                            hook_target_{ };
		void*                                            can_crouch_target_{ };
		bool                                             install_attempted_{ };
		bool                                             hook_ready_{ };
		bool                                             minhook_initialized_{ };
		bool                                             original_mouse_smoothing_{ };
		bool                                             has_original_mouse_smoothing_{ };
		bool                                             water_logged_{ };
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

	bool can_crouch_detour( UObject* movement ) {
		if ( g_mod && g_mod->should_allow_goldsrc_crouch( movement ) ) {
			return true;
		}
		return g_original_can_crouch && g_original_can_crouch( movement );
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
