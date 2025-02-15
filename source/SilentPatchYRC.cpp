﻿#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#define WINVER 0x0601
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <ShlObj.h>

#include "Utils/MemoryMgr.h"
#include "Utils/Trampoline.h"
#include "Utils/Patterns.h"

#include <chrono>
#include <format>
#include <fstream>

#if _DEBUG
#define DEBUG_DOCUMENTS_PATH	1
#else
#define DEBUG_DOCUMENTS_PATH	0
#endif

// Target game version
// 0 - day 1 (28.01)
// 1 - 1st patch (19.02)
#define TARGET_VERSION			1


//
// Usage: SetThreadName ((DWORD)-1, "MainThread");
//
#include <windows.h>
const DWORD MS_VC_EXCEPTION = 0x406D1388;
#pragma pack(push,8)
typedef struct tagTHREADNAME_INFO
{
    DWORD dwType; // Must be 0x1000.
    LPCSTR szName; // Pointer to name (in user addr space).
    DWORD dwThreadID; // Thread ID (-1=caller thread).
    DWORD dwFlags; // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)
void SetThreadName(DWORD dwThreadID, const char* threadName) {
    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = threadName;
    info.dwThreadID = dwThreadID;
    info.dwFlags = 0;
#pragma warning(push)
#pragma warning(disable: 6320 6322)
    __try{
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER){
    }
#pragma warning(pop)
}

static HANDLE WINAPI CreateThread_SetDesc(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter,
								DWORD dwCreationFlags, LPDWORD lpThreadId)
{
	DWORD threadId;
	HANDLE result = CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, &threadId);
	if ( result != nullptr )
	{
		const uintptr_t threadNameAddr = reinterpret_cast<uintptr_t>(lpParameter) + 4;
		const char* threadName = reinterpret_cast<const char*>(threadNameAddr);

		SetThreadName(threadId, threadName);
	}
	if ( lpThreadId != nullptr ) *lpThreadId = threadId;

	return result;
}

namespace MessagePumpFixes
{
	BOOL WINAPI PeekMessageA_WaitForMessages( LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg )
	{
		// This function is only ever called from a single thread, so a static variable is acceptable
		static bool shouldWaitForMessages = false;

		if ( std::exchange(shouldWaitForMessages, false) )
		{
			GetMessageA( lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax );
			return TRUE; // GetMessage definitely processed a message
		}

		const BOOL result = PeekMessageA( lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg );
		shouldWaitForMessages = result == FALSE;
		return result;
	}
};

#if TARGET_VERSION < 1 // High CPU usage thread – CPU usage has been cut down by ~30%.
namespace ZeroSleepRemoval
{
	void WINAPI Sleep_NoZero(DWORD dwMilliseconds)
	{
		Sleep(dwMilliseconds != 0 ? dwMilliseconds : 1);
	}

	DWORD WINAPI SleepEx_NoZero(DWORD dwMilliseconds, BOOL bAlertable)
	{
		if (dwMilliseconds == 0)
		{
			Sleep(1);
			return 0;
		}
		return SleepEx(dwMilliseconds, bAlertable);
	}

	void ReplacedYield()
	{
		SwitchToThread();
	}

	void WINAPI SleepAsYield(DWORD /*dwMilliseconds*/)
	{
		SwitchToThread();
	}
}
#endif

namespace WinMainCmdLineFix
{
	int (WINAPI *orgWinMain)(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd);
	int WINAPI WinMain_AlignCmdLine(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
	{
		std::string alignedCmdLine(lpCmdLine);

		// Align the size to 16 bytes
		const size_t size = alignedCmdLine.size() + 1; // Include the null terminator in padding
		const size_t alignedSize = (size + 15) & ~15;
		alignedCmdLine.resize( alignedSize );

		return orgWinMain(hInstance, hPrevInstance, alignedCmdLine.data(), nShowCmd);
	}
}

namespace LLKeyboardHookRemoval
{
	static const HHOOK FAKE_HHOOK = HHOOK(-1);
	HHOOK SetWindowsHookExA_LLRemoval(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId)
	{
		if (idHook == WH_KEYBOARD_LL)
		{
			return FAKE_HHOOK;
		}
		return SetWindowsHookExA(idHook, lpfn, hmod, dwThreadId);
	}

