#define NOMINMAX
#include "process.h"

#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace bhop::native::util {
	namespace {
		constexpr std::uint32_t expected_time_date_stamp = 0x6A291D66;
		constexpr std::uint32_t expected_size_of_image   = 0x05233000;
	}  // namespace

	auto validate_game_image( ) noexcept -> bool {
		const auto module = reinterpret_cast< std::byte* >( GetModuleHandleW( nullptr ) );
		if ( !module ) {
			return false;
		}

		const auto* dos = reinterpret_cast< const IMAGE_DOS_HEADER* >( module );
		if ( dos->e_magic != IMAGE_DOS_SIGNATURE ) {
			return false;
		}

		const auto* nt = reinterpret_cast< const IMAGE_NT_HEADERS64* >( module + dos->e_lfanew );
		return nt->Signature == IMAGE_NT_SIGNATURE &&
		    nt->FileHeader.TimeDateStamp == expected_time_date_stamp &&
		    nt->OptionalHeader.SizeOfImage == expected_size_of_image;
	}

	auto is_executable_game_address( const void* address ) noexcept -> bool {
		MEMORY_BASIC_INFORMATION information{};
		if ( !address || VirtualQuery( address, &information, sizeof( information ) ) != sizeof( information ) ) {
			return false;
		}

		const DWORD protection = information.Protect & 0xff;
		const bool  executable =
		    protection == PAGE_EXECUTE ||
		    protection == PAGE_EXECUTE_READ ||
		    protection == PAGE_EXECUTE_READWRITE ||
		    protection == PAGE_EXECUTE_WRITECOPY;
		return executable &&
		    information.Type == MEM_IMAGE &&
		    information.AllocationBase == GetModuleHandleW( nullptr );
	}
}  // namespace bhop::native::util
