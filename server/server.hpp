#include <string>
#include <chrono>
#include <cstdio>

///ISO 8601 format
struct log_entry {
    int64_t time_stamp;
    bool status;
    std::string message;
    log_entry(bool status, std::string message) : status(status), message(message) {
        this->time_stamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
    void display() {
        std::chrono::sys_seconds tp{std::chrono::seconds{time_stamp}};
        std::string ts = std::format("{:%FT%TZ}", tp);
        const char* color = status ? "\033[32m" : "\033[31m";
        const char* label = status ? "SUCCESS" : "FAILURE";
        printf("%s %s%s\033[0m %s\n", ts.c_str(), color, label, message.c_str());
    };
};

log_entry cap_init();
log_entry cap_end();

log_entry init_network(std::string saddr, uint16_t port_snd, uint16_t port_rcv,
                       std::string caddr, uint16_t port_play);
log_entry init_network_snd(std::string addr, uint16_t port, std::string caddr, uint16_t port_play);
log_entry init_network_rcv(std::string saddr, uint16_t port, std::string caddr);

log_entry cleanup_network();
log_entry cleanup_network_snd();
log_entry cleanup_network_rcv();

/// capture, encode and send a single screen frame
log_entry snd();

/// listen for the next single input
log_entry rcv();
