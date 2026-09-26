#pragma once
//SemVer-style: bump MAJOR for breaking changes, MINOR for backward-compatible features, PATCH for
//bug fixes. Bumped manually at release time - not tied to build count or commit count.
#define VERSION_MAJOR 1
#define VERSION_MINOR 1
#define VERSION_PATCH 1
//Windows' FILEVERSION/PRODUCTVERSION resource fields (see ReplayClipTrainer.rc) require exactly 4
//numbers - this 4th one is fixed at 0 and isn't part of the version shown anywhere (see plugin_version
//in ReplayClipTrainer.h, which only uses MAJOR.MINOR.PATCH).
#define VERSION_BUILD 0

#define stringify(a) stringify_(a)
#define stringify_(a) #a

