#pragma once

namespace bhop::native::util {
	[[nodiscard]] auto validate_game_image( ) noexcept -> bool;
	[[nodiscard]] auto is_executable_game_address( const void* address ) noexcept -> bool;
}  // namespace bhop::native::util
