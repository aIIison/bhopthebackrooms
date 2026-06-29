#include "hooks.h"

#include "mod.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Windows.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <array>
#include <vector>

namespace bhop::native {
	namespace {
		constexpr std::uint32_t expected_time_date_stamp = 0x6A291D66;
		constexpr std::uint32_t expected_size_of_image   = 0x05233000;

		enum class e_pointer_kind : std::uint8_t {
			unknown,
			context_object,
			context_vtable,
			volume_object,
		};
	}  // namespace

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

	[[nodiscard]] auto follow_jump_thunks( void* address ) noexcept -> void* {
		auto* current = static_cast< std::uint8_t* >( address );
		for ( int depth = 0; depth < 4 && is_executable_game_address( current ); ++depth ) {
			ZydisDecoder decoder{};
			if ( !ZYAN_SUCCESS( ZydisDecoderInit( &decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64 ) ) ) {
				break;
			}
			ZydisDecodedInstruction instruction{};
			ZydisDecodedOperand     operands[ ZYDIS_MAX_OPERAND_COUNT ]{};
			if ( !ZYAN_SUCCESS( ZydisDecoderDecodeFull( &decoder, current, 16, &instruction, operands ) ) ||
			     instruction.mnemonic != ZYDIS_MNEMONIC_JMP ||
			     operands[ 0 ].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
			     !operands[ 0 ].imm.is_relative ) {
				break;
			}
			ZyanU64 target{};
			if ( !ZYAN_SUCCESS( ZydisCalcAbsoluteAddress(
			         &instruction,
			         &operands[ 0 ],
			         reinterpret_cast< ZyanU64 >( current ),
			         &target ) ) ) {
				break;
			}
			current = reinterpret_cast< std::uint8_t* >( target );
		}
		return current;
	}

	// UE's generated execCalcVelocity wrapper eventually performs
	// P_THIS->CalcVelocity(...). Track the context pointer through simple MOVs
	// and accept a slot only when the wrapper contains exactly one indirect call
	// through that context's vtable.
	[[nodiscard]] auto resolve_virtual_slot( UFunction* function, void** vtable, bool log_failure )
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
		std::vector< void* >       direct_targets;
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
				} else if ( operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative ) {
					ZyanU64 absolute_address{};
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
					if ( follow_jump_thunks( vtable[ slot ] ) == follow_jump_thunks( target ) ) {
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

		ZydisDecoder decoder{};
		if ( !ZYAN_SUCCESS( ZydisDecoderInit( &decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64 ) ) ) {
			return std::nullopt;
		}

		std::vector< std::size_t > falling_predicates;
		for ( std::size_t slot = 0; slot < 384; ++slot ) {
			auto* code = static_cast< const std::uint8_t* >( vtable[ slot ] );
			if ( !is_executable_game_address( code ) ) {
				continue;
			}

			std::array< e_pointer_kind, ZYDIS_REGISTER_MAX_VALUE + 1 > kinds{};
			kinds[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context_object;
			bool        reads_movement_mode{};
			bool        compares_falling{};
			bool        reached_return{};
			std::size_t offset{};
			while ( offset < 64 ) {
				ZydisDecodedInstruction instruction{};
				ZydisDecodedOperand     operands[ ZYDIS_MAX_OPERAND_COUNT ]{};
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
		std::vector< const void* > visited;
		for ( std::size_t slot = 0; slot < 384; ++slot ) {
			auto* code = static_cast< const std::uint8_t* >( vtable[ slot ] );
			if ( !is_executable_game_address( code ) || std::find( visited.begin( ), visited.end( ), vtable[ slot ] ) != visited.end( ) ) {
				continue;
			}
			visited.push_back( vtable[ slot ] );

			std::array< e_pointer_kind, ZYDIS_REGISTER_MAX_VALUE + 1 > kinds{};
			kinds[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context_object;
			bool        calls_falling{};
			bool        calls_ground{};
			bool        reads_updated_component{};
			bool        reached_return{};
			std::size_t offset{};
			while ( offset < 192 ) {
				ZydisDecodedInstruction instruction{};
				ZydisDecodedOperand     operands[ ZYDIS_MAX_OPERAND_COUNT ]{};
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
						ZyanU64 target{};
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

	[[nodiscard]] auto resolve_physics_volume_changed_slot( void** vtable, std::int64_t water_volume_offset ) -> std::optional< std::size_t > {
		if ( !vtable || water_volume_offset < 0 ) {
			return std::nullopt;
		}

		ZydisDecoder decoder{};
		if ( !ZYAN_SUCCESS( ZydisDecoderInit( &decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64 ) ) ) {
			return std::nullopt;
		}

		std::vector< std::size_t > candidates;
		std::vector< const void* > visited;
		for ( std::size_t slot = 0; slot < 384; ++slot ) {
			auto* code = static_cast< const std::uint8_t* >( follow_jump_thunks( vtable[ slot ] ) );
			if ( !is_executable_game_address( code ) || std::find( visited.begin( ), visited.end( ), code ) != visited.end( ) ) {
				continue;
			}
			visited.push_back( code );

			std::array< e_pointer_kind, ZYDIS_REGISTER_MAX_VALUE + 1 > kinds{};
			kinds[ ZYDIS_REGISTER_RCX ] = e_pointer_kind::context_object;
			kinds[ ZYDIS_REGISTER_RDX ] = e_pointer_kind::volume_object;
			bool        reads_water_volume{};
			bool        uses_swimming_mode{};
			bool        reached_return{};
			std::size_t offset{};
			while ( offset < 768 ) {
				ZydisDecodedInstruction instruction{};
				ZydisDecodedOperand     operands[ ZYDIS_MAX_OPERAND_COUNT ]{};
				if ( !ZYAN_SUCCESS( ZydisDecoderDecodeFull( &decoder, code + offset, 768 - offset, &instruction, operands ) ) ) {
					break;
				}

				for ( std::uint8_t index = 0; index < instruction.operand_count_visible; ++index ) {
					const auto& operand = operands[ index ];
					if ( operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
					     operand.mem.base <= ZYDIS_REGISTER_MAX_VALUE &&
					     kinds[ operand.mem.base ] == e_pointer_kind::volume_object &&
					     operand.mem.disp.has_displacement &&
					     operand.mem.disp.value == water_volume_offset ) {
						reads_water_volume = true;
					}
					if ( operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE ) {
						uses_swimming_mode |= operand.imm.value.u == movement_swimming;
					}
				}

				if ( instruction.mnemonic == ZYDIS_MNEMONIC_CALL ) {
					clear_volatile_pointer_kinds( kinds );
				} else if ( instruction.mnemonic == ZYDIS_MNEMONIC_MOV && operands[ 0 ].type == ZYDIS_OPERAND_TYPE_REGISTER ) {
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
			if ( reached_return && reads_water_volume && uses_swimming_mode ) {
				candidates.push_back( slot );
			}
		}

		if ( candidates.size( ) != 1 ) {
			RC::Output::send< RC::LogLevel::Error >(
			    STR( "[bhop] Refused PhysicsVolumeChanged hook: semantic scan yielded {} candidates.\n" ),
			    candidates.size( ) );
			return std::nullopt;
		}
		return candidates.front( );
	}
}  // namespace bhop::native
