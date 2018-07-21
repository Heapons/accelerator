/*
 * =============================================================================
 * Accelerator Extension
 * Copyright (C) 2011 Asher Baker (asherkin).  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "extension.h"

#include <IWebternet.h>
#include "MemoryDownloader.h"

#if defined _LINUX
#include "client/linux/handler/exception_handler.h"
#include "common/linux/linux_libc_support.h"
#include "third_party/lss/linux_syscall_support.h"
#include "common/linux/dump_symbols.h"
#include "common/path_helper.h"

#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <paths.h>

class StderrInhibitor
{
	FILE *saved_stderr = nullptr;

public:
	StderrInhibitor() {
		saved_stderr = fdopen(dup(fileno(stderr)), "w");
		if (freopen(_PATH_DEVNULL, "w", stderr)) {
			// If it fails, not a lot we can (or should) do.
			// Add this brace section to silence gcc warnings.
		}
	}

	~StderrInhibitor() {
		fflush(stderr);
		dup2(fileno(saved_stderr), fileno(stderr));
		fclose(saved_stderr);
	}
};

#elif defined _WINDOWS
#define _STDINT // ~.~
#include "client/windows/handler/exception_handler.h"

#else
#error Bad platform.
#endif

#include <google_breakpad/processor/minidump.h>
#include <google_breakpad/processor/minidump_processor.h>
#include <google_breakpad/processor/process_state.h>
#include <google_breakpad/processor/call_stack.h>
#include <google_breakpad/processor/stack_frame.h>
#include <processor/pathname_stripper.h>

#include <sstream>
#include <streambuf>

Accelerator g_accelerator;
SMEXT_LINK(&g_accelerator);

IWebternet *webternet;
IGameConfig *gameconfig;

typedef void (*GetSpew_t)(char *buffer, unsigned int length);
GetSpew_t GetSpew;
#if defined _WINDOWS
typedef void(__fastcall *GetSpewFastcall_t)(char *buffer, unsigned int length);
GetSpewFastcall_t GetSpewFastcall;
#endif

char spewBuffer[65536]; // Hi.

char crashMap[256];
char crashGamePath[512];
char crashCommandLine[1024];
char crashSourceModPath[512];
char crashGameDirectory[256];
char steamInf[1024];

char dumpStoragePath[512];
char logPath[512];

google_breakpad::ExceptionHandler *handler = NULL;

# if 0
struct PluginInfo {
	unsigned int serial;
	PluginStatus status;
	char filename[256];
	char name[256];
	char author[256];
	char description[256];
	char version[256];
	char url[256];
};

unsigned int plugin_count;
PluginInfo plugins[256];
#endif

#if defined _LINUX
void (*SignalHandler)(int, siginfo_t *, void *);

const int kExceptionSignals[] = {
	SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS
};

const int kNumHandledSignals = sizeof(kExceptionSignals) / sizeof(kExceptionSignals[0]);

static bool dumpCallback(const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded)
{
	//printf("Wrote minidump to: %s\n", descriptor.path());

	if (succeeded) {
		sys_write(STDOUT_FILENO, "Wrote minidump to: ", 19);
	} else {
		sys_write(STDOUT_FILENO, "Failed to write minidump to: ", 29);
	}

	sys_write(STDOUT_FILENO, descriptor.path(), my_strlen(descriptor.path()));
	sys_write(STDOUT_FILENO, "\n", 1);

	if (!succeeded) {
		return succeeded;
	}

	my_strlcpy(dumpStoragePath, descriptor.path(), sizeof(dumpStoragePath));
	my_strlcat(dumpStoragePath, ".txt", sizeof(dumpStoragePath));

	int extra = sys_open(dumpStoragePath, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (extra == -1) {
		sys_write(STDOUT_FILENO, "Failed to open metadata file!\n", 30);
		return succeeded;
	}

	sys_write(extra, "-------- CONFIG BEGIN --------", 30);
	sys_write(extra, "\nMap=", 5);
	sys_write(extra, crashMap, my_strlen(crashMap));
	sys_write(extra, "\nGamePath=", 10);
	sys_write(extra, crashGamePath, my_strlen(crashGamePath));
	sys_write(extra, "\nCommandLine=", 13);
	sys_write(extra, crashCommandLine, my_strlen(crashCommandLine));
	sys_write(extra, "\nSourceModPath=", 15);
	sys_write(extra, crashSourceModPath, my_strlen(crashSourceModPath));
	sys_write(extra, "\nGameDirectory=", 15);
	sys_write(extra, crashGameDirectory, my_strlen(crashGameDirectory));
	sys_write(extra, "\nExtensionVersion=", 18);
	sys_write(extra, SM_VERSION, my_strlen(SM_VERSION));
	sys_write(extra, "\nExtensionBuild=", 16);
	sys_write(extra, SM_BUILD_UNIQUEID, my_strlen(SM_BUILD_UNIQUEID));
	sys_write(extra, steamInf, my_strlen(steamInf));
	sys_write(extra, "\n-------- CONFIG END --------\n", 30);

	if (GetSpew) {
		GetSpew(spewBuffer, sizeof(spewBuffer));

		if (my_strlen(spewBuffer) > 0) {
			sys_write(extra, "-------- CONSOLE HISTORY BEGIN --------\n", 40);
			sys_write(extra, spewBuffer, my_strlen(spewBuffer));
			sys_write(extra, "-------- CONSOLE HISTORY END --------\n", 38);
		}
	}

#if 0
	char pis[64];
	char pds[32];
	for (unsigned i = 0; i < plugin_count; ++i) {
		PluginInfo *p = &plugins[i];
		if (p->serial == 0) continue;
		my_uitos(pds, i, my_uint_len(i));
		pds[my_uint_len(i)] = '\0';
		my_strlcpy(pis, "plugin[", sizeof(pis));
		my_strlcat(pis, pds, sizeof(pis));
		my_strlcat(pis, "].", sizeof(pis));
		sys_write(extra, pis, my_strlen(pis));
		sys_write(extra, "filename=", 9);
		sys_write(extra, p->filename, my_strlen(p->filename));
		sys_write(extra, "\n", 1);
		sys_write(extra, pis, my_strlen(pis));
		sys_write(extra, "name=", 5);
		sys_write(extra, p->name, my_strlen(p->name));
		sys_write(extra, "\n", 1);
		sys_write(extra, pis, my_strlen(pis));
		sys_write(extra, "author=", 7);
		sys_write(extra, p->author, my_strlen(p->author));
		sys_write(extra, "\n", 1);
		sys_write(extra, pis, my_strlen(pis));
		sys_write(extra, "description=", 12);
		sys_write(extra, p->description, my_strlen(p->description));
		sys_write(extra, "\n", 1);
		sys_write(extra, pis, my_strlen(pis));
		sys_write(extra, "version=", 8);
		sys_write(extra, p->version, my_strlen(p->version));
		sys_write(extra, "\n", 1);
		sys_write(extra, pis, my_strlen(pis));
		sys_write(extra, "url=", 4);
		sys_write(extra, p->url, my_strlen(p->url));
		sys_write(extra, "\n", 1);
	}
#endif

	sys_close(extra);

	return succeeded;
}

void OnGameFrame(bool simulating)
{
	bool weHaveBeenFuckedOver = false;
	struct sigaction oact;

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaction(kExceptionSignals[i], NULL, &oact);

		if (oact.sa_sigaction != SignalHandler) {
			weHaveBeenFuckedOver = true;
			break;
		}
	}

	if (!weHaveBeenFuckedOver) {
		return;
	}

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	sigemptyset(&act.sa_mask);

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaddset(&act.sa_mask, kExceptionSignals[i]);
	}

	act.sa_sigaction = SignalHandler;
	act.sa_flags = SA_ONSTACK | SA_SIGINFO;

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaction(kExceptionSignals[i], &act, NULL);
	}
}

#elif defined _WINDOWS
void *vectoredHandler = NULL;

LONG CALLBACK BreakpadVectoredHandler(_In_ PEXCEPTION_POINTERS ExceptionInfo)
{
	switch (ExceptionInfo->ExceptionRecord->ExceptionCode)
	{
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_INVALID_HANDLE:
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		case EXCEPTION_DATATYPE_MISALIGNMENT:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_STACK_OVERFLOW:
		case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN
		case 0xC0000374: // STATUS_HEAP_CORRUPTION
			break;
		case 0: // Valve use this for Sys_Error.
			if ((ExceptionInfo->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) == 0)
				return EXCEPTION_CONTINUE_SEARCH;
			break;
		default:
			return EXCEPTION_CONTINUE_SEARCH;
	}

	if (handler->WriteMinidumpForException(ExceptionInfo))
	{
		// Stop the handler thread from deadlocking us.
		delete handler;

		// Stop Valve's handler being called.
		ExceptionInfo->ExceptionRecord->ExceptionCode = EXCEPTION_BREAKPOINT;

		return EXCEPTION_EXECUTE_HANDLER;
	} else {
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

static bool dumpCallback(const wchar_t* dump_path,
                         const wchar_t* minidump_id,
                         void* context,
                         EXCEPTION_POINTERS* exinfo,
                         MDRawAssertionInfo* assertion,
                         bool succeeded)
{
	if (!succeeded) {
		printf("Failed to write minidump to: %ls\\%ls.dmp\n", dump_path, minidump_id);
		return succeeded;
	}

	printf("Wrote minidump to: %ls\\%ls.dmp\n", dump_path, minidump_id);

	sprintf(dumpStoragePath, "%ls\\%ls.dmp.txt", dump_path, minidump_id);

	FILE *extra = fopen(dumpStoragePath, "wb");
	if (!extra) {
		printf("Failed to open metadata file!\n");
		return succeeded;
	}

	fprintf(extra, "-------- CONFIG BEGIN --------");
	fprintf(extra, "\nMap=%s", crashMap);
	fprintf(extra, "\nGamePath=%s", crashGamePath);
	fprintf(extra, "\nCommandLine=%s", crashCommandLine);
	fprintf(extra, "\nSourceModPath=%s", crashSourceModPath);
	fprintf(extra, "\nGameDirectory=%s", crashGameDirectory);
	fprintf(extra, "\nExtensionVersion=%s", SM_VERSION);
	fprintf(extra, "\nExtensionBuild=%s", SM_BUILD_UNIQUEID);
	fprintf(extra, "%s", steamInf);
	fprintf(extra, "\n-------- CONFIG END --------\n");

	if (GetSpew || GetSpewFastcall) {
		if (GetSpew) {
			GetSpew(spewBuffer, sizeof(spewBuffer));
		} else if (GetSpewFastcall) {
			GetSpewFastcall(spewBuffer, sizeof(spewBuffer));
		}

		if (strlen(spewBuffer) > 0) {
			fprintf(extra, "-------- CONSOLE HISTORY BEGIN --------\n%s-------- CONSOLE HISTORY END --------\n", spewBuffer);
		}
	}

	fclose(extra);

	return succeeded;
}

#else
#error Bad platform.
#endif

class ClogInhibitor
{
	std::streambuf *saved_clog = nullptr;

public:
	ClogInhibitor() {
		saved_clog = std::clog.rdbuf();
		std::clog.rdbuf(nullptr);
	}

	~ClogInhibitor() {
		std::clog.rdbuf(saved_clog);
	}
};

class UploadThread: public IThread
{
	void RunThread(IThreadHandle *pHandle) {
		rootconsole->ConsolePrint("Accelerator upload thread started.");

		FILE *log = fopen(logPath, "a");
		if (!log) {
			g_pSM->LogError(myself, "Failed to open Accelerator log file: %s", logPath);
		}

		IDirectory *dumps = libsys->OpenDirectory(dumpStoragePath);

		int count = 0;
		int failed = 0;
		char path[512];
		char response[512];

		while (dumps->MoreFiles()) {
			if (!dumps->IsEntryFile()) {
				dumps->NextEntry();
				continue;
			}

			const char *name = dumps->GetEntryName();

			int namelen = strlen(name);
			if (namelen < 4 || strcmp(&name[namelen-4], ".dmp") != 0) {
				dumps->NextEntry();
				continue;
			}

			g_pSM->Format(path, sizeof(path), "%s/%s", dumpStoragePath, name);

			// TODO: Check the return value.
			bool wantsUpload = PresubmitCrashDump(path);

			bool uploaded = UploadAndDeleteCrashDump(path, response, sizeof(response));

			if (uploaded) {
				count++;
				g_pSM->LogError(myself, "Accelerator uploaded crash dump: %s", response);
				if (log) fprintf(log, "Uploaded crash dump: %s\n", response);
			} else {
				failed++;
				g_pSM->LogError(myself, "Accelerator failed to upload crash dump: %s", response);
                                if (log) fprintf(log, "Failed to upload crash dump: %s\n", response);
			}

			dumps->NextEntry();
		}

		libsys->CloseDirectory(dumps);

		if (log) {
			fclose(log);
		}

		rootconsole->ConsolePrint("Accelerator upload thread finished. (%d uploaded, %d failed)", count, failed);
	}

	void OnTerminate(IThreadHandle *pHandle, bool cancel) {
		rootconsole->ConsolePrint("Accelerator upload thread terminated. (canceled = %s)", (cancel ? "true" : "false"));
	}

#if defined _LINUX
	bool UploadSymbolFile(const google_breakpad::CodeModule *module) {
		auto debugFile = module->debug_file();
		if (debugFile[0] != '/') {
			return false;
		}

		printf(">>> Submitting %s\n", debugFile.c_str());

		auto debugFileDir = google_breakpad::DirName(debugFile);
		std::vector<string> debug_dirs{
			debugFileDir,
			debugFileDir + "/.debug",
			"/usr/lib/debug" + debugFileDir,
		};

		std::ostringstream outputStream;
		google_breakpad::DumpOptions options(ALL_SYMBOL_DATA, true);

		{
			StderrInhibitor stdrrInhibitor;

			if (!WriteSymbolFile(debugFile, debug_dirs, options, outputStream)) {
				outputStream.str("");
				outputStream.clear();

				// Try again without debug dirs.
				if (!WriteSymbolFile(debugFile, {}, options, outputStream)) {
					return false;
				}
			}
		}

		auto output = outputStream.str();
		// output = output.substr(0, output.find("\n"));
		// printf(">>> %s\n", output.c_str());

		IWebForm *form = webternet->CreateForm();
		form->AddString("symbol_file", output.c_str());

		MemoryDownloader data;
		IWebTransfer *xfer = webternet->CreateSession();
		xfer->SetFailOnHTTPError(true);

		const char *symbolUrl = g_pSM->GetCoreConfigValue("MinidumpSymbolUrl");
		if (!symbolUrl) symbolUrl = "http://crash.limetech.org/symbols/submit";

		bool symbolUploaded = xfer->PostAndDownload(symbolUrl, form, &data, NULL);

		if (!symbolUploaded) {
			printf(">>> Symbol upload failed: %s (%d)\n", xfer->LastErrorMessage(), xfer->LastErrorCode());
			return false;
		}

		int responseSize = data.GetSize();
		char *response = new char[responseSize + 1];
		strncpy(response, data.GetBuffer(), responseSize + 1);
		response[responseSize] = '\0';
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		printf(">>> Symbol upload complete: %s\n", response);
		delete[] response;

		return true;
	}
#endif

	bool PresubmitCrashDump(const char *path) {
		google_breakpad::ProcessState processState;
		google_breakpad::ProcessResult processResult;
		google_breakpad::MinidumpProcessor minidumpProcessor(nullptr, nullptr);

		{
			ClogInhibitor clogInhibitor;
			processResult = minidumpProcessor.Process(path, &processState);
		}

		if (processResult != google_breakpad::PROCESS_OK) {
			return false;
		}

		int requestingThread = processState.requesting_thread();
		if (requestingThread == -1) {
			requestingThread = 0;
		}

		const google_breakpad::CallStack *stack = processState.threads()->at(requestingThread);
		if (!stack) {
			return false;
		}

		int frameCount = stack->frames()->size();
		/*if (frameCount > 10) {
			frameCount = 10;
		}*/

		std::ostringstream summaryStream;
		summaryStream << 1 << "|" << processState.crashed() << "|" << processState.crash_reason() << "|" << std::hex << processState.crash_address() << std::dec << "|" << requestingThread;

		std::map<const google_breakpad::CodeModule *, unsigned int> moduleMap;

		unsigned int moduleCount = processState.modules()->module_count();
		for (unsigned int moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex) {
			auto module = processState.modules()->GetModuleAtIndex(moduleIndex);
			moduleMap[module] = moduleIndex;

			auto debugFile = google_breakpad::PathnameStripper::File(module->debug_file());
			auto debugIdentifier = module->debug_identifier();

			summaryStream << "|M|" << debugFile << "|" << debugIdentifier;
		}

		for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
			auto frame = stack->frames()->at(frameIndex);

			int moduleIndex = -1;
			auto moduleOffset = frame->ReturnAddress();
			if (frame->module) {
				moduleIndex = moduleMap[frame->module];
				moduleOffset -= frame->module->base_address();
			}

			summaryStream << "|F|" << moduleIndex << "|" << std::hex << moduleOffset << std::dec;
		}

		auto summaryLine = summaryStream.str();
		// printf("%s\n", summaryLine.c_str());

		IWebForm *form = webternet->CreateForm();

		const char *minidumpAccount = g_pSM->GetCoreConfigValue("MinidumpAccount");
		if (minidumpAccount) form->AddString("UserID", minidumpAccount);

		form->AddString("CrashSignature", summaryLine.c_str());

		MemoryDownloader data;
		IWebTransfer *xfer = webternet->CreateSession();
		xfer->SetFailOnHTTPError(true);

		const char *minidumpUrl = g_pSM->GetCoreConfigValue("MinidumpUrl");
		if (!minidumpUrl) minidumpUrl = "http://crash.limetech.org/submit";

		bool uploaded = xfer->PostAndDownload(minidumpUrl, form, &data, NULL);

		if (!uploaded) {
			printf(">>> Presubmit failed: %s (%d)\n", xfer->LastErrorMessage(), xfer->LastErrorCode());
			return false;
		}

		int responseSize = data.GetSize();
		char *response = new char[responseSize + 1];
		strncpy(response, data.GetBuffer(), responseSize + 1);
		response[responseSize] = '\0';
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		printf(">>> Presubmit complete: %s\n", response);

		if (responseSize < 2) {
			printf(">>> Response too short\n");
			delete[] response;
			return false;
		}

		if (response[0] == 'E') {
			printf(">>> Presubmit error: %s\n", &response[2]);
			delete[] response;
			return false;
		}

		bool submitCrash = (response[0] == 'Y');

		if (response[1] != '|') {
			printf(">>> Response delimiter missing\n");
			delete[] response;
			return false;
		}

		unsigned int responseCount = responseSize - 2;
		if (responseCount < moduleCount) {
			printf(">>> Response module list doesn't match sent list (%d < %d)\n", responseCount, moduleCount);
			delete[] response;
			return false;
		}

