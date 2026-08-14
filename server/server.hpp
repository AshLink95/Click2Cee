#include <string>
///ISO 8601 format
struct log_entry {
    int time_stamp;
    bool status;
    std::string message;
};

enum snd_typ { tcp, udp };

/// send a single screen frame, through the chosen send type
log_entry snd(snd_typ typ);

/// listen for the next single input
log_entry rcv();