	BOOL WINAPI UnhookWindowsHookEx_LLRemoval(HHOOK hhk)
	{
		if (hhk == FAKE_HHOOK)
		{
			return TRUE;
		}
		return UnhookWindowsHookEx(hhk);
	}
}

namespace UTF8PathFixes
{
	static std::wstring UTF8ToWchar(std::string_view text)
	{
		std::wstring result;

		const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
		if ( count != 0 )
		{
			result.resize(count);
			MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
		}

		return result;
	}

	static std::string WcharToUTF8(std::wstring_view text)
	{
		std::string result;

		const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		if ( count != 0 )
		{
			result.resize(count);
			WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
		}

		return result;
	}

	BOOL WINAPI CreateDirectoryUTF8(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
	{
		return CreateDirectoryW(UTF8ToWchar(lpPathName).c_str(), lpSecurityAttributes);
	}

	DWORD WINAPI GetFileAttributesUTF8(LPCSTR lpFileName)
	{
		return GetFileAttributesW(UTF8ToWchar(lpFileName).c_str());
	}

	HANDLE WINAPI CreateFileUTF8(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
				DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
	{
		return CreateFileW(UTF8ToWchar(lpFileName).c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
	}

	int WINAPI WideCharToMultiByte_UTF8(UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte,
		LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar)
	{
		return WideCharToMultiByte(CodePage != 0 ? CodePage : CP_UTF8, dwFlags, lpWideCharStr, cchWideChar, lpMultiByteStr, cbMultiByte, lpDefaultChar, lpUsedDefaultChar);
	}

	int WINAPI MultiByteToWideChar_UTF8(UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar)
	{
		return MultiByteToWideChar(CodePage != 0 ? CodePage : CP_UTF8, dwFlags, lpMultiByteStr, cbMultiByte, lpWideCharStr, cchWideChar);
	}
}

namespace RtResizeThreadFix
{
	HANDLE waitEvent;

	static void** threadLoopReplacedVar;
	void* ResizeThreadWait()
	{
		WaitForSingleObject(waitEvent, INFINITE);
		return *threadLoopReplacedVar;
	}

	static uint32_t* terminateRtResizeThread;
	void TerminateResizeThread()
	{
		InterlockedExchange(terminateRtResizeThread, 1);
		SetEvent(waitEvent);
	}

	void SignalResolutionChange()
	{
		SetEvent(waitEvent);
	}
}

#if DEBUG_DOCUMENTS_PATH
HRESULT WINAPI SHGetKnownFolderPath_Fake(REFKNOWNFOLDERID rfid, DWORD dwFlags, HANDLE hToken, PWSTR *ppszPath)
{
	if ( rfid == FOLDERID_Documents )
	{
		constexpr wchar_t debugPath[] = L"H:\\ŻąłóРстуぬねのはen\\🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱\\Docu Ments";
		*ppszPath = static_cast<PWSTR>(CoTaskMemAlloc( sizeof(debugPath) ));
		memcpy( *ppszPath, debugPath, sizeof(debugPath) );
		return S_OK;
	}

	if ( rfid == FOLDERID_LocalAppData )
	{
		constexpr wchar_t debugPath[] = L"H:\\ŻąłóРстуぬねのはen\\🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱\\Lo Cal";
		*ppszPath = static_cast<PWSTR>(CoTaskMemAlloc( sizeof(debugPath) ));
		memcpy( *ppszPath, debugPath, sizeof(debugPath) );
		return S_OK;
	}

	if ( rfid == FOLDERID_RoamingAppData )
	{
		constexpr wchar_t debugPath[] = L"H:\\ŻąłóРстуぬねのはen\\🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱\\Roa Ming";
		*ppszPath = static_cast<PWSTR>(CoTaskMemAlloc( sizeof(debugPath) ));
		memcpy( *ppszPath, debugPath, sizeof(debugPath) );
		return S_OK;
	}

	return SHGetKnownFolderPath(rfid, dwFlags, hToken, ppszPath);
}
#endif


static void RedirectImports()
{
	const DWORD_PTR instance = reinterpret_cast<DWORD_PTR>(GetModuleHandle(nullptr));
	const PIMAGE_NT_HEADERS ntHeader = reinterpret_cast<PIMAGE_NT_HEADERS>(instance + reinterpret_cast<PIMAGE_DOS_HEADER>(instance)->e_lfanew);

	ScopedUnprotect::Section Protect( reinterpret_cast<HINSTANCE>(instance), ".idata" );

	// Find IAT
	PIMAGE_IMPORT_DESCRIPTOR pImports = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(instance + ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

	for ( ; pImports->Name != 0; pImports++ )
	{
		if ( _stricmp(reinterpret_cast<const char*>(instance + pImports->Name), "kernel32.dll") == 0 )
		{
			assert ( pImports->OriginalFirstThunk != 0 );

			const PIMAGE_THUNK_DATA pFunctions = reinterpret_cast<PIMAGE_THUNK_DATA>(instance + pImports->OriginalFirstThunk);

			for ( ptrdiff_t j = 0; pFunctions[j].u1.AddressOfData != 0; j++ )
			{
#if TARGET_VERSION < 1 // High CPU usage thread – CPU usage has been cut down by ~30%.
				if ( strcmp(reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(instance + pFunctions[j].u1.AddressOfData)->Name, "Sleep") == 0 )
				{
					void** pAddress = reinterpret_cast<void**>(instance + pImports->FirstThunk) + j;
					*pAddress = ZeroSleepRemoval::Sleep_NoZero;
					continue;
				}
				if ( strcmp(reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(instance + pFunctions[j].u1.AddressOfData)->Name, "SleepEx") == 0 )
				{
					void** pAddress = reinterpret_cast<void**>(instance + pImports->FirstThunk) + j;
					*pAddress = ZeroSleepRemoval::SleepEx_NoZero;
					continue;
				}
#endif
				if ( strcmp(reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(instance + pFunctions[j].u1.AddressOfData)->Name, "CreateFileA") == 0 )
				{
					void** pAddress = reinterpret_cast<void**>(instance + pImports->FirstThunk) + j;
					*pAddress = UTF8PathFixes::CreateFileUTF8;
					continue;
				}
				if ( strcmp(reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(instance + pFunctions[j].u1.AddressOfData)->Name, "CreateDirectoryA") == 0 )
				{
					void** pAddress = reinterpret_cast<void**>(instance + pImports->FirstThunk) + j;
					*pAddress = UTF8PathFixes::CreateDirectoryUTF8;
					continue;
				}
				if ( strcmp(reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(instance + pFunctions[j].u1.AddressOfData)->Name, "GetFileAttributesA") == 0 )
				{
					void** pAddress = reinterpret_cast<void**>(instance + pImports->FirstThunk) + j;
					*pAddress = UTF8PathFixes::GetFileAttributesUTF8;
					continue;
				}
				if ( strcmp(reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(instance + pFunctions[j].u1.AddressOfData)->Name, "WideCharToMultiByte") == 0 )
				{
					void** pAddress = reinterpret_cast<void**>(instance + pImports->FirstThunk) + j;
					*pAddress = UTF8PathFixes::WideCharToMultiByte_UTF8;
					continue;
				}
				if ( strcmp(reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(instance + pFunctions[j].u1.AddressOfData)->Name, "MultiByteToWideChar") == 0 )
				{
					void** pAddress = reinterpret_cast<void**>(instance + pImports->FirstThunk) + j;
					*pAddress = UTF8PathFixes::MultiByteToWideChar_UTF8;
					continue;
				}
			}
			continue;
		}

#if DEBUG_DOCUMENTS_PATH
		if ( _stricmp(reinterpret_cast<const char*>(instance + pImports->Name), "shell32.dll") == 0 )
		{
			assert ( pImports->OriginalFirstThunk != 0 );

			const PIMAGE_THUNK_DATA pFunctions = reinterpret_cast<PIMAGE_THUNK_DATA>(instance + pImports->OriginalFirstThunk);

			for ( ptrdiff_t j = 0; pFunctions[j].u1.AddressOfData != 0; j++ )
			{
				if ( strcmp(reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(instance + pFunctions[j].u1.AddressOfData)->Name, "SHGetKnownFolderPath") == 0 )
				{
					void** pAddress = reinterpret_cast<void**>(instance + pImports->FirstThunk) + j;
					*pAddress = SHGetKnownFolderPath_Fake;
				}
			}
			continue;
		}
#endif

		// Low level keyboard hook removed
		if ( _stricmp(reinterpret_cast<const char*>(instance + pImports->Name), "user32.dll") == 0 )
		{
			assert ( pImports->OriginalFirstThunk != 0 );

			const PIMAGE_THUNK_DATA pFunctions = reinterpret_cast<PIMAGE_THUNK_DATA>(instance + pImports->OriginalFirstThunk);

			for ( ptrdiff_t j = 0; pFunctions[j].u1.AddressOfData != 0; j++ )
			{
				if ( strcmp(reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(instance + pFunctions[j].u1.AddressOfData)->Name, "SetWindowsHookExA") == 0 )
				{
					void** pAddress = reinterpret_cast<void**>(instance + pImports->FirstThunk) + j;
					*pAddress = LLKeyboardHookRemoval::SetWindowsHookExA_LLRemoval;
					continue;
				}
				if ( strcmp(reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(instance + pFunctions[j].u1.AddressOfData)->Name, "UnhookWindowsHookEx") == 0 )
				{
					void** pAddress = reinterpret_cast<void**>(instance + pImports->FirstThunk) + j;
					*pAddress = LLKeyboardHookRemoval::UnhookWindowsHookEx_LLRemoval;
					continue;
				}
			}
			continue;
		}
	}
}


void OnInitializeHook()
{
	std::unique_ptr<ScopedUnprotect::Unprotect> Protect = ScopedUnprotect::UnprotectSectionOrFullModule( GetModuleHandle( nullptr ), ".text" );

	using namespace Memory;
	using namespace hook;

	enum class Game
	{
		Yakuza3,
		Yakuza4,
		Yakuza5, // Unsupported for now
	} game;
	{
		auto gameWindowName = pattern( "4C 8D 05 ? ? ? ? 48 8B 15 ? ? ? ? 33 DB" ).count_hint(1);
		if ( gameWindowName.size() == 1 )
		{
			// Read the window name from the pointer
			void* match = gameWindowName.get_first( 3 );

			const char* windowName;
			ReadOffsetValue( match, windowName );
			game = windowName == std::string_view("Yakuza 4") ? Game::Yakuza4 : Game::Yakuza3;
		}
		else
		{
			// Not found? Most likely Yakuza 5
			// Not supported yet
			game = Game::Yakuza5;
		}
	}


	RedirectImports();
	
	// Restore thread names	
	if ( auto createThreadPattern = pattern( "FF 15 ? ? ? ? 48 89 ? 20 48 85 C0 74 5D" ).count(1); createThreadPattern.size() == 1 )
	{
		auto addr = createThreadPattern.get_first( 2 );

		Trampoline* hop = Trampoline::MakeTrampoline( addr );

		void** funcPtr = hop->Pointer<void*>();
		*funcPtr = &CreateThread_SetDesc;
		WriteOffsetValue( addr, funcPtr );
	}


	// Message pump thread using less CPU time
	if ( auto peekMessage = pattern( "FF 15 ? ? ? ? 85 C0 74 16" ).count_hint(1); peekMessage.size() == 1 )
	{
		using namespace MessagePumpFixes;

		auto match = peekMessage.get_one();
		Trampoline* trampoline = Trampoline::MakeTrampoline( match.get<void*>() );

		void** funcPtr = trampoline->Pointer<void*>();
		*funcPtr = &PeekMessageA_WaitForMessages;
		WriteOffsetValue( match.get<void*>( 2 ), funcPtr );

		const uint8_t elseStatementPayload[] = {
			0xFF, 0x15, 0x0, 0x0, 0x0, 0x0, // call ds:[DispatchMessageA]
			0xE9, 0x0, 0x0, 0x0, 0x0 // jmp loc_1404CF4A0
		};

		std::byte* space = trampoline->RawSpace( sizeof(elseStatementPayload) );
		memcpy( space, elseStatementPayload, sizeof(elseStatementPayload) );

		// Fill pointers accordingly and redirect to payload
		void* orgDispatchMessage;
		ReadOffsetValue( match.get<void*>( 0x1A + 2 ), orgDispatchMessage );
		WriteOffsetValue( space + 2, orgDispatchMessage );
		WriteOffsetValue( space + 6 + 1, match.get<void*>( -0x28 ) );

		InjectHook( match.get<void*>( 0x1A ), space, PATCH_JUMP );
	}


#if TARGET_VERSION < 1 // Random crash when ending fights with Heat Move – we managed to fix a crash occurring occasionally after finishing battles with a Heat Action.
	// Post-battle race condition crash workaround
	// HACK! A real fix is probably realistically not possible to do without
	// the source access.
	// The game crashes at
	// movsx eax, word ptr [rdx+rcx*8]
	// so add an early out from the job if rdx is 0
	{
		auto earlyOutPoint_pattern = pattern( "48 8B 57 18 41 8B C8" );
		auto earlyOutJumpAddr_pattern = pattern( "B8 05 40 00 80 48 81 C4 E0 21 00 00" );
		if ( earlyOutPoint_pattern.count(1).size() == 1 && earlyOutJumpAddr_pattern.count(1).size() == 1 )
		{
			auto earlyOutPoint = earlyOutPoint_pattern.get_first( 4 );
			auto earlyOutJumpAddr = earlyOutJumpAddr_pattern.get_first();

			Trampoline* trampoline = Trampoline::MakeTrampoline( earlyOutPoint );

			const uint8_t payload[] = {
				0x48, 0x85, 0xD2, // test rdx, rdx
				0x0F, 0x84, 0x0, 0x0, 0x0, 0x0, // jz earlyOutJumpAddr
				0x41, 0x8B, 0xC8, // mov ecx, r8d
				0x48, 0x03, 0xC9, // add rcx, rcx
				0xE9, 0x0, 0x0, 0x0, 0x0, // jmp earlyOutPoint+6
			};

			std::byte* space = trampoline->RawSpace( sizeof(payload) );
			memcpy( space, payload, sizeof(payload) );

			// Fill pointers accordingly and redirect to payload
			WriteOffsetValue( space + 3 + 2, earlyOutJumpAddr );
			WriteOffsetValue( space + 3 + 6 + 3 + 3 + 1, reinterpret_cast<intptr_t>(earlyOutPoint) + 6 );

			InjectHook( earlyOutPoint, space, PATCH_JUMP );
		}
	}
#endif


#if TARGET_VERSION < 1 // High CPU usage thread – CPU usage has been cut down by ~30%.
	// Sleepless render idle
	if ( auto renderSleep = pattern( "33 C9 FF 15 ? ? ? ? 48 8D 8D" ).count(1); renderSleep.size() == 1 )
	{
		auto match = renderSleep.get_first( 2 + 2 );
		Trampoline* trampoline = Trampoline::MakeTrampoline( match );

		void** funcPtr = trampoline->Pointer<void*>();
		*funcPtr = &ZeroSleepRemoval::SleepAsYield;

		WriteOffsetValue( match, funcPtr );
	}


	// Sleepless gxd::server_job
	// (Yakuza 4 only)
	// Also for Yakuza 3 for now, else causes slowdowns without Special K
	//if ( game == Game::Yakuza4 )
	{
		auto serverJob = pattern( "E8 ? ? ? ? 83 3D ? ? ? ? ? 74 83" ).count(1);
		if ( serverJob.size() == 1 )
		{
			auto match = serverJob.get_first();
			Trampoline* trampoline = Trampoline::MakeTrampoline( match );
			InjectHook( match, trampoline->Jump(ZeroSleepRemoval::ReplacedYield) );
		}
	}
#endif

	// Work around read-past-bounds issues in WinMain
	// Ideally, it should have been fixed by replacing buggy SSE-based string comparison,
	// but padding the passed memory to 16 bytes fixes the root cause just fine
	auto detourWinMain = [](hook::pattern& pattern, ptrdiff_t offset) {
		using namespace WinMainCmdLineFix;

		// Since Yakuza 3, Yakuza 4 and Yakuza 5 have slightly different WinMain prologues and very different callees,
		// detour WinMain properly
		auto match = pattern.get_one();
		auto funcStart = match.get<void>( -offset );
		Trampoline* trampoline = Trampoline::MakeTrampoline( funcStart );

		std::byte* trampolineSpace = trampoline->RawSpace( offset + 5 );
		orgWinMain = reinterpret_cast<decltype(orgWinMain)>(trampolineSpace);

		memcpy( trampolineSpace, funcStart, offset );
		trampolineSpace += offset;
		InjectHook( trampolineSpace, match.get<void>(), PATCH_JUMP );

		// Trampoline to the custom function
		InjectHook( funcStart, trampoline->Jump(WinMain_AlignCmdLine), PATCH_JUMP );
	};

	// Yakuza 3/4
	if ( auto winMain3 = pattern( "48 8D AC 24 B0 FD FF FF 48 81 EC 50 03 00 00 48 8B 05" ).count_hint(1); winMain3.size() == 1 )
	{
		detourWinMain(winMain3, 5);
	}
	// Yakuza 5
	else if ( auto winMain5 = pattern( "41 55 41 56 41 57 48 8D A8 78 FE FF FF 48 81 EC 60 02 00 00 48 C7 45 C0 FE FF FF FF 48 89 58 10 48 89 70 18 48 89 78 20 48 8B 05" ).count_hint(1);
		winMain5.size() == 1 )
	{
		detourWinMain(winMain5, 6);
	}


	// Reduce CPU usage of a rt_resize_thread (Yakuza 5)
	// Add an event waking up this thread to avoid it looping infinitely, burning CPU cycles
	{
		auto rtThreadLoop = pattern( "48 8B 05 ? ? ? ? 49 89 04 2F" );
		auto signalRtThreadFinish = pattern( "87 35 ? ? ? ? 8B 05" );
		auto signalResolutionChange = pattern( "C6 43 03 01 89 43 10" );
		if ( rtThreadLoop.count_hint(1).size() == 1 && signalRtThreadFinish.count_hint(1).size() == 1 && signalResolutionChange.count_hint(1).size() == 1 )
		{
			using namespace RtResizeThreadFix;

			// rt_resize_thread: Replace
			// mov rax, cs:qword_141D955C0
			// with a function call waiting and returning this variable
			{
				auto match = rtThreadLoop.get_one();
				Trampoline* trampoline = Trampoline::MakeTrampoline( match.get<void>() );

				ReadOffsetValue( match.get<void>( 3 ), threadLoopReplacedVar );
				Nop( match.get<void>(), 2 );
				InjectHook( match.get<void>( 2 ), trampoline->Jump(ResizeThreadWait), PATCH_CALL );
			}

			// WinMain: Replace
			// xchg esi, cs:terminateRtResizeThread
			// with a function call atomically setting this variable and waking up the event
			{
				auto match = signalRtThreadFinish.get_one();
				Trampoline* trampoline = Trampoline::MakeTrampoline( match.get<void>() );

				ReadOffsetValue( match.get<void>( 2 ), terminateRtResizeThread );
				Nop( match.get<void>(), 1 );
				InjectHook( match.get<void>( 1 ), trampoline->Jump(TerminateResizeThread), PATCH_CALL );
			}

			// sub_1413253B0: Detour
			// mov byte ptr [rbx+3], 1
			// mov [rbx+10h], eax
			// to a trampoline and jump to wake up the event
			{
				auto match = signalResolutionChange.get_one();
				Trampoline* trampoline = Trampoline::MakeTrampoline( match.get<void>() );

				// Can't use trampoline->Jump as that overwrites rax!
				std::byte* trampolineSpace = trampoline->RawSpace( 7 + 14 );
				memcpy( trampolineSpace, match.get<void>(), 4 + 3 );
				InjectHook( match.get<void>(), trampolineSpace, PATCH_CALL );
				Nop( match.get<void>( 5 ), 2 );
				trampolineSpace += 7;

				const uint8_t jmpRip[] = { 0xFF, 0x25 };
				memcpy( trampolineSpace, jmpRip, sizeof(jmpRip) );
				trampolineSpace += 6; // Leave offset at 0

				auto funcPtr = &SignalResolutionChange;
				memcpy( trampolineSpace, &funcPtr, sizeof(funcPtr) );
			}

			waitEvent = CreateEvent(nullptr, FALSE, TRUE, nullptr);
		}
	}

	// log current time to file to get some feedback once hook is done
	{
		using namespace std::chrono;
		const auto t1 = system_clock::now();
		const auto s1 = std::format("{:%Y/%m/%d %H:%M:%S}", floor<seconds>(t1));
		// to local time, ref: https://akrzemi1.wordpress.com/2022/04/24/local-time/
		const auto tz = current_zone();
		const auto t2 = tz->to_local(t1);
		const auto s2 = std::format("{:%Y/%m/%d %H:%M:%S}", floor<seconds>(t2));
		auto ofs = std::ofstream("SilentPatchYRC.txt", std::ios::binary | std::ios::trunc | std::ios::out);
		if (game == Game::Yakuza3) {
			ofs << "Game:  " << "Yakuza 3" << std::endl;
		}
		else if (game == Game::Yakuza4) {
			ofs << "Game:  " << "Yakuza 4" << std::endl;
		}
		else if (game == Game::Yakuza5) {
			ofs << "Game:  " << "Yakuza 5" << std::endl;
		}
		ofs << "Local: " << s2 << std::endl;
		ofs << "UTC:   " << s1 << std::endl;
		ofs.close();
	}
}
