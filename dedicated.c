#include "stdafx.h"
#include "ttd.h"
#include "debug.h"
#include "network.h"
#include "hal.h"

#ifdef ENABLE_NETWORK

#include "gfx.h"
#include "window.h"
#include "command.h"
#include "console.h"
#ifdef WIN32
#	include <windows.h> /* GetTickCount */
#	include <conio.h>
#endif

#ifdef __OS2__
#	include <sys/time.h> /* gettimeofday */
#	include <sys/types.h>
#	include <unistd.h>
#	include <conio.h>
#	define STDIN 0  /* file descriptor for standard input */

	extern void OS2_SwitchToConsoleMode();
#endif

#ifdef UNIX
#	include <sys/time.h> /* gettimeofday */
#	include <sys/types.h>
#	include <unistd.h>
#	include <signal.h>
#	define STDIN 0  /* file descriptor for standard input */
#endif
#ifdef __MORPHOS__
/* Voids the fork, option will be disabled for MorphOS build anyway, because
 * MorphOS doesn't support forking (could only implemented with lots of code
 * changes here). */
int fork(void) { return -1; }
int dup2(int oldd, int newd) { return -1; }
#endif

// This file handles all dedicated-server in- and outputs

static void *_dedicated_video_mem;

extern bool SafeSaveOrLoad(const char *filename, int mode, int newgm);
extern void SwitchMode(int new_mode);

#ifdef UNIX
/* We want to fork our dedicated server */
void DedicatedFork(void)
{
	/* Fork the program */
	pid_t pid = fork();
	switch (pid) {
		case -1:
			perror("Unable to fork");
			exit(1);
		case 0:
			// We're the child

			/* Open the log-file to log all stuff too */
			_log_file_fd = fopen(_log_file, "a");
			if (!_log_file_fd) {
				perror("Unable to open logfile");
				exit(1);
			}
			/* Redirect stdout and stderr to log-file */
			if (dup2(fileno(_log_file_fd), fileno(stdout)) == -1) {
				perror("Re-routing stdout");
				exit(1);
			}
			if (dup2(fileno(_log_file_fd), fileno(stderr)) == -1) {
				perror("Re-routing stderr");
				exit(1);
			}
			break;
		default:
			// We're the parent
			printf("Loading dedicated server...\n");
			printf("  - Forked to background with pid %d\n", pid);
			exit(0);
	}
}

/* Signal handlers */
static void DedicatedSignalHandler(int sig)
{
	_exit_game = true;
	signal(sig, DedicatedSignalHandler);
}
#endif

#ifdef WIN32
#include <time.h>
HANDLE hEvent;
static HANDLE hThread; // Thread to close
static char _win_console_thread_buffer[200];

/* Windows Console thread. Just loop and signal when input has been received */
void WINAPI CheckForConsoleInput(void)
{
	while (true) {
		fgets(_win_console_thread_buffer, lengthof(_win_console_thread_buffer), stdin);
		SetEvent(hEvent); // signal input waiting that the line is ready
	}
}

void CreateWindowsConsoleThread(void)
{
	static char tbuffer[9];
	/* Create event to signal when console input is ready */
	hEvent = CreateEvent(NULL, false, false, _strtime(tbuffer));
	if (hEvent == NULL)
		error("Cannot create console event!");

	hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)CheckForConsoleInput, 0, 0, NULL);
	if (hThread == NULL)
		error("Cannot create console thread!");

	DEBUG(misc, 0) ("Windows console thread started...");
}

void CloseWindowsConsoleThread(void)
{
	CloseHandle(hThread);
	CloseHandle(hEvent);
	DEBUG(misc, 0) ("Windows console thread shut down...");
}

#endif

static const char *DedicatedVideoStart(const char * const *parm)
{
	_screen.width = _screen.pitch = _cur_resolution[0];
	_screen.height = _cur_resolution[1];
	_dedicated_video_mem = malloc(_cur_resolution[0]*_cur_resolution[1]);

	_debug_net_level = 6;
	_debug_misc_level = 0;

#ifdef WIN32
	// For win32 we need to allocate an console (debug mode does the same)
	CreateConsole();
	CreateWindowsConsoleThread();
	SetConsoleTitle("OpenTTD Dedicated Server");
#endif

#ifdef __OS2__
	// For OS/2 we also need to switch to console mode instead of PM mode
	OS2_SwitchToConsoleMode();
#endif

	DEBUG(misc,0)("Loading dedicated server...");
	return NULL;
}

static void DedicatedVideoStop(void)
{
#ifdef WIN32
	CloseWindowsConsoleThread();
#endif
	free(_dedicated_video_mem);
}

static void DedicatedVideoMakeDirty(int left, int top, int width, int height) {}
static bool DedicatedVideoChangeRes(int w, int h) { return false; }

