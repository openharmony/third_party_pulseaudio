/*
 * Copyright (c) 2025 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "securec.h"
#include "resampleLoader.h"
#define LD_ABS_PATH_LEN 50
#if (defined(__aarch64__) || defined(__x86_64__))
    static char absolutePath[LD_ABS_PATH_LEN] = "/system/lib64/";
#else
    static char absolutePath[LD_ABS_PATH_LEN] = "/system/lib/";
#endif
// TO DO: read ProResampler library name from system config file
static char *libProResamplerName = "libaudio_proresampler.z.so";
bool LoadProResampler(int (**func_ptr_addr)(pa_resampler *r))
{
    if(*func_ptr_addr != NULL) {
        AUDIO_INFO_LOG("ProResampler has already been loaded!");
        return true;
    }
    if (strcat_s(absolutePath, LD_ABS_PATH_LEN, libProResamplerName) != 0) {
        AUDIO_ERR_LOG("LoadProResampler: strcat_s failed!");
        return false;
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