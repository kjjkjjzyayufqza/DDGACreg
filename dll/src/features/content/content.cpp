#include "features/content/content.h"

#include <cstdio>
#include <string>

// [Content] BaseUrl names the local content server. Only its port is used:
// the NESYS service (features/nesys) sends every game API request, whatever
// NESiCA host the game addressed, to 127.0.0.1:<port>. The DLL does not open
// SQLite.

namespace {

constexpr int kDefaultPort = 8750;

std::wstring Trim(std::wstring text)
{
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == L' ' || text[begin] == L'\t' || text[begin] == L'\r'))
        ++begin;
    size_t end = text.size();
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t' || text[end - 1] == L'\r'))
        --end;
    return text.substr(begin, end - begin);
}

int PortFromBaseUrl(const std::wstring& raw)
{
    std::wstring url = Trim(raw);
    const size_t scheme = url.find(L"://");
    std::wstring rest = scheme == std::wstring::npos ? url : url.substr(scheme + 3);
    const size_t slash = rest.find(L'/');
    if (slash != std::wstring::npos)
        rest.resize(slash);
    const size_t colon = rest.rfind(L':');
    if (colon == std::wstring::npos || rest.find(L':') != colon)
        return kDefaultPort;
    int port = 0;
    for (size_t i = colon + 1; i < rest.size(); ++i) {
        if (rest[i] < L'0' || rest[i] > L'9')
            return kDefaultPort;
        port = port * 10 + (rest[i] - L'0');
        if (port > 65535)
            return kDefaultPort;
    }
    return port > 0 ? port : kDefaultPort;
}

std::wstring ReadBaseUrl(const wchar_t* moduleDir)
{
    if (!moduleDir || !moduleDir[0])
        return L"http://127.0.0.1:8750";
    std::wstring path = moduleDir;
    path += L"\\config.ini";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return L"http://127.0.0.1:8750";
    std::string bytes;
    char buf[512];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), file)) > 0)
        bytes.append(buf, n);
    fclose(file);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF)
        bytes.erase(0, 3);

    bool inContent = false;
    size_t i = 0;
    while (i < bytes.size()) {
        size_t end = bytes.find('\n', i);
        if (end == std::string::npos)
            end = bytes.size();
        std::string line = bytes.substr(i, end - i);
        i = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
            ++start;
        line = line.substr(start);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            inContent = line == "[Content]";
            continue;
        }
        if (!inContent)
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        if (key != "BaseUrl")
            continue;
        std::string value = line.substr(eq + 1);
        size_t v = 0;
        while (v < value.size() && (value[v] == ' ' || value[v] == '\t'))
            ++v;
        value = value.substr(v);
        return std::wstring(value.begin(), value.end());
    }
    return L"http://127.0.0.1:8750";
}

} // namespace

namespace features {

int Content_Port(const wchar_t* moduleDir)
{
    return PortFromBaseUrl(ReadBaseUrl(moduleDir));
}

} // namespace features
