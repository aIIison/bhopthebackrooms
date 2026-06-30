#pragma once

#include <Zydis/Zydis.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace bhop::native::util {
	inline constexpr std::size_t vtable_slot_count = 384;

	enum class e_pointer_kind : std::uint8_t {
		unknown,
		context,
		vtable,
		volume,
	};

	using pointer_map_t = std::array< e_pointer_kind, ZYDIS_REGISTER_MAX_VALUE + 1 >;

	struct instruction_view_t {
		const ZydisDecodedInstruction& instruction;
		const ZydisDecodedOperand*     operands;
		const std::uint8_t*            address;

		[[nodiscard]] auto operand_count( ) const noexcept -> std::uint8_t;
	};

	using instruction_visitor_t = std::function< void( const instruction_view_t&, const pointer_map_t& ) >;
	using slot_predicate_t      = std::function< bool( const std::uint8_t* ) >;

	// Tracks only the pointer provenance needed by the supported hook
	// signatures. Unknown instructions erase written registers, so ambiguous
	// code fails validation instead of producing a speculative slot.
	[[nodiscard]] auto scan_function(
	    const std::uint8_t*          code,
	    std::size_t                  byte_limit,
	    pointer_map_t                pointers,
	    const instruction_visitor_t& visitor ) -> bool;

	[[nodiscard]] auto reads_field(
	    const instruction_view_t& view,
	    const pointer_map_t&      pointers,
	    e_pointer_kind            owner,
	    std::int64_t              offset ) noexcept -> bool;

	[[nodiscard]] auto uses_immediate(
	    const instruction_view_t& view,
	    std::uint64_t             value,
	    ZydisMnemonic             mnemonic = ZYDIS_MNEMONIC_INVALID ) noexcept -> bool;

	[[nodiscard]] auto virtual_call_slot(
	    const instruction_view_t& view,
	    const pointer_map_t&      pointers ) noexcept -> std::optional< std::size_t >;

	[[nodiscard]] auto direct_call_target( const instruction_view_t& view ) noexcept -> void*;
	[[nodiscard]] auto canonical_function_address( void* address ) noexcept -> void*;

	[[nodiscard]] auto find_vtable_matches(
	    void**                  vtable,
	    bool                    canonicalize,
	    bool                    skip_duplicate_targets,
	    const slot_predicate_t& predicate ) -> std::vector< std::size_t >;
}  // namespace bhop::native::util
