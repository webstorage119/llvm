//===- Unix/Process.cpp - Unix Process Implementation --------- -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the generic Unix implementation of the Process class.
//
//===----------------------------------------------------------------------===//

#include "Unix.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/MutexGuard.h"
#include "llvm/Support/TimeValue.h"
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
// DragonFlyBSD, OpenBSD, and Bitrig have deprecated <malloc.h> for
// <stdlib.h> instead. Unix.h includes this for us already.
#if defined(HAVE_MALLOC_H) && !defined(__DragonFly__) && \
    !defined(__OpenBSD__) && !defined(__Bitrig__)
#include <malloc.h>
#endif
#ifdef HAVE_MALLOC_MALLOC_H
#include <malloc/malloc.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#  include <sys/ioctl.h>
#endif
#ifdef HAVE_TERMIOS_H
#  include <termios.h>
#endif

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only generic UNIX code that
//===          is guaranteed to work on *all* UNIX variants.
//===----------------------------------------------------------------------===//

using namespace llvm;
using namespace sys;


process::id_type self_process::get_id() {
  return getpid();
}

static std::pair<TimeValue, TimeValue> getRUsageTimes() {
#if defined(HAVE_GETRUSAGE)
  struct rusage RU;
  ::getrusage(RUSAGE_SELF, &RU);
  return std::make_pair(
      TimeValue(
          static_cast<TimeValue::SecondsType>(RU.ru_utime.tv_sec),
          static_cast<TimeValue::NanoSecondsType>(
              RU.ru_utime.tv_usec * TimeValue::NANOSECONDS_PER_MICROSECOND)),
      TimeValue(
          static_cast<TimeValue::SecondsType>(RU.ru_stime.tv_sec),
          static_cast<TimeValue::NanoSecondsType>(
              RU.ru_stime.tv_usec * TimeValue::NANOSECONDS_PER_MICROSECOND)));
#else
#warning Cannot get usage times on this platform
  return std::make_pair(TimeValue(), TimeValue());
#endif
}

TimeValue self_process::get_user_time() const {
#if _POSIX_TIMERS > 0 && _POSIX_CPUTIME > 0
  // Try to get a high resolution CPU timer.
  struct timespec TS;
  if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &TS) == 0)
    return TimeValue(static_cast<TimeValue::SecondsType>(TS.tv_sec),
                     static_cast<TimeValue::NanoSecondsType>(TS.tv_nsec));
#endif

  // Otherwise fall back to rusage based timing.
  return getRUsageTimes().first;
}

TimeValue self_process::get_system_time() const {
  // We can only collect system time by inspecting the results of getrusage.
  return getRUsageTimes().second;
}

// On Cygwin, getpagesize() returns 64k and offset in mmap(3) should be
// aligned to its pagesize.
static unsigned getPageSize() {
#if defined(HAVE_GETPAGESIZE)
  const int page_size = ::getpagesize();
#elif defined(HAVE_SYSCONF)
  long page_size = ::sysconf(_SC_PAGE_SIZE);
#else
#warning Cannot get the page size on this machine
#endif
  return static_cast<unsigned>(page_size);
}

// This constructor guaranteed to be run exactly once on a single thread, and
// sets up various process invariants that can be queried cheaply from then on.
self_process::self_process() : PageSize(getPageSize()) {
}


size_t Process::GetMallocUsage() {
#if defined(HAVE_MALLINFO)
  struct mallinfo mi;
  mi = ::mallinfo();
  return mi.uordblks;
#elif defined(HAVE_MALLOC_ZONE_STATISTICS) && defined(HAVE_MALLOC_MALLOC_H)
  malloc_statistics_t Stats;
  malloc_zone_statistics(malloc_default_zone(), &Stats);
  return Stats.size_in_use;   // darwin
#elif defined(HAVE_SBRK)
  // Note this is only an approximation and more closely resembles
  // the value returned by mallinfo in the arena field.
  static char *StartOfMemory = reinterpret_cast<char*>(::sbrk(0));
  char *EndOfMemory = (char*)sbrk(0);
  if (EndOfMemory != ((char*)-1) && StartOfMemory != ((char*)-1))
    return EndOfMemory - StartOfMemory;
  else
    return 0;
#else
#warning Cannot get malloc info on this platform
  return 0;
#endif
}

