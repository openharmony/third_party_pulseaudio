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
#include "parameter.h"
#include "securec.h"
#include "resampleLoader.h"
#define LD_ABS_PATH_LEN 80
#if (defined(__aarch64__) || defined(__x86_64__))
    static char absolutePath[LD_ABS_PATH_LEN] = "/system/lib64/";
#else
    static char absolutePath[LD_ABS_PATH_LEN] = "/system/lib/";
#endif
static char libProResamplerName[40] = {0};
bool LoadProResampler(int (**func_ptr_addr)(pa_resampler *r))
{
    CHECK_AND_RETURN_RET_LOG(*func_ptr_addr == NULL, true, "ProResampler has already been loaded!");

    int ret = GetParameter("const.multimedia.audio.lib_proresampler_name", "-1", libProResamplerName,
        sizeof(libProResamplerName));
    CHECK_AND_RETURN_RET_LOG(ret > 0, false, "LoadProResampler GetSysPara fail, use speeX!");

    ret = strcat_s(absolutePath, LD_ABS_PATH_LEN, libProResamplerName);
    CHECK_AND_RETURN_RET_LOG(ret == 0, false, "LoadProResampler: strcat_s failed!");

    ret = access(absolutePath, F_OK);
    CHECK_AND_RETURN_RET_LOG(ret == 0, false, "ProResampler does not exist! use SpeeX resampler!");

    void *handle = dlopen(absolutePath, 1);
    CHECK_AND_RETURN_RET_LOG(handle != NULL, false, "dlopen lib ProResampler fail!, error: [%{public}s]", dlerror());
    
    AUDIO_INFO_LOG("dlopen lib ProResampler successful!");

    *func_ptr_addr = (int (*)(pa_resampler *r))(dlsym(handle, ProResamplerInit_SYM_AS_STR));
    CHECK_AND_RETURN_RET_LOG(*func_ptr_addr != NULL, false, "dlsym lib ProResampler failed! error: [%{public}s]",
        dlerror());

    AUDIO_INFO_LOG("dlsym lib ProResampler success!");
    return true;
}