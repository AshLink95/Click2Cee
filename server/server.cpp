#include "server.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>

static std::atomic<bool> run{true};
static void stop(int) { run = false; }

static std::string env(const std::string& key, const std::string& fallback) {
    for (const char* path : {".env", "../.env"}) {
        std::ifstream f(path);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos || line.substr(0, eq) != key) continue;
            std::string v = line.substr(eq + 1);
            if (!v.empty() && v.back() == '\r') v.pop_back();
            return v;
        }
    }
    return fallback;
}

log_entry init_network(std::string saddr, uint16_t port_snd, uint16_t port_rcv,
                       std::string caddr, uint16_t port_play) {
    log_entry snd = init_network_snd(saddr, port_snd, caddr, port_play);
    if (!snd.status) return snd;
    log_entry rcv = init_network_rcv(saddr, port_rcv, caddr);
    if (!rcv.status) return rcv;
    return log_entry(true, "network up");
}
log_entry cleanup_network() {
    log_entry snd = cleanup_network_snd();
    log_entry rcv = cleanup_network_rcv();
    if (!snd.status) return snd;
    if (!rcv.status) return rcv;
    return log_entry(true, "network down");
}

int main() {
    std::string host = env("SERVER_HOST", "127.0.0.1");
    std::string clnt = env("CLIENT_HOST", "127.0.0.1");
    uint16_t port_snd = std::stoi(env("VIDEO_PORT", "5000"));
    uint16_t port_rcv = std::stoi(env("INPUT_PORT", "5001"));
    uint16_t port_play = std::stoi(env("PLAYER_PORT", "5002"));
    log_entry cap_works = cap_init();
    cap_works.display(); if (!cap_works.status) return 1;
    log_entry net_works = init_network(host, port_snd, port_rcv, clnt, port_play);
    net_works.display(); if (!net_works.status) return 1;

    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    std::thread sx([&] {
        while (run) {
            log_entry le = snd();
            if (!le.status) { le.display(); run = false; }
        }
    });
    std::thread rx([&] {
        while (run) {
            log_entry le = rcv();
            if (!le.status && run) le.display();
        }
    });

    sx.join();
    cleanup_network().display();
    rx.join();
    cap_end().display();
    return 0;
}
