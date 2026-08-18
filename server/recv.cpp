#include "server.hpp"
#include <vector>
#include <algorithm>
#include <format>

// The client caps itself at 64 keys of 16 chars, so 1094 bytes is its longest
// packet. Rounded up, and anything larger is not ours to begin with.
#define MAX_IN 2048

//NOTE: might accept other inputs (exp: controllers)
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <unordered_map>

static std::vector<WORD> keyHeld;

std::vector<WORD> convert(const std::vector<std::string>& keys) {
    static const std::unordered_map<std::string, WORD> keyMap = {
        {"Escape", VK_ESCAPE}, {"Tab", VK_TAB}, {"CapsLock", VK_CAPITAL},
        {"Space", VK_SPACE}, {"Enter", VK_RETURN}, {"Backspace", VK_BACK},
        {"Delete", VK_DELETE}, {"Insert", VK_INSERT},
        {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
        {"ArrowLeft", VK_LEFT}, {"ArrowUp", VK_UP},
        {"ArrowRight", VK_RIGHT}, {"ArrowDown", VK_DOWN},
        {"ShiftLeft", VK_LSHIFT}, {"ShiftRight", VK_RSHIFT},
        {"ControlLeft", VK_LCONTROL}, {"ControlRight", VK_RCONTROL},
        {"AltLeft", VK_LMENU}, {"AltRight", VK_RMENU},
        {"MetaLeft", VK_LWIN}, {"MetaRight", VK_RWIN},
        {"Minus", VK_OEM_MINUS}, {"Equal", VK_OEM_PLUS},
        {"BracketLeft", VK_OEM_4}, {"BracketRight", VK_OEM_6}, {"Backslash", VK_OEM_5},
        {"Semicolon", VK_OEM_1}, {"Quote", VK_OEM_7}, {"Backquote", VK_OEM_3},
        {"Comma", VK_OEM_COMMA}, {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2},
    };

    std::vector<WORD> out;
    out.reserve(keys.size());
    for (const std::string& c : keys) {
        if (c.empty()) continue;
        // 'A'..'Z' and '0'..'9' are already their own virtual-key values
        if (c.size() == 4 && c.rfind("Key", 0) == 0)        out.push_back(c[3]);
        else if (c.size() == 6 && c.rfind("Digit", 0) == 0) out.push_back(c[5]);
        else if (c[0] == 'F' && c.size() <= 3 && isdigit((unsigned char)c[1]))
            out.push_back(VK_F1 + std::stoi(c.substr(1)) - 1);
        else {
            auto it = keyMap.find(c);
            out.push_back(it == keyMap.end() ? 0 : it->second); // 0 = unmapped
        }
    }
    return out;
}

static int butHeld = 0; // mouse button currently held, 0 for none

struct mbtn { DWORD down, up, data; };
static const std::unordered_map<int, mbtn> butMap = {
    {1, {MOUSEEVENTF_LEFTDOWN,   MOUSEEVENTF_LEFTUP,   0}},
    {2, {MOUSEEVENTF_RIGHTDOWN,  MOUSEEVENTF_RIGHTUP,  0}},
    {3, {MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0}},
    {4, {MOUSEEVENTF_XDOWN,      MOUSEEVENTF_XUP,      XBUTTON1}},
    {5, {MOUSEEVENTF_XDOWN,      MOUSEEVENTF_XUP,      XBUTTON2}},
};

bool click(int x, int y, int mbt) {
    std::vector<INPUT> ev;
    INPUT in = {0};
    in.type = INPUT_MOUSE;

    in.mi.dx = x;
    in.mi.dy = y;
    in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    ev.push_back(in);
    in.mi.dx = in.mi.dy = 0;

    if (mbt == 100 || mbt == 101) {
        in.mi.mouseData = (DWORD)(mbt == 100 ? WHEEL_DELTA : -WHEEL_DELTA);
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        ev.push_back(in);
    } else if (mbt != butHeld) {
        if (auto it = butMap.find(butHeld); it != butMap.end()) {
            in.mi.mouseData = it->second.data;
            in.mi.dwFlags = it->second.up;
            ev.push_back(in);
        }
        if (auto it = butMap.find(mbt); it != butMap.end()) {
            in.mi.mouseData = it->second.data;
            in.mi.dwFlags = it->second.down;
            ev.push_back(in);
        }
        butHeld = mbt;
    }

    return SendInput((UINT)ev.size(), ev.data(), sizeof(INPUT)) == ev.size();
}

bool press(const std::vector<std::string>& keys) {
    std::vector<WORD> want = convert(keys);
    want.erase(std::remove(want.begin(), want.end(), 0), want.end());

    std::vector<INPUT> ev;
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;

    for (WORD k : keyHeld) {
        if (std::find(want.begin(), want.end(), k) != want.end()) continue;
        in.ki.wVk = k;
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        ev.push_back(in);
    }
    for (WORD k : want) {
        if (std::find(keyHeld.begin(), keyHeld.end(), k) != keyHeld.end()) continue;
        in.ki.wVk = k;
        in.ki.dwFlags = 0;
        ev.push_back(in);
    }

    keyHeld = std::move(want);
    if (ev.empty()) return true;
    return SendInput((UINT)ev.size(), ev.data(), sizeof(INPUT)) == ev.size();
}

static struct {
    SOCKET   sock = INVALID_SOCKET;
    uint32_t peer = INADDR_ANY;
} in_net;

log_entry init_network_rcv(std::string saddr, uint16_t port, std::string caddr) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return log_entry(false, "WSAStartup failed");
    }

    in_net.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (in_net.sock == INVALID_SOCKET) {
        return log_entry(false, std::format("socket failed ({})", WSAGetLastError()));
    }

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(port);
    local.sin_addr.s_addr = inet_addr(saddr.c_str());
    if (local.sin_addr.s_addr == INADDR_NONE) {
        closesocket(in_net.sock);
        in_net.sock = INVALID_SOCKET;
        return log_entry(false, "bad address");
    }
    if (bind(in_net.sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        closesocket(in_net.sock);
        in_net.sock = INVALID_SOCKET;
        return log_entry(false, std::format("bind failed ({})", WSAGetLastError()));
    }

    // The address is all we can check, the port being ephemeral. 0.0.0.0 in
    // config resolves to INADDR_ANY here, which is how you ask for no filter.
    in_net.peer = inet_addr(caddr.c_str());
    if (in_net.peer == INADDR_NONE) {
        closesocket(in_net.sock);
        in_net.sock = INVALID_SOCKET;
        return log_entry(false, "bad client address");
    }

    // Inputs are small and frequent, and the socket stays blocking on purpose:
    // this runs on its own thread and should sleep until something arrives.
    int rcvbuf = 1 << 20;
    setsockopt(in_net.sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

    return log_entry(true, std::format("udp {}:{} from {}", saddr, port, caddr));
}

bool rcv_frame(std::vector<uint8_t>& in) {
    if (in_net.sock == INVALID_SOCKET) return false;

    uint8_t     pkt[MAX_IN];
    sockaddr_in from{};
    int         flen = sizeof(from);

    int n = recvfrom(in_net.sock, (char*)pkt, sizeof(pkt), 0, (sockaddr*)&from, &flen);
    if (n <= 0) return false;
    if (in_net.peer != INADDR_ANY && from.sin_addr.s_addr != in_net.peer) return false;

    in.assign(pkt, pkt + n);
    return true;
}

log_entry cleanup_network_rcv() {
    if (in_net.sock != INVALID_SOCKET) closesocket(in_net.sock);
    in_net.sock = INVALID_SOCKET;
    WSACleanup();
    return log_entry(true, "input socket closed");
}

#elif defined(__linux__)
//TODO: check equivalent API on linux

std::vector<char> convert(const std::vector<std::string>& keys) {
    return {};
}

bool click(int x, int y, int mbt) {
    return false;
}

bool press(const std::vector<std::string>& keys) {
    return false;
}

log_entry init_network_rcv(std::string saddr, uint16_t port, std::string caddr) {
    return log_entry(false, "linux path not implemented");
}

bool rcv_frame(std::vector<uint8_t>& in) {
    return false;
}

log_entry cleanup_network_rcv() {
    return log_entry(false, "linux path not implemented");
}

#endif

log_entry rcv() {
    std::vector<uint8_t> in;
    if (!rcv_frame(in)) return log_entry(false, "input recv failed");
    if (in.size() < 6) return log_entry(false, "input packet too short");

    size_t n   = in[0];
    int    btn = in[1];
    int    x   = (in[2] << 8) | in[3];
    int    y   = (in[4] << 8) | in[5];

    std::vector<std::string> keys;
    keys.reserve(n);
    size_t off = 6;
    for (size_t i = 0; i < n; ++i) {
        if (off >= in.size()) return log_entry(false, "truncated key list");
        size_t len = in[off];
        if (off + 1 + len > in.size()) return log_entry(false, "truncated key name");
        keys.emplace_back((const char*)&in[off + 1], len);
        off += 1 + len;
    }

    if (!press(keys)) return log_entry(false, "key inject failed");
    if (!click(x, y, btn)) return log_entry(false, "mouse inject failed");
    return log_entry(true, "input applied");
}