#if defined _LINUX
		for (unsigned int moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex) {
			bool submitModule = (response[2 + moduleIndex] == 'Y');
			if (!submitModule) {
				continue;
			}

			auto module = processState.modules()->GetModuleAtIndex(moduleIndex);
			UploadSymbolFile(module);
		}
#else
		printf(">>> Symbol submission not available on this platform\n");
#endif

		delete[] response;
		return submitCrash;
	}

	bool UploadAndDeleteCrashDump(const char *path, char *response, int maxlen) {
		IWebForm *form = webternet->CreateForm();

		const char *minidumpAccount = g_pSM->GetCoreConfigValue("MinidumpAccount");
		if (minidumpAccount) form->AddString("UserID", minidumpAccount);

		form->AddString("GameDirectory", g_pSM->GetGameFolderName());
		form->AddString("ExtensionVersion", SMEXT_CONF_VERSION);

		form->AddFile("upload_file_minidump", path);

		char metapath[512];
		g_pSM->Format(metapath, sizeof(metapath), "%s.txt", path);
		if (libsys->PathExists(metapath)) {
			form->AddFile("upload_file_metadata", metapath);
		}

		MemoryDownloader data;
		IWebTransfer *xfer = webternet->CreateSession();
		xfer->SetFailOnHTTPError(true);

		const char *minidumpUrl = g_pSM->GetCoreConfigValue("MinidumpUrl");
		if (!minidumpUrl) minidumpUrl = "http://crash.limetech.org/submit";

		bool uploaded = xfer->PostAndDownload(minidumpUrl, form, &data, NULL);

		if (response) {
			if (uploaded) {
				int responseSize = data.GetSize();
				if (responseSize >= maxlen) responseSize = maxlen - 1;
				strncpy(response, data.GetBuffer(), responseSize);
				response[responseSize] = '\0';
				while (responseSize > 0 && response[responseSize - 1] == '\n') {
					response[--responseSize] = '\0';
				}
			} else {
				g_pSM->Format(response, maxlen, "%s (%d)", xfer->LastErrorMessage(), xfer->LastErrorCode());
			}
		}

		if (libsys->PathExists(metapath)) {
			unlink(metapath);
		}

		unlink(path);

		return uploaded;
	}
} uploadThread;

