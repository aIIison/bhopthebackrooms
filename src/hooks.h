#pragma once

#include <Unreal/UObject.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace bhop::native {
	[[nodiscard]] auto validate_game_image( ) noexcept -> bool;
	[[nodiscard]] auto is_executable_game_address( const void* address ) noexcept -> bool;
	[[nodiscard]] auto resolve_virtual_slot( RC::Unreal::UFunction* function, void** vtable = nullptr, bool log_failure = true ) -> std::optional< std::size_t >;
	[[nodiscard]] auto resolve_can_crouch_slot( void** vtable, std::int64_t movement_mode_offset, std::int64_t updated_component_offset ) -> std::optional< std::size_t >;
	[[nodiscard]] auto resolve_physics_volume_changed_slot( void** vtable, std::int64_t water_volume_offset ) -> std::optional< std::size_t >;
}  // namespace bhop::native