#if defined(UNIX) || defined(__OS2__)
static bool InputWaiting(void)
{
	struct timeval tv;
	fd_set readfds;
	byte ret;

	tv.tv_sec = 0;
	tv.tv_usec = 1;

	FD_ZERO(&readfds);
	FD_SET(STDIN, &readfds);

	/* don't care about writefds and exceptfds: */
	ret = select(STDIN + 1, &readfds, NULL, NULL, &tv);

	if (ret > 0)
		return true;

	return false;
}
#else
static bool InputWaiting(void)
{
	if (WaitForSingleObject(hEvent, 1) == WAIT_OBJECT_0)
		return true;

	return false;
}
#endif

static void DedicatedHandleKeyInput(void)
{
	static char input_line[200] = "";

	if (!InputWaiting())
		return;

	if (_exit_game)
		return;

#if defined(UNIX) || defined(__OS2__)
		fgets(input_line, lengthof(input_line), stdin);
#else
		strncpy(input_line, _win_console_thread_buffer, lengthof(input_line));
#endif

	/* XXX - strtok() does not 'forget' \n\r if it is the first character! */
	strtok(input_line, "\r\n"); // Forget about the final \n (or \r)
	{ /* Remove any special control characters */
		uint i;
		for (i = 0; i < lengthof(input_line); i++) {
			if (input_line[i] == '\n' || input_line[i] == '\r') // cut missed beginning '\0'
				input_line[i] = '\0';

			if (input_line[i] == '\0')
				break;

			if (!IS_INT_INSIDE(input_line[i], ' ', 256))
				input_line[i] = ' ';
		}
	}

	IConsoleCmdExec(input_line); // execute command
}

static int DedicatedVideoMainLoop(void)
{
#ifndef WIN32
	struct timeval tim;
#endif
	uint32 next_tick;
	uint32 cur_ticks;

#ifdef WIN32
	next_tick = GetTickCount() + 30;
#else
	gettimeofday(&tim, NULL);
	next_tick = (tim.tv_usec / 1000) + 30 + (tim.tv_sec * 1000);
#endif

	/* Signal handlers */
#ifdef UNIX
	signal(SIGTERM, DedicatedSignalHandler);
	signal(SIGINT, DedicatedSignalHandler);
	signal(SIGQUIT, DedicatedSignalHandler);
#endif

	// Load the dedicated server stuff
	_is_network_server = true;
	_network_dedicated = true;
	_network_playas = OWNER_SPECTATOR;
	_local_player = OWNER_SPECTATOR;

	/* If SwitchMode is SM_LOAD, it means that the user used the '-g' options */
	if (_switch_mode != SM_LOAD) {
		_switch_mode = SM_NONE;
		DoCommandP(0, Random(), InteractiveRandom(), NULL, CMD_GEN_RANDOM_NEW_GAME);
	} else {
		_switch_mode = SM_NONE;
		/* First we need to test if the savegame can be loaded, else we will end up playing the
		 *  intro game... */
		if (!SafeSaveOrLoad(_file_to_saveload.name, _file_to_saveload.mode, GM_NORMAL)) {
			/* Loading failed, pop out.. */
			DEBUG(net, 0)("Loading request map failed. Aborting..");
			_networking = false;
		} else {
			/* We can load this game, so go ahead */
			SwitchMode(SM_LOAD);
		}
	}

	// Done loading, start game!

	if (!_networking) {
		DEBUG(net, 1)("Dedicated server could not be launced. Aborting..");
		return ML_QUIT;
	}

	while (true) {
		InteractiveRandom(); // randomness

		if (_exit_game) return ML_QUIT;

		if (!_dedicated_forks)
			DedicatedHandleKeyInput();

#ifdef WIN32
		cur_ticks = GetTickCount();
#else
		gettimeofday(&tim, NULL);
		cur_ticks = (tim.tv_usec / 1000) + (tim.tv_sec * 1000);
#endif

		if (cur_ticks >= next_tick) {
			next_tick += 30;

			GameLoop();
			_screen.dst_ptr = _dedicated_video_mem;
			UpdateWindows();
		}
		CSleep(1);
	}

	return ML_QUIT;
}


const HalVideoDriver _dedicated_video_driver = {
	DedicatedVideoStart,
	DedicatedVideoStop,
	DedicatedVideoMakeDirty,
	DedicatedVideoMainLoop,
	DedicatedVideoChangeRes,
};

#else

static void *_dedicated_video_mem;

static const char *DedicatedVideoStart(const char * const *parm)
{
	DEBUG(misc, 0) ("OpenTTD compiled without network-support, exiting...");

	return NULL;
}

void DedicatedFork(void) {}
static void DedicatedVideoStop(void) { free(_dedicated_video_mem); }
static void DedicatedVideoMakeDirty(int left, int top, int width, int height) {}
static bool DedicatedVideoChangeRes(int w, int h) { return false; }
static int DedicatedVideoMainLoop(void) { return ML_QUIT; }

const HalVideoDriver _dedicated_video_driver = {
	DedicatedVideoStart,
	DedicatedVideoStop,
	DedicatedVideoMakeDirty,
	DedicatedVideoMainLoop,
	DedicatedVideoChangeRes,
};

#endif /* ENABLE_NETWORK */