class VFuncEmptyClass {};

const char *GetCmdLine()
{
	static int getCmdLineOffset = 0;
	if (getCmdLineOffset == 0) {
		if (!gameconfig || !gameconfig->GetOffset("GetCmdLine", &getCmdLineOffset)) {
			return "";
		}
		if (getCmdLineOffset == 0) {
			return "";
		}
	}

	void *cmdline = gamehelpers->GetValveCommandLine();
	void **vtable = *(void ***)cmdline;
	void *vfunc = vtable[getCmdLineOffset];

	union {
		const char *(VFuncEmptyClass::*mfpnew)();
#ifndef WIN32
		struct {
			void *addr;
			intptr_t adjustor;
		} s;
	} u;
	u.s.addr = vfunc;
	u.s.adjustor = 0;
#else
		void *addr;
	} u;
	u.addr = vfunc;
#endif

	return (const char *)(reinterpret_cast<VFuncEmptyClass*>(cmdline)->*u.mfpnew)();
}

bool Accelerator::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	sharesys->AddDependency(myself, "webternet.ext", true, true);
	SM_GET_IFACE(WEBTERNET, webternet);

	g_pSM->BuildPath(Path_SM, dumpStoragePath, sizeof(dumpStoragePath), "data/dumps");

	if (!libsys->IsPathDirectory(dumpStoragePath))
	{
		if (!libsys->CreateFolder(dumpStoragePath))
		{
			if (error)
				g_pSM->Format(error, maxlength, "%s didn't exist and we couldn't create it :(", dumpStoragePath);
			return false;
		}
	}

	g_pSM->BuildPath(Path_SM, logPath, sizeof(logPath), "logs/accelerator.log");

	threader->MakeThread(&uploadThread);

	do {
		char gameconfigError[256];
		if (!gameconfs->LoadGameConfigFile("accelerator.games", &gameconfig, gameconfigError, sizeof(gameconfigError))) {
			smutils->LogMessage(myself, "WARNING: Failed to load gamedata file, console output and command line will not be included in crash reports: %s", gameconfigError);
			break;
		}

		bool useFastcall = false;


#if defined _WINDOWS
		const char *fastcall = gameconfig->GetKeyValue("UseFastcall");
		if (fastcall && strcmp(fastcall, "yes") == 0) {
			useFastcall = true;
		}

		if (useFastcall && !gameconfig->GetMemSig("GetSpewFastcall", (void **)&GetSpewFastcall)) {
			smutils->LogMessage(myself, "WARNING: GetSpewFastcall not found in gamedata, console output will not be included in crash reports.");
			break;
		}
#endif

		if (!useFastcall && !gameconfig->GetMemSig("GetSpew", (void **)&GetSpew)) {
			smutils->LogMessage(myself, "WARNING: GetSpew not found in gamedata, console output will not be included in crash reports.");
			break;
		}

		if (!GetSpew
#if defined _WINDOWS
			&& !GetSpewFastcall
#endif
		) {
			smutils->LogMessage(myself, "WARNING: Sigscan for GetSpew failed, console output will not be included in crash reports.");
			break;
		}
	} while(false);

