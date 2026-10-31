#include "panplay/version.hpp"

#ifndef PANPLAY_VERSION
#define PANPLAY_VERSION "开发版"
#endif

namespace panplay {

const char* app_version() { return PANPLAY_VERSION; }

}  // namespace panplay
