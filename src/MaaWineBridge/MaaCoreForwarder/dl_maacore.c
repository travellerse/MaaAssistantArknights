#include <dlfcn.h>
#define DL_FUNC_IMPL
#include "dl_maacore.h"

static int maacore_loaded = 0;

void dl_init_maacore()
{
    maacore_loaded = 0;
    void* maacore = dlopen("libMaaCore.so", RTLD_NOW);
    if (!maacore) {
        return;
    }

    int complete = 1;

#define FILL_REQUIRED(func)          \
    do {                             \
        FILL_DL_FUNC(maacore, func); \
        if (!dl_##func) {            \
            complete = 0;            \
        }                            \
    } while (0)

    FILL_REQUIRED(AsstSetUserDir);
    FILL_REQUIRED(AsstLoadResource);
    FILL_REQUIRED(AsstSetStaticOption);

    FILL_REQUIRED(AsstCreate);
    FILL_REQUIRED(AsstCreateEx);
    FILL_REQUIRED(AsstDestroy);

    FILL_REQUIRED(AsstSetInstanceOption);
    FILL_REQUIRED(AsstConnect);

    FILL_REQUIRED(AsstAppendTask);
    FILL_REQUIRED(AsstSetTaskParams);

    FILL_REQUIRED(AsstStart);
    FILL_REQUIRED(AsstStop);
    FILL_REQUIRED(AsstRunning);
    FILL_REQUIRED(AsstConnected);
    FILL_REQUIRED(AsstBackToHome);

    FILL_REQUIRED(AsstAsyncConnect);
    FILL_REQUIRED(AsstSetConnectionExtras);
    FILL_REQUIRED(AsstAsyncClick);
    FILL_REQUIRED(AsstAsyncScreencap);

    FILL_REQUIRED(AsstGetImage);
    FILL_REQUIRED(AsstGetImageBgr);
    FILL_REQUIRED(AsstGetUUID);
    FILL_REQUIRED(AsstGetTasksList);
    FILL_REQUIRED(AsstGetNullSize);

    FILL_REQUIRED(AsstGetVersion);
    FILL_REQUIRED(AsstLog);

#undef FILL_REQUIRED

    if (!complete) {
        dlclose(maacore);
        return;
    }
    maacore_loaded = 1;
}

#include <windef.h>

int dl_maacore_ready(void)
{
    return maacore_loaded;
}

int __cdecl dl_has_maacore()
{
    return maacore_loaded;
}