#if defined _LINUX
	google_breakpad::MinidumpDescriptor descriptor(dumpStoragePath);
	handler = new google_breakpad::ExceptionHandler(descriptor, NULL, dumpCallback, NULL, true, -1);

	struct sigaction oact;
	sigaction(SIGSEGV, NULL, &oact);
	SignalHandler = oact.sa_sigaction;

	g_pSM->AddGameFrameHook(OnGameFrame);
#elif defined _WINDOWS
	wchar_t *buf = new wchar_t[sizeof(dumpStoragePath)];
	size_t num_chars = mbstowcs(buf, dumpStoragePath, sizeof(dumpStoragePath));

	handler = new google_breakpad::ExceptionHandler(std::wstring(buf, num_chars), NULL, dumpCallback, NULL, google_breakpad::ExceptionHandler::HANDLER_ALL);

	vectoredHandler = AddVectoredExceptionHandler(0, BreakpadVectoredHandler);

	delete buf;
#else
#error Bad platform.
#endif

#if 0
	IPluginIterator *i = plsys->GetPluginIterator();
	while (i->MorePlugins()) {
		IPlugin *p = i->GetPlugin();
		const sm_plugininfo_t *pmi = p->GetPublicInfo();
		PluginInfo *pi = &plugins[plugin_count++];

		pi->serial = p->GetSerial();
		pi->status = p->GetStatus();

		strncpy(pi->filename, p->GetFilename(), sizeof(pi->filename) - 1);

		strncpy(pi->name, pmi->name, sizeof(pi->name) - 1);
		strncpy(pi->author, pmi->author, sizeof(pi->author) - 1);
		strncpy(pi->description, pmi->description, sizeof(pi->description) - 1);
		strncpy(pi->version, pmi->version, sizeof(pi->version) - 1);
		strncpy(pi->url, pmi->url, sizeof(pi->url) - 1);

		i->NextPlugin();
	}
	delete i;
