#include "resampleLoader.h"
// TO DO: read ProResampler library name from system config file
static const char *absolutePath = "PATH_TO_PROREAMPLER";
bool LoadProResampler(int (**func_ptr_addr)(pa_resampler *r))
{
    if(*func_ptr_addr != NULL) {
        AUDIO_INFO_LOG("ProResampler has already been loaded!");
        return true;
    }
    if (access(absolutePath, F_OK) != 0) {
        AUDIO_INFO_LOG("ProResampler does not exist! use SpeeX resampler!");
        return false;
    }
    void *handle = dlopen(absolutePath, 1);
    if (!handle) {
        AUDIO_ERR_LOG("dlopen lib ProResampler fail!, error: [%{public}s]", dlerror());
        return false;
    } else {
        AUDIO_INFO_LOG("dlopen lib ProResampler successful!");
    }

    *func_ptr_addr = (int (*)(pa_resampler *r))(dlsym(handle, ProResamplerInit_SYM_AS_STR));
    if (*func_ptr_addr == NULL) {
        AUDIO_ERR_LOG("dlsym lib ProResampler failed! error: [%{public}s]", dlerror());
        return false;
    }
    AUDIO_INFO_LOG("dlsym lib ProResampler success!");
    return true;
}