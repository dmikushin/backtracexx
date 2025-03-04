#include "backtracexx.hpp"
#include <iomanip>
#include <iostream>
#include <cstdlib>
#include <cstring>

//
//	TODO:
//	- use libdwarf for printing line info for ELF objects.
//

#if defined( __GNUC__ )
#include <cxxabi.h>
#if defined( __linux__ ) || defined( __APPLE__ )
#include <dlfcn.h>
#endif
#include <unwind.h>
#elif defined( WIN32 ) || defined( WIN64 )
#include <windows.h>
#include <winnt.h>
#else
#error "not testet yet."
#endif
//
//	please use a recent dbghelp.dll because older versions
//	have unexpected problems with symbols resolving, e.g.
//	::SymGetSymFromAddr() produces ERROR_INVALID_ADDRESS.
//
//	this code works fine with:
//	- dbghelp.dll v5.1.2600.2180 from WinXP/SP2.
//	- dbghelp.dll v6.5.0003.7 from Visual C++ 2005 Express Edition.
//
//	this code doesn't work with:
//	- dbghelp.dll v5.00.2195.6613 from Win2000/SP4.
//

#if defined( _MSC_VER )
#include <dbghelp.h>
#pragma comment( lib, "dbghelp" )
#endif

namespace backtracexx
{
#if defined( __GNUC__ )

	bool lookupSymbol( Frame& frame )
	{
#if defined( __linux__ ) || defined( __APPLE__ )

		Dl_info info;
		if ( ::dladdr( frame.address, &info ) )
		{
			frame.moduleBaseAddress = info.dli_fbase;
			if ( info.dli_fname && std::strlen( info.dli_fname ) )
				frame.moduleName = info.dli_fname;
			if ( info.dli_saddr )
			{
				frame.symbolMangled = info.dli_sname;
				frame.displacement = reinterpret_cast< ::ptrdiff_t >( frame.address )
					- reinterpret_cast< ::ptrdiff_t >( info.dli_saddr );
				int status;
				char* demangled = abi::__cxa_demangle( info.dli_sname, 0, 0, &status );
				if ( status != -1 )
				{
					if ( status == 0 )
					{
						frame.symbol = demangled;
						std::free( demangled );
					}
					else
						frame.symbol = info.dli_sname;
				}
			}
			return true;
		}

#endif
		return false;
	}

	namespace
	{
		struct TraceHelper
		{
			_Unwind_Ptr prevIp = -1;
			unsigned recursionDepth;
			Trace trace;
		};

		_Unwind_Reason_Code helper( struct _Unwind_Context* ctx, TraceHelper* th )
		{
			_Unwind_Ptr ip = _Unwind_GetIP( ctx );
			Frame frame( reinterpret_cast< void const* >( ip ) );
			lookupSymbol( frame );
			th->trace.push_back( frame );
			//
			// temporary workaround for glibc bug:
			// http://sources.redhat.com/bugzilla/show_bug.cgi?id=6693
			//
			unsigned const RecursionLimit = 8;
			if ( th->prevIp == ip )
			{
				if ( ++th->recursionDepth > RecursionLimit )
					return _URC_END_OF_STACK;
			}
			else
			{
				th->prevIp = ip;
				th->recursionDepth = 0;
			}
			return _URC_NO_REASON;
		}
	}

#elif defined( _MSC_VER )

	bool lookupSymbol( Frame& frame )
	{
		::MEMORY_BASIC_INFORMATION mbi;
		if ( !::VirtualQuery( reinterpret_cast< ::LPCVOID >( frame.address ), &mbi, sizeof( mbi ) ) )
			return false;
		::CHAR moduleName[ MAX_PATH ];
		::GetModuleFileNameA( reinterpret_cast< ::HMODULE >( mbi.AllocationBase ), moduleName, sizeof( moduleName ) );
		if ( mbi.Protect & PAGE_NOACCESS )
			return false;
		frame.moduleBaseAddress = mbi.AllocationBase;
		frame.moduleName = moduleName;
		int const MaxSymbolNameLength = 8192;
		::BYTE symbolBuffer[ sizeof( ::IMAGEHLP_SYMBOL64 ) + MaxSymbolNameLength ];
		::PIMAGEHLP_SYMBOL64 symbol = reinterpret_cast< ::PIMAGEHLP_SYMBOL64 >( symbolBuffer );
		symbol->SizeOfStruct = sizeof( symbolBuffer );
		symbol->MaxNameLength = MaxSymbolNameLength - 1;
		if ( ::SymLoadModule64( ::GetCurrentProcess(), 0, moduleName, 0,
			reinterpret_cast< ::DWORD64 >( mbi.AllocationBase ), 0 ) )
		{
			::DWORD64 displacement;
			if ( ::SymGetSymFromAddr64( ::GetCurrentProcess(), reinterpret_cast< ::DWORD64 >( frame.address ),
				&displacement, symbol ) )
			{
				frame.symbol = symbol->Name;
				frame.displacement = static_cast< unsigned long >( displacement );
				::IMAGEHLP_LINE64 line;
				line.SizeOfStruct = sizeof( ::IMAGEHLP_LINE64 );
				::DWORD lineDisplacement;
				if ( ::SymGetLineFromAddr64( ::GetCurrentProcess(), reinterpret_cast< ::DWORD64 >( frame.address ),
					&lineDisplacement, &line ) )
				{
					frame.fileName = line.FileName;
					frame.lineNumber = line.LineNumber;
				}
			}
			::SymUnloadModule64( ::GetCurrentProcess(), reinterpret_cast< ::DWORD64 >( mbi.AllocationBase ) );
		}
		return true;
	}

#endif

