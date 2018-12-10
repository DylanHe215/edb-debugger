/*
Copyright (C) 2006 - 2015 Evan Teran
                          evan.teran@gmail.com

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "DebuggerCore.h"
#include "edb.h"

#include "MemoryRegions.h"
#include "PlatformEvent.h"
#include "PlatformProcess.h"
#include "PlatformRegion.h"
#include "PlatformState.h"
#include "PlatformThread.h"
#include "State.h"
#include "string_hash.h"

#include <QDateTime>
#include <QDebug>
#include <QStringList>
#include <QFileInfo>

#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>

#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Psapi.lib")
#endif

/* NOTE(eteran): from the MSDN:
 * Note that while reporting debug events, all threads within the reporting
 * process are frozen. Debuggers are expected to use the SuspendThread and
 * ResumeThread functions to limit the set of threads that can execute within a
 * process. By suspending all threads in a process except for the one reporting
 * a debug event, it is possible to "single step" a single thread. The other
 * threads are not released by a continue operation if they are suspended.
 */

namespace DebuggerCorePlugin {

namespace {

/*
 * Required to debug and adjust the memory of a process owned by another account.
 * OpenProcess quote (MSDN):
 *   "If the caller has enabled the SeDebugPrivilege privilege, the requested access
 *    is granted regardless of the contents of the security descriptor."
 * Needed to open system processes (user SYSTEM)
 *
 * NOTE: You need to be admin to enable this privilege
 * NOTE: You need to have the 'Debug programs' privilege set for the current user,
 *       if the privilege is not present it can't be enabled!
 * NOTE: Detectable by antidebug code (changes debuggee privileges too)
 */
bool set_debug_privilege(HANDLE process, bool set) {

	HANDLE token;
	bool ok = false;

	//process must have PROCESS_QUERY_INFORMATION
	if(OpenProcessToken(process, TOKEN_ADJUST_PRIVILEGES, &token)) {

		LUID luid;
		if(LookupPrivilegeValue(nullptr, SE_DEBUG_NAME, &luid)) {
			TOKEN_PRIVILEGES tp;
			tp.PrivilegeCount = 1;
			tp.Privileges[0].Luid = luid;
			tp.Privileges[0].Attributes = set ? SE_PRIVILEGE_ENABLED : 0;

			ok = AdjustTokenPrivileges(token, false, &tp, NULL, nullptr, nullptr);
		}
		CloseHandle(token);
	}

	return ok;
}

}


//------------------------------------------------------------------------------
// Name: DebuggerCore
// Desc: constructor
//------------------------------------------------------------------------------
DebuggerCore::DebuggerCore() {
	DebugSetProcessKillOnExit(false);

	SYSTEM_INFO sys_info;
	GetSystemInfo(&sys_info);
	page_size_ = sys_info.dwPageSize;

	set_debug_privilege(GetCurrentProcess(), true); // gogo magic powers
}

//------------------------------------------------------------------------------
// Name: ~DebuggerCore
// Desc:
//------------------------------------------------------------------------------
DebuggerCore::~DebuggerCore() {
	detach();
	set_debug_privilege(GetCurrentProcess(), false);
}

//------------------------------------------------------------------------------
// Name: page_size
// Desc: returns the size of a page on this system
//------------------------------------------------------------------------------
size_t DebuggerCore::page_size() const {
	return page_size_;
}

//------------------------------------------------------------------------------
// Name: has_extension
// Desc:
//------------------------------------------------------------------------------
bool DebuggerCore::has_extension(quint64 ext) const {
#if !defined(EDB_X86_64)
	switch(ext) {
	case edb::string_hash("MMX"):
		return IsProcessorFeaturePresent(PF_MMX_INSTRUCTIONS_AVAILABLE);
	case edb::string_hash("XMM"):
		return IsProcessorFeaturePresent(PF_XMMI_INSTRUCTIONS_AVAILABLE);
	default:
		return false;
	}
#else
	switch(ext) {
	case edb::string_hash("MMX"):
	case edb::string_hash("XMM"):
		return true;
	default:
		return false;
	}
#endif
}

//------------------------------------------------------------------------------
// Name: wait_debug_event
// Desc: waits for a debug event, secs is a timeout (but is not yet respected)
//       ok will be set to false if the timeout expires
//------------------------------------------------------------------------------
std::shared_ptr<IDebugEvent> DebuggerCore::wait_debug_event(int msecs) {
	if(attached()) {
		DEBUG_EVENT de;
		while(WaitForDebugEvent(&de, msecs == 0 ? INFINITE : static_cast<DWORD>(msecs))) {

			Q_ASSERT(process_->pid() == de.dwProcessId);

			active_thread_ = de.dwThreadId;
			bool propagate = false;

			switch(de.dwDebugEventCode) {
			case CREATE_THREAD_DEBUG_EVENT: {
				auto newThread = std::make_shared<PlatformThread>(this, process_, &de.u.CreateThread);
				threads_.insert(active_thread_, newThread);
				break;
			}
			case EXIT_THREAD_DEBUG_EVENT:
				threads_.remove(active_thread_);
				break;
			case CREATE_PROCESS_DEBUG_EVENT: {				
				CloseHandle(de.u.CreateProcessInfo.hFile);

				process_ = std::make_shared<PlatformProcess>(this, de.u.CreateProcessInfo.hProcess);

				// fake a thread create event for the main thread..
				CREATE_THREAD_DEBUG_INFO thread_info;
				thread_info.hThread           = de.u.CreateProcessInfo.hThread;
				thread_info.lpStartAddress    = de.u.CreateProcessInfo.lpStartAddress;
				thread_info.lpThreadLocalBase = de.u.CreateProcessInfo.lpThreadLocalBase;
				auto newThread = std::make_shared<PlatformThread>(this, process_, &thread_info);
				threads_.insert(active_thread_, newThread);
				break;
			}
			case LOAD_DLL_DEBUG_EVENT:
				CloseHandle(de.u.LoadDll.hFile);
				break;
			case EXIT_PROCESS_DEBUG_EVENT:
				process_->resume(edb::DEBUG_CONTINUE);
				process_        = nullptr;
				// handle_event_exited returns DEBUG_STOP, which in turn keeps the debugger from resuming the process
				// However, this is needed to close all internal handles etc. and finish the debugging session
				// So we do it manually here
				propagate = true;
				break;
			case EXCEPTION_DEBUG_EVENT:
				propagate = true;
				break;
			case RIP_EVENT:
				break;
			default:
				break;
			}

			if(auto p = static_cast<PlatformProcess *>(process_.get())) {
				p->lastEvent_ = de;
			}

			if(propagate) {
				// normal event
				auto e = std::make_shared<PlatformEvent>();
				e->event = de;
				return e;
			}

			process_->resume(edb::DEBUG_EXCEPTION_NOT_HANDLED);
		}
	}
	return nullptr;
}

//------------------------------------------------------------------------------
// Name: attach
// Desc:
//------------------------------------------------------------------------------
Status DebuggerCore::attach(edb::pid_t pid) {

	detach();

	if(DebugActiveProcess(pid)) {
		process_ = std::make_shared<PlatformProcess>(this, pid);
		return Status::Ok;
	}

	return Status("Error DebuggerCore::attach");
}

//------------------------------------------------------------------------------
// Name: detach
// Desc:
//------------------------------------------------------------------------------
Status DebuggerCore::detach() {
	if(attached()) {
		clear_breakpoints();
		// Make sure exceptions etc. are passed
		ContinueDebugEvent(process_->pid(), active_thread(), DBG_CONTINUE);
		DebugActiveProcessStop(process_->pid());
		process_ = nullptr;
		threads_.clear();
	}
	return Status::Ok;
}

//------------------------------------------------------------------------------
// Name: kill
// Desc:
//------------------------------------------------------------------------------
void DebuggerCore::kill() {
	if(auto p = static_cast<PlatformProcess *>(process_.get())) {
		TerminateProcess(p->hProcess_, -1);
		detach();
	}
}

//------------------------------------------------------------------------------
// Name: open
// Desc:
// TODO: Don't inherit security descriptors from this process (default values)
//       Is this even possible?
//------------------------------------------------------------------------------
Status DebuggerCore::open(const QString &path, const QString &cwd, const QList<QByteArray> &args, const QString &tty) {

	Q_UNUSED(tty);

	Q_ASSERT(!path.isEmpty());

	bool ok = false;

	detach();

	// default to process's directory
	QString tcwd;
	if(cwd.isEmpty()) {
		tcwd = QFileInfo(path).canonicalPath();
	} else {
		tcwd = cwd;
	}

	STARTUPINFO         startup_info = { 0 };
	PROCESS_INFORMATION process_info = { nullptr };

	const DWORD CREATE_FLAGS = DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE;

    wchar_t *const env_block = GetEnvironmentStringsW();

	// Set up command line
	QString command_str = '\"' + QFileInfo(path).canonicalPath() + '\"'; // argv[0] = full path (explorer style)
	if(!args.isEmpty()) {
		for(QByteArray arg: args) {
			command_str += " ";
			command_str += arg;
		}
	}

	// CreateProcessW wants a writable copy of the command line :<
	auto command_path = new wchar_t[command_str.length() + sizeof(wchar_t)];
    wcscpy_s(command_path, command_str.length() + 1, reinterpret_cast<const wchar_t*>(command_str.utf16()));

	if(CreateProcessW(
			reinterpret_cast<const wchar_t*>(path.utf16()), // exe
	        command_path,    // commandline
	        nullptr,         // default security attributes
	        nullptr,         // default thread security too
	        FALSE,           // inherit handles
			CREATE_FLAGS,
	        env_block,       // environment data
			reinterpret_cast<const wchar_t*>(tcwd.utf16()), // working directory
			&startup_info,
			&process_info)) {

		active_thread_ = process_info.dwThreadId;
		CloseHandle(process_info.hThread); // We don't need the thread handle
		set_debug_privilege(process_info.hProcess, false);

		//process_info.hProcess  has PROCESS_ALL_ACCESS
		process_ = std::make_shared<PlatformProcess>(this, process_info.hProcess);

		ok = true;
	}

	delete[] command_path;
	FreeEnvironmentStringsW(env_block);

	if (ok) {
		return Status::Ok;
	} else {
		return Status("Error DebuggerCore::open");
	}
}

//------------------------------------------------------------------------------
// Name: create_state
// Desc:
//------------------------------------------------------------------------------
std::unique_ptr<IState> DebuggerCore::create_state() const {
	return std::make_unique<PlatformState>();
}

//------------------------------------------------------------------------------
// Name: sys_pointer_size
// Desc: returns the size of a pointer on this arch
//------------------------------------------------------------------------------
int DebuggerCore::sys_pointer_size() const {
	return sizeof(void *);
}

//------------------------------------------------------------------------------
// Name: enumerate_processes
// Desc:
//------------------------------------------------------------------------------
QMap<edb::pid_t, std::shared_ptr<IProcess> > DebuggerCore::enumerate_processes() const {
	QMap<edb::pid_t, std::shared_ptr<IProcess> > ret;

	HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if(handle != INVALID_HANDLE_VALUE) {

		PROCESSENTRY32 lppe;

		std::memset(&lppe, 0, sizeof(lppe));
		lppe.dwSize = sizeof(lppe);

		if(Process32First(handle, &lppe)) {
			do {
				// NOTE(eteran): the const_cast is reasonable here.
				// While we don't want THIS function to mutate the DebuggerCore object
				// we do want the associated PlatformProcess to be able to trigger
				// non-const operations in the future, at least hypothetically.
				auto pi = std::make_shared<PlatformProcess>(const_cast<DebuggerCore*>(this), lppe.th32ProcessID);
				if(pi->hProcess_ == nullptr) {
					continue;
				}

				ret.insert(pi->pid(), pi);

				std::memset(&lppe, 0, sizeof(lppe));
				lppe.dwSize = sizeof(lppe);
			} while(Process32Next(handle, &lppe));
		}

		CloseHandle(handle);
	}
	return ret;
}


//------------------------------------------------------------------------------
// Name:
// Desc:
//------------------------------------------------------------------------------
edb::pid_t DebuggerCore::parent_pid(edb::pid_t pid) const {
	edb::pid_t parent = 1; // 1??
	HANDLE hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, pid);
	if(hProcessSnap != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W pe32;
		pe32.dwSize = sizeof(pe32);

		if(Process32FirstW(hProcessSnap, &pe32)) {
			do {
				if(pid == pe32.th32ProcessID) {
					parent = pe32.th32ParentProcessID;
					break;
				}
			} while(Process32NextW(hProcessSnap, &pe32));
		}
		CloseHandle(hProcessSnap);
	}
	return parent;
}


