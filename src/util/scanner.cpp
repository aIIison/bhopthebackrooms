#include "scanner.h"

#include "process.h"

#include <algorithm>

namespace bhop::native::util {
	namespace {
		auto clear_volatile_pointers( pointer_map_t& pointers ) noexcept -> void {
			// Calls may destroy these registers under the Windows x64 ABI.
			constexpr ZydisRegister registers[]{
				ZYDIS_REGISTER_RAX,
				ZYDIS_REGISTER_RCX,
				ZYDIS_REGISTER_RDX,
				ZYDIS_REGISTER_R8,
				ZYDIS_REGISTER_R9,
				ZYDIS_REGISTER_R10,
				ZYDIS_REGISTER_R11,
			};
			for ( const auto reg : registers ) {
				pointers[ reg ] = e_pointer_kind::unknown;
			}
		}

		auto update_pointer_map( const instruction_view_t& view, pointer_map_t& pointers ) noexcept -> void {
			const auto& instruction = view.instruction;
			const auto* operands    = view.operands;

			if ( instruction.mnemonic == ZYDIS_MNEMONIC_CALL ) {
				clear_volatile_pointers( pointers );
				return;
			}

			if ( instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
			     view.operand_count( ) >= 2 &&
			     operands[ 0 ].type == ZYDIS_OPERAND_TYPE_REGISTER ) {
				const auto     destination = operands[ 0 ].reg.value;
				const auto&    source      = operands[ 1 ];
				e_pointer_kind kind        = e_pointer_kind::unknown;
				if ( source.type == ZYDIS_OPERAND_TYPE_REGISTER ) {
					kind = pointers[ source.reg.value ];
				} else if ( source.type == ZYDIS_OPERAND_TYPE_MEMORY &&
				            source.mem.base <= ZYDIS_REGISTER_MAX_VALUE &&
				            pointers[ source.mem.base ] == e_pointer_kind::context &&
				            source.mem.index == ZYDIS_REGISTER_NONE &&
				            ( !source.mem.disp.has_displacement || source.mem.disp.value == 0 ) ) {
					kind = e_pointer_kind::vtable;
				}
				pointers[ destination ] = kind;
				return;
			}

			for ( std::uint8_t index = 0; index < view.operand_count( ); ++index ) {
				const auto& operand = operands[ index ];
				if ( operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
				     operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE ) {
					pointers[ operand.reg.value ] = e_pointer_kind::unknown;
				}
			}
		}
	}  // namespace

	auto instruction_view_t::operand_count( ) const noexcept -> std::uint8_t {
		return instruction.operand_count_visible;
	}

	auto scan_function(
	    const std::uint8_t*          code,
	    std::size_t                  byte_limit,
	    pointer_map_t                pointers,
	    const instruction_visitor_t& visitor ) -> bool {
		ZydisDecoder decoder{};
		if ( !code ||
		     !ZYAN_SUCCESS( ZydisDecoderInit( &decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64 ) ) ) {
			return false;
		}

		std::size_t offset{};
		while ( offset < byte_limit ) {
			ZydisDecodedInstruction instruction{};
			ZydisDecodedOperand     operands[ ZYDIS_MAX_OPERAND_COUNT ]{};
			if ( !ZYAN_SUCCESS( ZydisDecoderDecodeFull(
			         &decoder,
			         code + offset,
			         byte_limit - offset,
			         &instruction,
			         operands ) ) ) {
				return false;
			}

			const instruction_view_t view{ instruction, operands, code + offset };
			visitor( view, pointers );
			update_pointer_map( view, pointers );
			offset += instruction.length;

			if ( instruction.mnemonic == ZYDIS_MNEMONIC_RET ) {
				return true;
			}
		}
		return false;
	}

	auto reads_field(
	    const instruction_view_t& view,
	    const pointer_map_t&      pointers,
	    e_pointer_kind            owner,
	    std::int64_t              offset ) noexcept -> bool {
		for ( std::uint8_t index = 0; index < view.operand_count( ); ++index ) {
			const auto& operand = view.operands[ index ];
			if ( operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
			     operand.mem.base <= ZYDIS_REGISTER_MAX_VALUE &&
			     pointers[ operand.mem.base ] == owner &&
			     operand.mem.disp.has_displacement &&
			     operand.mem.disp.value == offset ) {
				return true;
			}
		}
		return false;
	}

	auto uses_immediate(
	    const instruction_view_t& view,
	    std::uint64_t             value,
	    ZydisMnemonic             mnemonic ) noexcept -> bool {
		if ( mnemonic != ZYDIS_MNEMONIC_INVALID && view.instruction.mnemonic != mnemonic ) {
			return false;
		}
		for ( std::uint8_t index = 0; index < view.operand_count( ); ++index ) {
			const auto& operand = view.operands[ index ];
			if ( operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.value.u == value ) {
				return true;
			}
		}
		return false;
	}

	auto virtual_call_slot(
	    const instruction_view_t& view,
	    const pointer_map_t&      pointers ) noexcept -> std::optional< std::size_t > {
		if ( view.instruction.mnemonic != ZYDIS_MNEMONIC_CALL ) {
			return std::nullopt;
		}

		const auto& operand = view.operands[ 0 ];
		if ( operand.type != ZYDIS_OPERAND_TYPE_MEMORY ||
		     operand.mem.base > ZYDIS_REGISTER_MAX_VALUE ||
		     pointers[ operand.mem.base ] != e_pointer_kind::vtable ||
		     operand.mem.index != ZYDIS_REGISTER_NONE ||
		     !operand.mem.disp.has_displacement ||
		     operand.mem.disp.value < 0 ||
		     operand.mem.disp.value % sizeof( void* ) != 0 ) {
			return std::nullopt;
		}
		return static_cast< std::size_t >( operand.mem.disp.value ) / sizeof( void* );
	}

	auto direct_call_target( const instruction_view_t& view ) noexcept -> void* {
		if ( view.instruction.mnemonic != ZYDIS_MNEMONIC_CALL ) {
			return nullptr;
		}

		const auto& operand = view.operands[ 0 ];
		if ( operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative ) {
			return nullptr;
		}

		ZyanU64 target{};
		return ZYAN_SUCCESS( ZydisCalcAbsoluteAddress(
		           &view.instruction,
		           &operand,
		           reinterpret_cast< ZyanU64 >( view.address ),
		           &target ) )
		    ? reinterpret_cast< void* >( target )
		    : nullptr;
	}

	auto canonical_function_address( void* address ) noexcept -> void* {
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

	auto find_vtable_matches(
	    void**                  vtable,
	    bool                    canonicalize,
	    bool                    skip_duplicate_targets,
	    const slot_predicate_t& predicate ) -> std::vector< std::size_t > {
		std::vector< std::size_t > matches;
		std::vector< const void* > visited;
		for ( std::size_t slot = 0; slot < vtable_slot_count; ++slot ) {
			const void* target = canonicalize ? canonical_function_address( vtable[ slot ] ) : vtable[ slot ];
			if ( !is_executable_game_address( target ) ||
			     ( skip_duplicate_targets &&
			       std::find( visited.begin( ), visited.end( ), target ) != visited.end( ) ) ) {
				continue;
			}
			visited.push_back( target );
			if ( predicate( static_cast< const std::uint8_t* >( target ) ) ) {
				matches.push_back( slot );
			}
		}
		return matches;
	}
}  // namespace bhop::native::util
