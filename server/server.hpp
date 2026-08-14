#include <cstdint>
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

enum snd_typ { tcp, udp };

/// send a single screen frame, through the chosen send type
log_entry snd(snd_typ typ);

/// listen for the next single input
log_entry rcv();