//------------------------------------------------------------------------------
// Name:
// Desc:
//------------------------------------------------------------------------------
QMap<qlonglong, QString> DebuggerCore::exceptions() const {
	QMap<qlonglong, QString> exceptions;

	return exceptions;
}

//------------------------------------------------------------------------------
// Name: cpu_type
// Desc:
//------------------------------------------------------------------------------
quint64 DebuggerCore::cpu_type() const {
#ifdef EDB_X86
	return edb::string_hash("x86");
#elif defined(EDB_X86_64)
	return edb::string_hash("x86-64");
#endif
}

//------------------------------------------------------------------------------
// Name:
// Desc:
//------------------------------------------------------------------------------
QString DebuggerCore::stack_pointer() const {
#ifdef EDB_X86
	return "esp";
#elif defined(EDB_X86_64)
	// TODO(eteran): WOW64 support
	return "rsp";
#endif
}

//------------------------------------------------------------------------------
// Name:
// Desc:
//------------------------------------------------------------------------------
QString DebuggerCore::frame_pointer() const {
#ifdef EDB_X86
	return "ebp";
#elif defined(EDB_X86_64)
	// TODO(eteran): WOW64 support
	return "rbp";
#endif
}

//------------------------------------------------------------------------------
// Name:
// Desc:
//------------------------------------------------------------------------------
QString DebuggerCore::instruction_pointer() const {
#ifdef EDB_X86
	return "eip";
#elif defined(EDB_X86_64)
	// TODO(eteran): WOW64 support
	return "rip";
#endif
}

IProcess *DebuggerCore::process() const  {
	return process_.get();
}

}
