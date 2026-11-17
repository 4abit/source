#include <stdafx.hpp>

#include <core/hooking/trampoline.hpp>

#include <zydis.h>

#include <cstring>

namespace hooking {
namespace {

	constexpr std::size_t k_patch = 14;
	constexpr std::size_t k_block = 64;

	void write_abs_jump( std::uint8_t* at, const void* destination )
	{
		at[ 0 ] = 0xFF;
		at[ 1 ] = 0x25;
		std::memset( at + 2, 0, 4 );
		const auto address = reinterpret_cast<std::uint64_t>( destination );
		std::memcpy( at + 6, &address, sizeof( address ) );
	}

	// Copied rip-relative instructions keep a rel32, so the block has to live
	// within ±2 GiB of the function.
	void* allocate_near( const void* target )
	{
		SYSTEM_INFO info{};
		::GetSystemInfo( &info );
		const auto granularity = static_cast<std::uintptr_t>( info.dwAllocationGranularity );
		const auto origin = reinterpret_cast<std::uintptr_t>( target ) & ~( granularity - 1 );
		constexpr std::uintptr_t reach = 0x7FF00000ull;
		for ( std::uintptr_t offset = granularity; offset < reach; offset += granularity )
		{
			for ( const auto candidate : { origin - offset, origin + offset } )
			{
				if ( candidate < 0x10000 )
					continue;
				if ( auto* block = ::VirtualAlloc( reinterpret_cast<void*>( candidate ), k_block,
					MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) )
					return block;
			}
		}
		return nullptr;
	}

	bool relocate( std::uint8_t* source, std::uint8_t* destination, std::size_t& consumed )
	{
		ZydisDecoder decoder{};
		ZydisDecoderInit( &decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64 );
		std::size_t read = 0;
		std::size_t written = 0;
		while ( read < k_patch )
		{
			ZydisDecodedInstruction instruction{};
			if ( !ZYAN_SUCCESS( ZydisDecoderDecodeInstruction( &decoder, nullptr, source + read, 15, &instruction ) ) )
				return false;
			const auto length = instruction.length;
			auto* from = source + read;
			auto* to = destination + written;
			if ( instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE )
			{
				if ( instruction.raw.disp.size == 32 || instruction.raw.imm[ 0 ].size == 32 )
				{
					const auto use_disp = instruction.raw.disp.size == 32;
					const auto field = use_disp ? instruction.raw.disp.offset : instruction.raw.imm[ 0 ].offset;
					const auto value = use_disp ? static_cast<std::int64_t>( instruction.raw.disp.value )
						: instruction.raw.imm[ 0 ].value.s;
					std::memcpy( to, from, length );
					const auto absolute = reinterpret_cast<std::intptr_t>( from + length ) + value;
					const auto shifted = absolute - reinterpret_cast<std::intptr_t>( to + length );
					if ( shifted < INT32_MIN || shifted > INT32_MAX )
						return false;
					const auto rel = static_cast<std::int32_t>( shifted );
					std::memcpy( to + field, &rel, sizeof( rel ) );
					written += length;
				}
				else if ( instruction.meta.branch_type == ZYDIS_BRANCH_TYPE_SHORT )
				{
					const auto absolute = reinterpret_cast<std::intptr_t>( from + length )
						+ static_cast<std::int8_t>( instruction.raw.imm[ 0 ].value.s );
					if ( instruction.meta.category == ZYDIS_CATEGORY_COND_BR )
					{
						const auto rel = static_cast<std::int32_t>( absolute - reinterpret_cast<std::intptr_t>( to + 6 ) );
						to[ 0 ] = 0x0F;
						to[ 1 ] = static_cast<std::uint8_t>( 0x80 + ( instruction.opcode & 0x0F ) );
						std::memcpy( to + 2, &rel, sizeof( rel ) );
						written += 6;
					}
					else if ( instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR )
					{
						const auto rel = static_cast<std::int32_t>( absolute - reinterpret_cast<std::intptr_t>( to + 5 ) );
						to[ 0 ] = 0xE9;
						std::memcpy( to + 1, &rel, sizeof( rel ) );
						written += 5;
					}
					else
						return false;
				}
				else
					return false;
			}
			else
			{
				std::memcpy( to, from, length );
				written += length;
			}
			read += length;
			if ( written + k_patch > k_block )
				return false;
		}
		write_abs_jump( destination + written, source + read );
		consumed = read;
		return true;
	}

	std::uint8_t* follow_thunks( std::uint8_t* target )
	{
		for ( int depth = 0; depth < 8 && target; ++depth )
		{
			if ( target[ 0 ] == 0xE9 )
			{
				std::int32_t rel{};
				std::memcpy( &rel, target + 1, sizeof( rel ) );
				target = target + 5 + rel;
			}
			else if ( target[ 0 ] == 0xFF && target[ 1 ] == 0x25 )
			{
				std::int32_t rel{};
				std::memcpy( &rel, target + 2, sizeof( rel ) );
				std::uintptr_t next{};
				std::memcpy( &next, target + 6 + rel, sizeof( next ) );
				target = reinterpret_cast<std::uint8_t*>( next );
			}
			else
				break;
		}
		return target;
	}

} // namespace

bool trampoline::install( void* target, void* detour )
{
	if ( m_trampoline )
		return true;
	if ( !target || !detour )
		return false;
	MEMORY_BASIC_INFORMATION page{};
	if ( !::VirtualQuery( target, &page, sizeof( page ) ) || page.State != MEM_COMMIT )
		return false;
	auto* function = follow_thunks( static_cast<std::uint8_t*>( target ) );
	auto* block = static_cast<std::uint8_t*>( allocate_near( function ) );
	if ( !block )
		return false;
	std::size_t consumed = 0;
	if ( !relocate( function, block, consumed ) || consumed > sizeof( m_saved ) )
	{
		::VirtualFree( block, 0, MEM_RELEASE );
		return false;
	}

	std::memcpy( m_saved, function, consumed );
	m_saved_length = consumed;
	m_target = function;
	m_trampoline = block;

	std::uint8_t patch[ 32 ]{};
	std::memset( patch, 0x90, consumed );
	write_abs_jump( patch, detour );
	DWORD previous = 0;
	::VirtualProtect( function, consumed, PAGE_EXECUTE_READWRITE, &previous );
	std::memcpy( function, patch, consumed );
	::VirtualProtect( function, consumed, previous, &previous );
	::FlushInstructionCache( ::GetCurrentProcess(), function, consumed );
	return true;
}

void trampoline::remove()
{
	if ( !m_trampoline )
		return;
	DWORD previous = 0;
	::VirtualProtect( m_target, m_saved_length, PAGE_EXECUTE_READWRITE, &previous );
	std::memcpy( m_target, m_saved, m_saved_length );
	::VirtualProtect( m_target, m_saved_length, previous, &previous );
	::FlushInstructionCache( ::GetCurrentProcess(), m_target, m_saved_length );
	// The block is leaked on purpose: a thread may still be inside it.
	m_trampoline = nullptr;
}

} // namespace hooking
