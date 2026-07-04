#pragma once
/*
	core/memory.h  --  typed calls & memory helpers

	The modern replacement for Project Fuego's inline-asm `mov ecx,this; call 0xADDR`
	blocks and raw `*(T*)addr` casts. Everything is a typed, compiler-understood
	call (mirrors gta-reversed / plugin-sdk conventions) so it stays correct across
	toolsets. x86 only (the game is 32-bit).
*/

#include <cstdint>
#include <windows.h>

namespace mem {

// ---- typed absolute-address calls (replace inline-asm `call 0xADDR`) --------

// __thiscall: `this` in ECX, args after.
template <typename Ret, typename... Args>
inline Ret thiscall( uintptr_t addr, void *self, Args... args ) {
	return reinterpret_cast<Ret( __thiscall * )( void *, Args... )>( addr )( self, args... );
}

// __cdecl free function.
template <typename Ret, typename... Args>
inline Ret ccall( uintptr_t addr, Args... args ) {
	return reinterpret_cast<Ret( __cdecl * )( Args... )>( addr )( args... );
}

// __stdcall free function.
template <typename Ret, typename... Args>
inline Ret stdcall( uintptr_t addr, Args... args ) {
	return reinterpret_cast<Ret( __stdcall * )( Args... )>( addr )( args... );
}

// ---- typed memory access (replace raw `*(T*)addr`) --------------------------

template <typename T>
inline T &ref( uintptr_t addr ) { return *reinterpret_cast<T *>( addr ); }

template <typename T>
inline T read( uintptr_t addr ) { return *reinterpret_cast<T *>( addr ); }

template <typename T>
inline void write( uintptr_t addr, T value ) { *reinterpret_cast<T *>( addr ) = value; }

template <typename T = void>
inline T *ptr( uintptr_t addr ) { return reinterpret_cast<T *>( addr ); }

// ---- byte patching (VirtualProtect-guarded) ---------------------------------

inline void patch( uintptr_t addr, const void *bytes, size_t len ) {
	DWORD old;
	VirtualProtect( reinterpret_cast<void *>( addr ), len, PAGE_EXECUTE_READWRITE, &old );
	memcpy( reinterpret_cast<void *>( addr ), bytes, len );
	VirtualProtect( reinterpret_cast<void *>( addr ), len, old, &old );
}

inline void nop( uintptr_t addr, size_t len ) {
	DWORD old;
	VirtualProtect( reinterpret_cast<void *>( addr ), len, PAGE_EXECUTE_READWRITE, &old );
	memset( reinterpret_cast<void *>( addr ), 0x90, len );
	VirtualProtect( reinterpret_cast<void *>( addr ), len, old, &old );
}

} // namespace mem
