#include <dlfcn.h>
#include <unistd.h>
#include "log/audio_log.h"
#include "config.h"
#include "resampler.h"

#define ProResamplerInit_SYM_AS_STR "ProResamplerInit"
bool LoadProResampler(int (**func_ptr)(pa_resampler *r));