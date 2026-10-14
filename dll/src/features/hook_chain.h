#pragma once

// MinHook allows one detour per address. Quality, stream, and HUD share
// IConsoleVariable::Set and APlayerCameraManager::UpdateCamera, so the
// feature that creates the hook calls the policy registered by the other.

namespace hookchain {

// Return a replacement string, or nullptr to keep the incoming value.
// The pointer must remain valid after the call.
using SetRewrite = const wchar_t* (*)(void* self, const wchar_t* value);

void SetOwnedIntRewrite(SetRewrite rewrite); // RVA 0x472200
void SetIntRefRewrite(SetRewrite rewrite);   // RVA 0x4723F0

const wchar_t* RewriteOwnedInt(void* self, const wchar_t* value);
const wchar_t* RewriteIntRef(void* self, const wchar_t* value);

using CameraTick = void (*)();
void SetCameraTick(CameraTick tick);
void RunCameraTick();

}