	Frame::Frame( void const* address )
	:
		address( address ), displacement(), lineNumber()
	{
	}

	Trace scan( ::PCONTEXT ctx )
	{
#if defined( __GNUC__ )

		TraceHelper th;
		//
		//	libgcc takes care about proper stack walking.
		//
		_Unwind_Backtrace( reinterpret_cast< _Unwind_Trace_Fn >( helper ), &th );
		return th.trace;

#elif defined( _MSC_VER )

		Trace trace;

		::HANDLE process = ::GetCurrentProcess();
		::SymInitialize( process, 0, FALSE );
		::SymSetOptions( ::SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES );
		::CONTEXT context;
		::memset( &context, 0, sizeof( context ) );
		::STACKFRAME64 stackFrame;
		::memset( &stackFrame, 0, sizeof( stackFrame ) );
		stackFrame.AddrPC.Mode = stackFrame.AddrFrame.Mode = stackFrame.AddrStack.Mode = AddrModeFlat;
		if ( ctx )
		{
			context = *ctx;
#if defined( WIN64 )
			Frame frame( reinterpret_cast< void const* >( context.Rip ) );
#else
			Frame frame( reinterpret_cast< void const* >( static_cast< ::DWORD64 >( context.Eip ) ) );
#endif
			lookupSymbol( frame );
			trace.push_back( frame );
		}
		else
		{
#if defined( WIN32 )
			__asm
			{
				call $ + 5;
				pop eax;
				mov context.Eip, eax;
				mov context.Esp, esp;
				mov context.Ebp, ebp;
			}
#else
#error "msvc/win64 needs external assembly."
#endif
		}

#if defined( WIN64 )
		stackFrame.AddrPC.Offset = context.Rip;
		stackFrame.AddrStack.Offset = context.Rsp;
		stackFrame.AddrFrame.Offset = context.Rbp;

		while ( ::StackWalk64( IMAGE_FILE_MACHINE_AMD64,
#else
		stackFrame.AddrPC.Offset = context.Eip;
		stackFrame.AddrStack.Offset = context.Esp;
		stackFrame.AddrFrame.Offset = context.Ebp;

		while ( ::StackWalk64( IMAGE_FILE_MACHINE_I386,
#endif
			process, ::GetCurrentThread(), &stackFrame, &context, 0,
			::SymFunctionTableAccess64, ::SymGetModuleBase64, 0 ) )
		{
			void const* offset = reinterpret_cast< void const* >( stackFrame.AddrReturn.Offset );
			//
			//	the deepest frame pointer and return address of the process
			//	call chain are zeroed by kernel32.dll during process startup,
			//	so exclude such frame from trace and exit from loop.
			//
			if ( !offset )
				break;
			Frame frame( offset );
			if ( lookupSymbol( frame ) )
				trace.push_back( frame );
		}
		::SymCleanup( process );
		return trace;

#endif
	}

	std::ostream& operator << ( std::ostream& os, Trace const& t )
	{
		os << "=== backtrace ====" << std::endl;
		for ( backtracexx::Trace::const_iterator i = t.begin(); i != t.end(); ++i )
		{
			backtracexx::Frame const& f = *i;
			os << std::showbase << std::hex << std::setw( 16 ) << f.address << " : ";
			if ( f.symbol.empty() )
				os << '?';
			else
				os << f.symbol << '+' << f.displacement;
			os << " [" << f.moduleName << " @ " << std::showbase << std::hex << f.moduleBaseAddress << " ]" << std::endl;
			if ( !f.fileName.empty() )
			{
				static std::string filler( 14, ' ' );
				os << filler << "at : " << f.fileName << ':' << std::dec << f.lineNumber << std::endl;
			}
		}
		os << "==================" << std::endl;
		return os;
	}
}
