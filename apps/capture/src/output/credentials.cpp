#include "output/credentials.hpp"

#include <windows.h>
#include <wincred.h>

#include <vector>

namespace ncap::secrets {

namespace {
std::wstring target(const std::string& name) {
    std::string t = "NorthRayCapture/" + name;
    return std::wstring(t.begin(), t.end());
}
}  // namespace

bool save(const std::string& name, const std::string& secret) {
    std::wstring t = target(name);
    std::vector<BYTE> blob(secret.begin(), secret.end());
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = t.data();
    c.CredentialBlobSize = (DWORD)blob.size();
    c.CredentialBlob = blob.data();
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;   // this user, this PC (not roamed)
    wchar_t user[] = L"NorthRay Capture";
    c.UserName = user;
    const bool ok = CredWriteW(&c, 0) != FALSE;
    SecureZeroMemory(blob.data(), blob.size());
    return ok;
}

std::string load(const std::string& name) {
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target(name).c_str(), CRED_TYPE_GENERIC, 0, &c) || !c) return {};
    std::string s(reinterpret_cast<const char*>(c->CredentialBlob), c->CredentialBlobSize);
    CredFree(c);
    return s;
}

bool exists(const std::string& name) {
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target(name).c_str(), CRED_TYPE_GENERIC, 0, &c) || !c) return false;
    CredFree(c);
    return true;
}

bool remove(const std::string& name) { return CredDeleteW(target(name).c_str(), CRED_TYPE_GENERIC, 0) != FALSE; }

}  // namespace ncap::secrets
