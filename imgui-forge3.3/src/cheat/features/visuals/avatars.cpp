#include <stdafx.hpp>

#include <features/visuals/avatars.hpp>

#include "imgui_internal.h"

#include <system/runtime_events.hpp>

#include <Windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace features::visuals {
	namespace {

		struct image
		{
			std::vector<std::uint8_t> rgba{};
			int width{};
			int height{};
		};

		// One HTTPS GET into a byte buffer. The avatar call is small and rare, so a fresh
		// connection per request is fine and keeps the code free of session state.
		bool http_get( const std::wstring& host, const std::wstring& path,
			std::vector<std::uint8_t>& out )
		{
			const HINTERNET session = ::WinHttpOpen( L"neverlose-menu/1.0",
				WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				WINHTTP_NO_PROXY_BYPASS, 0 );
			if ( !session )
				return false;

			bool ok = false;
			HINTERNET connection{};
			HINTERNET request{};
			do
			{
				connection = ::WinHttpConnect( session, host.c_str(),
					INTERNET_DEFAULT_HTTPS_PORT, 0 );
				if ( !connection )
					break;
				request = ::WinHttpOpenRequest( connection, L"GET", path.c_str(), nullptr,
					WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE );
				if ( !request )
					break;
				const wchar_t* headers = L"Accept: */*\r\n";
				if ( !::WinHttpSendRequest( request, headers, -1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) )
					break;
				if ( !::WinHttpReceiveResponse( request, nullptr ) )
					break;

				DWORD status = 0;
				DWORD status_size = sizeof( status );
				if ( !::WinHttpQueryHeaders( request,
						WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
						WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
						WINHTTP_NO_HEADER_INDEX ) || status != 200 )
					break;

				for ( ;; )
				{
					DWORD available = 0;
					if ( !::WinHttpQueryDataAvailable( request, &available ) || available == 0 )
						break;
					const auto offset = out.size( );
					out.resize( offset + available );
					DWORD read = 0;
					if ( !::WinHttpReadData( request, out.data( ) + offset, available, &read ) )
					{
						out.resize( offset );
						break;
					}
					out.resize( offset + read );
					if ( out.size( ) > 4u * 1024u * 1024u )
						break;
				}
				ok = !out.empty( );
			} while ( false );

			if ( request )
				::WinHttpCloseHandle( request );
			if ( connection )
				::WinHttpCloseHandle( connection );
			::WinHttpCloseHandle( session );
			return ok;
		}

		// The profile XML carries the picture URLs; the medium one is the size the ESP
		// draws and is served as a plain JPEG.
		std::string find_avatar_url( const std::string& xml )
		{
			constexpr const char* open_tag = "<avatarMedium>";
			constexpr const char* close_tag = "</avatarMedium>";
			const auto start = xml.find( open_tag );
			if ( start == std::string::npos )
				return {};
			auto begin = start + std::strlen( open_tag );
			const auto end = xml.find( close_tag, begin );
			if ( end == std::string::npos )
				return {};
			// The value is wrapped in a CDATA section.
			const auto cdata = xml.find( "<![CDATA[", begin );
			if ( cdata != std::string::npos && cdata < end )
				begin = cdata + 9;
			return xml.substr( begin, end - begin );
		}

		bool fetch_avatar( const std::uint64_t steamid, image& out )
		{
			const std::wstring profile = L"/profiles/" + std::to_wstring( steamid ) + L"/?xml=1";
			std::vector<std::uint8_t> xml_bytes{};
			if ( !http_get( L"steamcommunity.com", profile, xml_bytes ) )
				return false;
			const std::string xml{ xml_bytes.begin( ), xml_bytes.end( ) };
			const auto url = find_avatar_url( xml );
			if ( url.empty( ) )
				return false;

			// "https://avatars.steamstatic.com/<hash>_medium.jpg"
			const auto scheme = url.find( "://" );
			if ( scheme == std::string::npos )
				return false;
			const auto slash = url.find( '/', scheme + 3 );
			if ( slash == std::string::npos )
				return false;
			const std::string host = url.substr( scheme + 3, slash - scheme - 3 );
			const std::string path = url.substr( slash );

			std::vector<std::uint8_t> bytes{};
			if ( !http_get( std::wstring{ host.begin( ), host.end( ) },
					std::wstring{ path.begin( ), path.end( ) }, bytes ) )
				return false;

			int width = 0, height = 0, channels = 0;
			unsigned char* pixels = stbi_load_from_memory( bytes.data( ),
				static_cast<int>( bytes.size( ) ), &width, &height, &channels, 4 );
			if ( !pixels )
				return false;
			out.width = width;
			out.height = height;
			out.rgba.assign( pixels, pixels + static_cast<std::size_t>( width ) * height * 4 );
			stbi_image_free( pixels );
			return true;
		}

	} // namespace

	struct avatar_cache::impl
	{
		std::mutex mutex{};
		std::condition_variable wake{};
		std::deque<std::uint64_t> queue{};
		std::unordered_map<std::uint64_t, image> ready{};
		std::unordered_map<std::uint64_t, bool> known{};   // fetched or definitively missing
		std::unordered_map<std::uint64_t, ImTextureData*> textures{};
		std::thread worker{};
		std::atomic<bool> running{ false };
	};

	void avatar_cache::start( )
	{
		if ( m_impl )
			return;
		m_impl = new impl{};
		m_impl->running.store( true, std::memory_order_release );
		m_impl->worker = std::thread( [ this ]
		{
			for ( ;; )
			{
				std::uint64_t steamid = 0;
				{
					std::unique_lock lock{ m_impl->mutex };
					m_impl->wake.wait( lock, [ this ]
					{
						return !m_impl->running.load( std::memory_order_acquire )
							|| !m_impl->queue.empty( );
					} );
					if ( !m_impl->running.load( std::memory_order_acquire )
						&& m_impl->queue.empty( ) )
						return;
					steamid = m_impl->queue.front( );
					m_impl->queue.pop_front( );
				}

				image picture{};
				const bool fetched = steamid != 0 && fetch_avatar( steamid, picture );
				std::scoped_lock lock{ m_impl->mutex };
				m_impl->known[ steamid ] = true;
				if ( fetched )
					m_impl->ready[ steamid ] = std::move( picture );
			}
		} );
	}

	void avatar_cache::stop( )
	{
		if ( !m_impl )
			return;
		m_impl->running.store( false, std::memory_order_release );
		m_impl->wake.notify_all( );
		if ( m_impl->worker.joinable( ) )
			m_impl->worker.join( );
		for ( auto& [ id, texture ] : m_impl->textures )
		{
			static_cast<void>( id );
			if ( texture )
				IM_DELETE( texture );
		}
		delete m_impl;
		m_impl = nullptr;
	}

	ImTextureID avatar_cache::texture( const std::uint64_t steamid )
	{
		if ( !m_impl || steamid == 0 )
			return 0;

		{
			std::scoped_lock lock{ m_impl->mutex };
			if ( const auto found = m_impl->textures.find( steamid );
				found != m_impl->textures.end( ) )
				return found->second ? found->second->GetTexID( ) : 0;

			// A picture that arrived since the last frame becomes a texture here, on the
			// render thread, because registering it is an ImGui call.
			if ( const auto ready = m_impl->ready.find( steamid ); ready != m_impl->ready.end( ) )
			{
				auto* texture = IM_NEW( ImTextureData )( );
				texture->Create( ImTextureFormat_RGBA32, ready->second.width, ready->second.height );
				texture->UseColors = true;
				std::memcpy( texture->GetPixelsAt( 0, 0 ), ready->second.rgba.data( ),
					ready->second.rgba.size( ) );
				ImGui::RegisterUserTexture( texture );
				m_impl->ready.erase( ready );
				m_impl->textures[ steamid ] = texture;
				platform::windows::runtime_event( "avatar",
					std::format( "id={} size={}x{}", steamid, texture->Width, texture->Height ) );
				return texture->GetTexID( );
			}

			// Nothing fetched yet: queue it once.
			if ( !m_impl->known.contains( steamid ) )
			{
				const bool queued = std::find( m_impl->queue.begin( ), m_impl->queue.end( ), steamid )
					!= m_impl->queue.end( );
				if ( !queued && m_impl->queue.size( ) < 32 )
				{
					m_impl->queue.push_back( steamid );
					m_impl->wake.notify_one( );
				}
			}
		}
		return 0;
	}

	float avatar_cache::draw_size( const float text_height )
	{
		// Slightly taller than the text cap so the round picture reads as an avatar and
		// not as a bullet point.
		return text_height * 1.35f;
	}

} // namespace features::visuals