void Process::GetTimeUsage(TimeValue &elapsed, TimeValue &user_time,
                           TimeValue &sys_time) {
  elapsed = TimeValue::now();
  llvm::tie(user_time, sys_time) = getRUsageTimes();
}

#if defined(HAVE_MACH_MACH_H) && !defined(__GNU__)
#include <mach/mach.h>
#endif

// Some LLVM programs such as bugpoint produce core files as a normal part of
// their operation. To prevent the disk from filling up, this function
// does what's necessary to prevent their generation.
void Process::PreventCoreFiles() {
#if HAVE_SETRLIMIT
  struct rlimit rlim;
  rlim.rlim_cur = rlim.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &rlim);
#endif

#if defined(HAVE_MACH_MACH_H) && !defined(__GNU__)
  // Disable crash reporting on Mac OS X 10.0-10.4

  // get information about the original set of exception ports for the task
  mach_msg_type_number_t Count = 0;
  exception_mask_t OriginalMasks[EXC_TYPES_COUNT];
  exception_port_t OriginalPorts[EXC_TYPES_COUNT];
  exception_behavior_t OriginalBehaviors[EXC_TYPES_COUNT];
  thread_state_flavor_t OriginalFlavors[EXC_TYPES_COUNT];
  kern_return_t err =
    task_get_exception_ports(mach_task_self(), EXC_MASK_ALL, OriginalMasks,
                             &Count, OriginalPorts, OriginalBehaviors,
                             OriginalFlavors);
  if (err == KERN_SUCCESS) {
    // replace each with MACH_PORT_NULL.
    for (unsigned i = 0; i != Count; ++i)
      task_set_exception_ports(mach_task_self(), OriginalMasks[i],
                               MACH_PORT_NULL, OriginalBehaviors[i],
                               OriginalFlavors[i]);
  }

  // Disable crash reporting on Mac OS X 10.5
  signal(SIGABRT, _exit);
  signal(SIGILL,  _exit);
  signal(SIGFPE,  _exit);
  signal(SIGSEGV, _exit);
  signal(SIGBUS,  _exit);
#endif
}

bool Process::StandardInIsUserInput() {
  return FileDescriptorIsDisplayed(STDIN_FILENO);
}

bool Process::StandardOutIsDisplayed() {
  return FileDescriptorIsDisplayed(STDOUT_FILENO);
}

bool Process::StandardErrIsDisplayed() {
  return FileDescriptorIsDisplayed(STDERR_FILENO);
}

bool Process::FileDescriptorIsDisplayed(int fd) {
#if HAVE_ISATTY
  return isatty(fd);
#else
  // If we don't have isatty, just return false.
  return false;
#endif
}

static unsigned getColumns(int FileID) {
  // If COLUMNS is defined in the environment, wrap to that many columns.
  if (const char *ColumnsStr = std::getenv("COLUMNS")) {
    int Columns = std::atoi(ColumnsStr);
    if (Columns > 0)
      return Columns;
  }

  unsigned Columns = 0;

#if defined(HAVE_SYS_IOCTL_H) && defined(HAVE_TERMIOS_H)
  // Try to determine the width of the terminal.
  struct winsize ws;
  if (ioctl(FileID, TIOCGWINSZ, &ws) == 0)
    Columns = ws.ws_col;
#endif

  return Columns;
}

unsigned Process::StandardOutColumns() {
  if (!StandardOutIsDisplayed())
    return 0;

  return getColumns(1);
}

unsigned Process::StandardErrColumns() {
  if (!StandardErrIsDisplayed())
    return 0;

  return getColumns(2);
}

#ifdef HAVE_TERMINFO
// We manually declare these extern functions because finding the correct
// headers from various terminfo, curses, or other sources is harder than
// writing their specs down.
extern "C" int setupterm(char *term, int filedes, int *errret);
extern "C" struct term *set_curterm(struct term *termp);
extern "C" int del_curterm(struct term *termp);
extern "C" int tigetnum(char *capname);
#endif