#endif

	strncpy(crashGamePath, g_pSM->GetGamePath(), sizeof(crashGamePath) - 1);
	strncpy(crashCommandLine, GetCmdLine(), sizeof(crashCommandLine) - 1);
	strncpy(crashSourceModPath, g_pSM->GetSourceModPath(), sizeof(crashSourceModPath) - 1);
	strncpy(crashGameDirectory, g_pSM->GetGameFolderName(), sizeof(crashGameDirectory) - 1);

	char steamInfPath[512];
	g_pSM->BuildPath(Path_Game, steamInfPath, sizeof(steamInfPath), "steam.inf");

	FILE *steamInfFile = fopen(steamInfPath, "rb");
	if (steamInfFile) {
		char steamInfTemp[1024] = {0};
		fread(steamInfTemp, sizeof(char), sizeof(steamInfTemp) - 1, steamInfFile);

		fclose(steamInfFile);

		unsigned commentChars = 0;
		unsigned valueChars = 0;
		unsigned source = 0;
		strcpy(steamInf, "\nSteam_");
		unsigned target = 7; // strlen("\nSteam_");
		while (true) {
			if (steamInfTemp[source] == '\0') {
				source++;
				break;
			}
			if (steamInfTemp[source] == '/') {
				source++;
				commentChars++;
				continue;
			}
			if (commentChars == 1) {
				commentChars = 0;
				steamInf[target++] = '/';
				valueChars++;
			}
			if (steamInfTemp[source] == '\r') {
				source++;
				continue;
			}
			if (steamInfTemp[source] == '\n') {
				commentChars = 0;
				source++;
				if (steamInfTemp[source] == '\0') {
					break;
				}
				if (valueChars > 0) {
					valueChars = 0;
					strcpy(&steamInf[target], "\nSteam_");
					target += 7;
				}
				continue;
			}
			if (commentChars >= 2) {
				source++;
				continue;
			}
			steamInf[target++] = steamInfTemp[source++];
			valueChars++;
		}
	}

	if (late) {
		this->OnCoreMapStart(NULL, 0, 0);
	}

	return true;
}

void Accelerator::SDK_OnUnload()
{
#if defined _LINUX
	g_pSM->RemoveGameFrameHook(OnGameFrame);
#elif defined _WINDOWS
	if (vectoredHandler) {
		RemoveVectoredExceptionHandler(vectoredHandler);
	}
#else
#error Bad platform.
#endif

	delete handler;
}

void Accelerator::OnCoreMapStart(edict_t *pEdictList, int edictCount, int clientMax)
{
	strncpy(crashMap, gamehelpers->GetCurrentMap(), sizeof(crashMap) - 1);
}
