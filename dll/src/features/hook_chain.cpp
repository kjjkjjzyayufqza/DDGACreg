#include "features/hook_chain.h"

namespace hookchain {

namespace {

SetRewrite g_ownedInt = nullptr;
SetRewrite g_intRef = nullptr;
CameraTick g_cameraTick = nullptr;

}

void SetOwnedIntRewrite(SetRewrite rewrite)
{
    g_ownedInt = rewrite;
}

void SetIntRefRewrite(SetRewrite rewrite)
{
    g_intRef = rewrite;
}

const wchar_t* RewriteOwnedInt(void* self, const wchar_t* value)
{
    return g_ownedInt != nullptr ? g_ownedInt(self, value) : nullptr;
}

const wchar_t* RewriteIntRef(void* self, const wchar_t* value)
{
    return g_intRef != nullptr ? g_intRef(self, value) : nullptr;
}

void SetCameraTick(CameraTick tick)
{
    g_cameraTick = tick;
}

void RunCameraTick()
{
    if (g_cameraTick != nullptr)
        g_cameraTick();
}

}