static bool terminalHasColors(int fd) {
#ifdef HAVE_TERMINFO
  // First, acquire a global lock because these C routines are thread hostile.
  static sys::Mutex M;
  MutexGuard G(M);

  int errret = 0;
  if (setupterm((char *)0, fd, &errret) != 0)
    // Regardless of why, if we can't get terminfo, we shouldn't try to print
    // colors.
    return false;

  // Test whether the terminal as set up supports color output. How to do this
  // isn't entirely obvious. We can use the curses routine 'has_colors' but it
  // would be nice to avoid a dependency on curses proper when we can make do
  // with a minimal terminfo parsing library. Also, we don't really care whether
  // the terminal supports the curses-specific color changing routines, merely
  // if it will interpret ANSI color escape codes in a reasonable way. Thus, the
  // strategy here is just to query the baseline colors capability and if it
  // supports colors at all to assume it will translate the escape codes into
  // whatever range of colors it does support. We can add more detailed tests
  // here if users report them as necessary.
  //
  // The 'tigetnum' routine returns -2 or -1 on errors, and might return 0 if
  // the terminfo says that no colors are supported.
  bool HasColors = tigetnum(const_cast<char *>("colors")) > 0;

  // Now extract the structure allocated by setupterm and free its memory
  // through a really silly dance.
  struct term *termp = set_curterm((struct term *)0);
  (void)del_curterm(termp); // Drop any errors here.

  // Return true if we found a color capabilities for the current terminal.
  if (HasColors)
    return true;
#endif

  // Otherwise, be conservative.
  return false;
}

bool Process::FileDescriptorHasColors(int fd) {
  // A file descriptor has colors if it is displayed and the terminal has
  // colors.
  return FileDescriptorIsDisplayed(fd) && terminalHasColors(fd);
}

bool Process::StandardOutHasColors() {
  return FileDescriptorHasColors(STDOUT_FILENO);
}

bool Process::StandardErrHasColors() {
  return FileDescriptorHasColors(STDERR_FILENO);
}

bool Process::ColorNeedsFlush() {
  // No, we use ANSI escape sequences.
  return false;
}

#define COLOR(FGBG, CODE, BOLD) "\033[0;" BOLD FGBG CODE "m"

#define ALLCOLORS(FGBG,BOLD) {\
    COLOR(FGBG, "0", BOLD),\
    COLOR(FGBG, "1", BOLD),\
    COLOR(FGBG, "2", BOLD),\
    COLOR(FGBG, "3", BOLD),\
    COLOR(FGBG, "4", BOLD),\
    COLOR(FGBG, "5", BOLD),\
    COLOR(FGBG, "6", BOLD),\
    COLOR(FGBG, "7", BOLD)\
  }

static const char colorcodes[2][2][8][10] = {
 { ALLCOLORS("3",""), ALLCOLORS("3","1;") },
 { ALLCOLORS("4",""), ALLCOLORS("4","1;") }
};

const char *Process::OutputColor(char code, bool bold, bool bg) {
  return colorcodes[bg?1:0][bold?1:0][code&7];
}

const char *Process::OutputBold(bool bg) {
  return "\033[1m";
}

const char *Process::OutputReverse() {
  return "\033[7m";
}

const char *Process::ResetColor() {
  return "\033[0m";
}

#if !defined(HAVE_ARC4RANDOM)
static unsigned GetRandomNumberSeed() {
  // Attempt to get the initial seed from /dev/urandom, if possible.
  if (FILE *RandomSource = ::fopen("/dev/urandom", "r")) {
    unsigned seed;
    int count = ::fread((void *)&seed, sizeof(seed), 1, RandomSource);
    ::fclose(RandomSource);

    // Return the seed if the read was successful.
    if (count == 1)
      return seed;
  }

  // Otherwise, swizzle the current time and the process ID to form a reasonable
  // seed.
  TimeValue Now = TimeValue::now();
  return hash_combine(Now.seconds(), Now.nanoseconds(), ::getpid());
}
#endif

unsigned llvm::sys::Process::GetRandomNumber() {
#if defined(HAVE_ARC4RANDOM)
  return arc4random();
#else
  static int x = (::srand(GetRandomNumberSeed()), 0);
  (void)x;
  return ::rand();
#endif
}
